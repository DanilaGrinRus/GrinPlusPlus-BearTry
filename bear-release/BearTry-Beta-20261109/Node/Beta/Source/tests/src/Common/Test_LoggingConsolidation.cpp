#include <catch.hpp>
#include <Common/Logger.h>
#include <Common/Util/FileUtil.h>
#include <filesystem.h>
#include <chrono>
#include <fstream>
#include <string>

namespace {
fs::path makeTempDir()
{
	const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path dir = fs::temp_directory_path() / ("grinpp-logging-" + std::to_string(suffix));
	fs::create_directories(dir);
	return dir;
}

std::string readFile(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}

TEST_CASE("Logging consolidation keeps Node.log and only errors/sync beside it", "[logging][.]")
{
	const fs::path dir = makeTempDir();

	LoggerAPI::Shutdown();
	LoggerAPI::Initialize(dir, "INFO");
	LoggerAPI::InitializeNodeDiagnostics(dir, "INFO");
	LOG_INFO_F("consolidated info event {}", 1);
	LOG_ERROR_F("consolidated error event {}", 2);
	LoggerAPI::LogSyncEvent("event=stage_begin stage=headers");
	LoggerAPI::Flush();
	LoggerAPI::Shutdown();

	REQUIRE(fs::exists(dir / "Node.log"));
	REQUIRE(fs::exists(dir / "errors.log"));
	REQUIRE(fs::exists(dir / "sync-stages.log"));
	REQUIRE_FALSE(fs::exists(dir / "node-diagnostics.log"));

	const std::string nodeText = readFile(dir / "Node.log");
	const std::string errorText = readFile(dir / "errors.log");
	const std::string syncText = readFile(dir / "sync-stages.log");
	REQUIRE(nodeText.find("consolidated info event 1") != std::string::npos);
	REQUIRE(nodeText.find("consolidated error event 2") != std::string::npos);
	REQUIRE(errorText.find("consolidated error event 2") != std::string::npos);
	REQUIRE(errorText.find("consolidated info event 1") == std::string::npos);
	REQUIRE(syncText.find("event=stage_begin stage=headers") != std::string::npos);

	// Restart reopens the same files and appends; no duplicate logger-name crash.
	LoggerAPI::Initialize(dir, "INFO");
	LoggerAPI::InitializeNodeDiagnostics(dir, "INFO");
	LOG_INFO_F("consolidated after restart");
	LoggerAPI::Flush();
	LoggerAPI::Shutdown();
	REQUIRE(readFile(dir / "Node.log").find("consolidated after restart") != std::string::npos);

	// A regular file cannot be used as the log directory: initialization must fail.
	const fs::path notADirectory = dir / "not-a-directory";
	std::ofstream(notADirectory, std::ios::binary) << "x";
	REQUIRE_THROWS(LoggerAPI::InitializeNodeDiagnostics(notADirectory, "INFO"));
	REQUIRE_FALSE(fs::exists(dir / "node-diagnostics.log"));

	fs::remove_all(dir);
}

TEST_CASE("Errors and sync logs rotate independently", "[logging][.]")
{
	const fs::path dir = makeTempDir();
	LoggerAPI::Shutdown();
	LoggerAPI::Initialize(dir, "INFO");
	LoggerAPI::InitializeNodeDiagnostics(dir, "INFO");

	const std::string payload(2048, 'e');
	for (int i = 0; i < 3200; ++i) {
		LOG_ERROR_F("{} {}", i, payload);
	}
	for (int i = 0; i < 600; ++i) {
		LoggerAPI::LogSyncEvent("event=stage_synthetic payload=" + payload);
	}
	LoggerAPI::Flush();
	LoggerAPI::Shutdown();

	REQUIRE(fs::file_size(dir / "errors.log") <= 5 * 1024 * 1024);
	REQUIRE(fs::exists(dir / "errors.1.log"));
	REQUIRE(fs::exists(dir / "sync-stages.log"));
	REQUIRE(fs::exists(dir / "sync-stages.1.log"));
	REQUIRE_FALSE(fs::exists(dir / "node-diagnostics.log"));
	fs::remove_all(dir);
}


