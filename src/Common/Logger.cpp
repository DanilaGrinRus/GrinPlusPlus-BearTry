#include <spdlog/spdlog.h>
#include <Common/Logger.h>
#include <Common/Util/FileUtil.h>
#include <shared_mutex>
#include <sstream>
#include <fstream>
#include <iostream>
#include <memory>

class Logger
{
public:
	static Logger& GetInstance();

	void StartLogger(
		const fs::path& logDirectory,
		const spdlog::level::level_enum& logLevel
	);
	void StopLogger();
	void StartNodeDiagnostics(const fs::path& directory, const spdlog::level::level_enum& level);
	void SyncEvent(const std::string& text) noexcept;
	void Log(const LoggerAPI::LogFile file, const spdlog::level::level_enum logLevel, const std::string& eventText);
	void Flush();
	void SetThreadName(const std::string& thread_name);

private:
	Logger() = default;

	std::shared_ptr<spdlog::logger> GetLogger(const LoggerAPI::LogFile file);
	std::string GetThreadName() const;

	mutable std::shared_mutex m_threadNamesMutex;
	std::unordered_map<std::thread::id, std::string> m_threadNamesById;

	std::shared_ptr<spdlog::logger> m_pNodeLogger;
	std::shared_ptr<spdlog::logger> m_pWalletLogger;
	std::shared_ptr<spdlog::logger> m_pDiagnosticLogger;
	std::shared_ptr<spdlog::logger> m_pErrorLogger;
	std::shared_ptr<spdlog::logger> m_pSyncLogger;
};

Logger& Logger::GetInstance()
{
	static Logger instance;
	return instance;
}

void Logger::StartNodeDiagnostics(const fs::path& directory, const spdlog::level::level_enum& level)
{
	fs::create_directories(directory);
	try {
		auto nodeSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
			StringUtil::ToWide((directory / "node-diagnostics.log").u8string()), 5 * 1024 * 1024, 5);
		auto errorSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
			StringUtil::ToWide((directory / "errors.log").u8string()), 5 * 1024 * 1024, 5);
		auto syncSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
			StringUtil::ToWide((directory / "sync-stages.log").u8string()), 1024 * 1024, 5);
		auto node = spdlog::create_async("NODE_DIAGNOSTICS", nodeSink, 8192, spdlog::async_overflow_policy::block_retry, nullptr, std::chrono::seconds(5));
		auto errors = spdlog::create_async("NODE_ERRORS", errorSink, 2048, spdlog::async_overflow_policy::block_retry, nullptr, std::chrono::seconds(5));
		auto sync = spdlog::create_async("NODE_SYNC_STAGES", syncSink, 2048, spdlog::async_overflow_policy::block_retry, nullptr, std::chrono::seconds(5));
		node->set_level(level);
		errors->set_level(spdlog::level::err);
		errors->flush_on(spdlog::level::err);
		sync->set_level(spdlog::level::info);
		sync->flush_on(spdlog::level::info);
		m_pDiagnosticLogger = std::move(node);
		m_pErrorLogger = std::move(errors);
		m_pSyncLogger = std::move(sync);
	} catch (...) {
		spdlog::drop("NODE_DIAGNOSTICS");
		spdlog::drop("NODE_ERRORS");
		spdlog::drop("NODE_SYNC_STAGES");
		throw;
	}
}

void Logger::SyncEvent(const std::string& text) noexcept
{
	try { if (m_pSyncLogger != nullptr) m_pSyncLogger->log(spdlog::level::info, text); }
	catch (...) { std::cerr << "Failed to write sync-stage log" << std::endl; }
}

