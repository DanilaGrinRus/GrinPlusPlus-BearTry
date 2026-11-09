#pragma once

#include <Common/Util/StringUtil.h>
#include <string>
#include <chrono>
#include <utility>

#define LOGGER_API

namespace LoggerAPI
{
	enum class LogFile
	{
		NODE,
		WALLET
	};

	enum class LogLevel
	{
		TRACE,
		DEBUG,
		INFO,
		WARN,
		ERR
	};

	LOGGER_API void Initialize(const fs::path& logDirectory, const std::string& logLevel);
	LOGGER_API void InitializeNodeDiagnostics(const fs::path& logDirectory, const std::string& logLevel);
	LOGGER_API void LogSyncEvent(const std::string& message) noexcept;
	LOGGER_API void Shutdown();
	LOGGER_API void SetThreadName(const std::string& thread_name);

	LOGGER_API bool WillLog(const LogFile file, const LogLevel level);

	LOGGER_API void LogTrace(const std::string& message);
	LOGGER_API void LogDebug(const std::string& message);
	LOGGER_API void LogInfo(const std::string& message);
	LOGGER_API void LogWarning(const std::string& message);
	LOGGER_API void LogError(const std::string& message);
	LOGGER_API void Flush();


	LOGGER_API void LogTrace(const LogFile file, const std::string& function, const size_t line, const std::string& message);
	LOGGER_API void LogDebug(const LogFile file, const std::string& function, const size_t line, const std::string& message);
	LOGGER_API void LogInfo(const LogFile file, const std::string& function, const size_t line, const std::string& message);
	LOGGER_API void LogWarning(const LogFile file, const std::string& function, const size_t line, const std::string& message);
	LOGGER_API void LogError(const LogFile file, const std::string& function, const size_t line, const std::string& message);
}

// A subphase is logged independently of the overall sync state. If validation
// exits early or throws, its ending is explicitly marked incomplete/failed.
class ScopedSyncPhase
{
public:
	explicit ScopedSyncPhase(const char* phase) noexcept
		: m_phase(phase), m_start(std::chrono::steady_clock::now())
	{
		try { LoggerAPI::LogSyncEvent(std::string("event=phase_begin phase=") + m_phase); } catch (...) { }
	}
	~ScopedSyncPhase() noexcept
	{
		try {
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - m_start).count();
			LoggerAPI::LogSyncEvent(std::string("event=phase_end phase=") + m_phase + " duration_ms=" +
				std::to_string(ms) + (m_success ? " outcome=success" : " outcome=incomplete_or_failed"));
		} catch (...) { }
	}
	void Success() noexcept { m_success = true; }
	ScopedSyncPhase(const ScopedSyncPhase&) = delete;
	ScopedSyncPhase& operator=(const ScopedSyncPhase&) = delete;
private:
	const char* m_phase;
	std::chrono::steady_clock::time_point m_start;
	bool m_success = false;
};

// Node Logger
#define LOG_TRACE(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::TRACE)) LoggerAPI::LogTrace(LoggerAPI::LogFile::NODE, __func__, __LINE__, message); } while (0)
#define LOG_DEBUG(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::DEBUG)) LoggerAPI::LogDebug(LoggerAPI::LogFile::NODE, __func__, __LINE__, message); } while (0)
#define LOG_INFO(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::INFO)) LoggerAPI::LogInfo(LoggerAPI::LogFile::NODE, __func__, __LINE__, message); } while (0)
#define LOG_WARNING(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::WARN)) LoggerAPI::LogWarning(LoggerAPI::LogFile::NODE, __func__, __LINE__, message); } while (0)
#define LOG_ERROR(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::ERR)) LoggerAPI::LogError(LoggerAPI::LogFile::NODE, __func__, __LINE__, message); } while (0)

#define LOG_TRACE_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::TRACE)) LoggerAPI::LogTrace(LoggerAPI::LogFile::NODE, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)
#define LOG_DEBUG_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::DEBUG)) LoggerAPI::LogDebug(LoggerAPI::LogFile::NODE, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)
#define LOG_INFO_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::INFO)) LoggerAPI::LogInfo(LoggerAPI::LogFile::NODE, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)
#define LOG_WARNING_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::WARN)) LoggerAPI::LogWarning(LoggerAPI::LogFile::NODE, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)
#define LOG_ERROR_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::NODE, LoggerAPI::LogLevel::ERR)) LoggerAPI::LogError(LoggerAPI::LogFile::NODE, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)

// Wallet Logger
#define WALLET_TRACE(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::TRACE)) LoggerAPI::LogTrace(LoggerAPI::LogFile::WALLET, __func__, __LINE__, message); } while (0)
#define WALLET_DEBUG(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::DEBUG)) LoggerAPI::LogDebug(LoggerAPI::LogFile::WALLET, __func__, __LINE__, message); } while (0)
#define WALLET_INFO(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::INFO)) LoggerAPI::LogInfo(LoggerAPI::LogFile::WALLET, __func__, __LINE__, message); } while (0)
#define WALLET_WARNING(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::WARN)) LoggerAPI::LogWarning(LoggerAPI::LogFile::WALLET, __func__, __LINE__, message); } while (0)
#define WALLET_ERROR(message) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::ERR)) LoggerAPI::LogError(LoggerAPI::LogFile::WALLET, __func__, __LINE__, message); } while (0)

#define WALLET_TRACE_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::TRACE)) LoggerAPI::LogTrace(LoggerAPI::LogFile::WALLET, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)
#define WALLET_DEBUG_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::DEBUG)) LoggerAPI::LogDebug(LoggerAPI::LogFile::WALLET, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)
#define WALLET_INFO_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::INFO)) LoggerAPI::LogInfo(LoggerAPI::LogFile::WALLET, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)
#define WALLET_WARNING_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::WARN)) LoggerAPI::LogWarning(LoggerAPI::LogFile::WALLET, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)
#define WALLET_ERROR_F(message, ...) do { if (LoggerAPI::WillLog(LoggerAPI::LogFile::WALLET, LoggerAPI::LogLevel::ERR)) LoggerAPI::LogError(LoggerAPI::LogFile::WALLET, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__)); } while (0)
