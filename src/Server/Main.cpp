#include "Node/Node.h"
#include "Node/NodeClients/RPCNodeClient.h"
#include "Wallet/WalletDaemon.h"
#include "opt.h"
#include "console.h"

#include <GrinVersion.h>
#include <Core/Context.h>
#include <Core/Global.h>
#include <Wallet/WalletManager.h>
#include <Core/Config.h>
#include <Common/Logger.h>
#include <Common/Util/ThreadUtil.h>
#include <Common/Util/FileUtil.h>
#include <Common/Util/ScopedTiming.h>

#include <stdio.h> 
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

using namespace std::chrono;

ConfigPtr Initialize(const std::optional<fs::path>& config_path, const Environment environment);
void Run(const ConfigPtr& pConfig, const Options& options);

static fs::path ExecutableDirectory()
{
#ifdef _WIN32
	std::vector<wchar_t> buffer(32768);
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0 || static_cast<size_t>(length) >= buffer.size()) throw std::runtime_error("Cannot resolve executable path");
	return fs::path(std::wstring(buffer.data(), length)).parent_path();
#else
	std::vector<char> buffer(4096);
	const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
	if (length <= 0 || static_cast<size_t>(length) >= buffer.size()) throw std::runtime_error("Cannot resolve executable path");
	return fs::path(std::string(buffer.data(), static_cast<size_t>(length))).parent_path();
#endif
}

int main(int argc, char* argv[])
{
	LoggerAPI::SetThreadName("MAIN");

	Options opt = ParseOptions(argc, argv);
	if (opt.help) {
		PrintHelp();
		return 0;
	}

	if (opt.headless) {
		IO::MakeHeadless();
	}

	ConfigPtr pConfig = Initialize(opt.config_path, opt.environment);
	
	try
	{
		Run(pConfig, opt);
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Exception thrown: {}", e.what());
		LoggerAPI::Flush();
		IO::Err("Exception thrown", e);
		throw;
	}

	LoggerAPI::Flush();

	return 0;
}

ConfigPtr Initialize(const std::optional<fs::path>& config_path, const Environment environment)
{
	IO::Out("INITIALIZING...");
	IO::Flush();

	Global::SetConfigFilePath(config_path, Env::ToString(environment));

	ConfigPtr pConfig = nullptr;
	try
	{
		pConfig = Config::Load(Global::GetConfigFilePath(), environment);
		IO::Out("Configuration loaded.");
	}
	catch (std::exception& e)
	{
		IO::Err("Failed to open config", e);
		throw;
	}

	try
	{
		LoggerAPI::Initialize(pConfig->GetLogDirectory(), pConfig->GetLogLevel());
		IO::Out("Logger initialized.");
	}
	catch (std::exception& e)
	{
		IO::Err("Failed to initialize logger", e);
		throw;
	}

	// Preserve the existing profile logs. Diagnostic copies normally live next
	// to the executable; a read-only install falls back to the profile with an
	// explicit warning rather than silently discarding diagnostics.
	try {
		const fs::path diagnosticPath = ExecutableDirectory() / "logs";
		LoggerAPI::InitializeNodeDiagnostics(diagnosticPath, pConfig->GetLogLevel());
		LOG_INFO_F("Node diagnostic logs: {}", diagnosticPath.u8string());
	} catch (const std::exception& e) {
		std::cerr << "Cannot create logs beside executable: " << e.what() << std::endl;
		LOG_WARNING_F("Cannot create logs beside executable: {}; using profile fallback", e.what());
		try {
			const fs::path fallback = pConfig->GetLogDirectory() / "node-diagnostics";
			LoggerAPI::InitializeNodeDiagnostics(fallback, pConfig->GetLogLevel());
			LOG_WARNING_F("Node diagnostic logs use fallback: {}", fallback.u8string());
		} catch (const std::exception& fallbackError) {
			std::cerr << "Cannot create fallback diagnostic logs: " << fallbackError.what() << std::endl;
			LOG_ERROR_F("Cannot create fallback diagnostic logs: {}", fallbackError.what());
		}
	}
	const auto runMillis = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	LoggerAPI::LogSyncEvent("event=run_start network=" + Env::ToString(environment) + " run_id=" + std::to_string(runMillis));

	return pConfig;
}

void Run(const ConfigPtr& pConfig, const Options& options)
{
	LOG_INFO_F("Starting Grin++ v{}", GRINPP_VERSION);

	Context::Ptr pContext = nullptr;

	{
		ScopedTiming timing("run_context_init");
		try
		{
			pContext = Context::Create(options.environment, pConfig);
			Global::Init(pContext);
		}
		catch (std::exception& e)
		{
			IO::Err("Failed to initialize global context", e);
			throw;
		}
		timing.Success();
	}

	std::unique_ptr<Node> pNode = nullptr;
	INodeClientPtr pNodeClient = nullptr;

	{
		ScopedTiming timing("run_node_create");
		if (options.shared_node.has_value())
		{
			pNodeClient = RPCNodeClient::Create(
				options.shared_node.value().first,
				options.shared_node.value().second
			);
		}
		else
		{
			pNode = Node::Create(pContext);
			pNodeClient = pNode->GetNodeClient();
		}
		timing.Success();
	}
	IO::Out("RPC Node Client started.");

	std::unique_ptr<WalletDaemon> pWallet = nullptr;
	if (options.include_wallet) {
		pWallet = WalletDaemon::Create(
			pContext->GetConfig(),
			Global::GetTorProcess(),
			pNodeClient
		);
	}
	IO::Out("Wallet Daemon started.");

	system_clock::time_point startTime = system_clock::now();
	while (true) {
		if (!Global::IsRunning()) {
			IO::Clear();
			IO::Out("SHUTTING DOWN...");
			IO::Flush();

			break;
		}

		if (pNode != nullptr && !options.headless) {
			auto duration = system_clock::now().time_since_epoch() - startTime.time_since_epoch();
			const int secondsRunning = (int)(duration_cast<seconds>(duration).count());
			pNode->UpdateDisplay(secondsRunning);
		}

		ThreadUtil::SleepFor(seconds(1));
	}

	LOG_INFO_F("Closing Grin++ v{}", GRINPP_VERSION);
	pWallet.reset();
	pNodeClient.reset();
	pNode.reset();
	pContext.reset();
	LoggerAPI::LogSyncEvent("event=run_end reason=shutdown");
}