void Logger::StartLogger(const fs::path& logDirectory, const spdlog::level::level_enum& logLevel)
{
	FileUtil::CreateDirectories(logDirectory);

	std::unique_lock<std::shared_mutex> thread_lock(m_threadNamesMutex);
	m_threadNamesById.clear();
	thread_lock.unlock();

	if (m_pNodeLogger == nullptr)
	{
		const fs::path logPath = logDirectory / "Node.log";
		auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(StringUtil::ToWide(logPath.u8string()), 5 * 1024 * 1024, 10);
		m_pNodeLogger = spdlog::create_async("NODE", sink, 32768, spdlog::async_overflow_policy::block_retry, nullptr, std::chrono::seconds(5));
		spdlog::set_pattern("[%D %X.%e%z] [%l] %v");
		if (m_pNodeLogger != nullptr)
		{
			m_pNodeLogger->set_level(logLevel);
		}
	}

	if (m_pWalletLogger == nullptr)
	{
		const fs::path logPath = logDirectory / "Wallet.log";
		auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(StringUtil::ToWide(logPath.u8string()), 5 * 1024 * 1024, 10);
		m_pWalletLogger = spdlog::create_async("WALLET", sink, 8192, spdlog::async_overflow_policy::block_retry, nullptr, std::chrono::seconds(5));
		spdlog::set_pattern("[%D %X.%e%z] [%l] %v");
		if (m_pWalletLogger != nullptr)
		{
			m_pWalletLogger->set_level(logLevel);
		}
	}
}

void Logger::StopLogger()
{
	Flush();
	m_pDiagnosticLogger.reset();
	m_pErrorLogger.reset();
	m_pSyncLogger.reset();
	spdlog::drop("NODE_DIAGNOSTICS");
	spdlog::drop("NODE_ERRORS");
	spdlog::drop("NODE_SYNC_STAGES");
	m_pNodeLogger.reset();
	m_pWalletLogger.reset();
}

void Logger::Log(const LoggerAPI::LogFile file, const spdlog::level::level_enum logLevel, const std::string& eventText)
{
	auto pLogger = GetLogger(file);
	if (pLogger != nullptr)
	{
		std::string eventTextClean = eventText;
		size_t newlinePos = eventTextClean.find("\n");
		while (newlinePos != std::string::npos)
		{
			eventTextClean.erase(newlinePos, 1);
			newlinePos = eventTextClean.find("\n");
		}

		std::string threadName = GetThreadName();
		if (!threadName.empty())
		{
			eventTextClean = threadName + " " + eventTextClean;
		}

		pLogger->log(logLevel, eventTextClean);
		try {
			if (file == LoggerAPI::LogFile::NODE && m_pDiagnosticLogger != nullptr) {
				m_pDiagnosticLogger->log(logLevel, eventTextClean);
			}
			if (file == LoggerAPI::LogFile::NODE && logLevel >= spdlog::level::err && m_pErrorLogger != nullptr) {
				m_pErrorLogger->log(logLevel, eventTextClean);
			}
		} catch (...) { std::cerr << "Failed to write node diagnostic log" << std::endl; }
	}
}

void Logger::Flush()
{
	if (m_pDiagnosticLogger != nullptr) m_pDiagnosticLogger->flush();
	if (m_pErrorLogger != nullptr) m_pErrorLogger->flush();
	if (m_pSyncLogger != nullptr) m_pSyncLogger->flush();
	if (m_pNodeLogger != nullptr)
	{
		m_pNodeLogger->flush();
	}

	if (m_pWalletLogger != nullptr)
	{
		m_pWalletLogger->flush();
	}
}

void Logger::SetThreadName(const std::string& thread_name)
{
	std::unique_lock<std::shared_mutex> lockGuard(m_threadNamesMutex);

	std::stringstream ss;
	ss << "[" << thread_name << ":" << std::this_thread::get_id() << "]";
	m_threadNamesById[std::this_thread::get_id()] = ss.str();
}

std::shared_ptr<spdlog::logger> Logger::GetLogger(const LoggerAPI::LogFile file)
{
	if (file == LoggerAPI::LogFile::WALLET)
	{
		return m_pWalletLogger;
	}
	else
	{
		return m_pNodeLogger;
	}
}

std::string Logger::GetThreadName() const
{
	std::shared_lock<std::shared_mutex> readLock(m_threadNamesMutex);

	std::thread::id thread_id = std::this_thread::get_id();

	try {
		if (m_threadNamesById.find(thread_id) != m_threadNamesById.end()) {
			return m_threadNamesById.find(thread_id)->second;
		}
	}
	catch (std::exception& e) {
		std::cout << "Exception thrown in GetThreadName() - " << e.what() << std::endl;
	}

	std::stringstream ss;
	ss << "[THREAD:" << thread_id << "]";
	return ss.str();
}

