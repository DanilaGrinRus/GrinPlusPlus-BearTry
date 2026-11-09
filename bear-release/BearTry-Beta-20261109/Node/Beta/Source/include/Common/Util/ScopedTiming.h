#pragma once

#include <Common/Logger.h>
#include <chrono>
#include <string>

// Lightweight RAII startup/timing probe. Logs one line to the node diagnostics
// log on destruction; the outcome is "success" only when Success() was called.
// Intentionally header-only so startup code can instrument nested phases
// without touching shared build configuration.
class ScopedTiming
{
public:
	explicit ScopedTiming(const char* name) noexcept
		: m_name(name), m_start(std::chrono::steady_clock::now())
	{
	}

	~ScopedTiming() noexcept
	{
		try {
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - m_start).count();
			LOG_INFO_F("startup_timing name={} duration_ms={} outcome={}",
				m_name,
				ms,
				m_success ? "success" : "incomplete_or_failed");
		} catch (...) { }
	}

	void Success() noexcept { m_success = true; }

	ScopedTiming(const ScopedTiming&) = delete;
	ScopedTiming& operator=(const ScopedTiming&) = delete;

private:
	const char* m_name;
	std::chrono::steady_clock::time_point m_start;
	bool m_success = false;
};