namespace LoggerAPI
{
	LOGGER_API void InitializeNodeDiagnostics(const fs::path& logDirectory, const std::string& logLevel)
	{
		auto level = spdlog::level::debug;
		if (logLevel == "TRACE") level = spdlog::level::trace;
		else if (logLevel == "INFO") level = spdlog::level::info;
		else if (logLevel == "WARN") level = spdlog::level::warn;
		else if (logLevel == "ERROR") level = spdlog::level::err;
		Logger::GetInstance().StartNodeDiagnostics(logDirectory, level);
	}

	LOGGER_API void LogSyncEvent(const std::string& message) noexcept
	{
		Logger::GetInstance().SyncEvent(message);
	}
	LOGGER_API void Initialize(const fs::path& logDirectory, const std::string& logLevel)
	{
		spdlog::level::level_enum logLevelEnum = spdlog::level::level_enum::debug;
		if (logLevel == "TRACE")
		{
			logLevelEnum = spdlog::level::level_enum::trace;
		}
		else if (logLevel == "DEBUG")
		{
			logLevelEnum = spdlog::level::level_enum::debug;
		}
		else if (logLevel == "INFO")
		{
			logLevelEnum = spdlog::level::level_enum::info;
		}
		else if (logLevel == "WARN")
		{
			logLevelEnum = spdlog::level::level_enum::warn;
		}
		else if (logLevel == "ERROR")
		{
			logLevelEnum = spdlog::level::level_enum::err;
		}

		Logger::GetInstance().StartLogger(logDirectory, logLevelEnum);
	}

	LOGGER_API void Shutdown()
	{
		Logger::GetInstance().StopLogger();
	}

	LOGGER_API void SetThreadName(const std::string& thread_name)
	{
		Logger::GetInstance().SetThreadName(thread_name);
	}

	LOGGER_API void LogTrace(const std::string& message)
	{
		Logger::GetInstance().Log(LogFile::NODE, spdlog::level::level_enum::trace, message);
	}

	LOGGER_API void LogDebug(const std::string& message)
	{
		Logger::GetInstance().Log(LogFile::NODE, spdlog::level::level_enum::debug, message);
	}

	LOGGER_API void LogInfo(const std::string& message)
	{
		Logger::GetInstance().Log(LogFile::NODE, spdlog::level::level_enum::info, message);
	}

	LOGGER_API void LogWarning(const std::string& message)
	{
		Logger::GetInstance().Log(LogFile::NODE, spdlog::level::level_enum::warn, message);
	}

	LOGGER_API void LogError(const std::string& message)
	{
		Logger::GetInstance().Log(LogFile::NODE, spdlog::level::level_enum::err, message);
	}

	LOGGER_API void Flush()
	{
		Logger::GetInstance().Flush();
	}


	LOGGER_API void LogTrace(const LogFile file, const std::string& function, const size_t line, const std::string& message)
	{
		const std::string formatted = function + "(" + std::to_string(line) + ") - " + message;
		Logger::GetInstance().Log(file, spdlog::level::level_enum::trace, formatted);
	}

	LOGGER_API void LogDebug(const LogFile file, const std::string& function, const size_t line, const std::string& message)
	{
		const std::string formatted = function + "(" + std::to_string(line) + ") - " + message;
		Logger::GetInstance().Log(file, spdlog::level::level_enum::debug, formatted);
	}

	LOGGER_API void LogInfo(const LogFile file, const std::string& function, const size_t line, const std::string& message)
	{
		const std::string formatted = function + "(" + std::to_string(line) + ") - " + message;
		Logger::GetInstance().Log(file, spdlog::level::level_enum::info, formatted);
	}

	LOGGER_API void LogWarning(const LogFile file, const std::string& function, const size_t line, const std::string& message)
	{
		const std::string formatted = function + "(" + std::to_string(line) + ") - " + message;
		Logger::GetInstance().Log(file, spdlog::level::level_enum::warn, formatted);
	}

	LOGGER_API void LogError(const LogFile file, const std::string& function, const size_t line, const std::string& message)
	{
		const std::string formatted = function + "(" + std::to_string(line) + ") - " + message;
		Logger::GetInstance().Log(file, spdlog::level::level_enum::err, formatted);
	}
}
