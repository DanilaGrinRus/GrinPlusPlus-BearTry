#pragma once

#include <cstdint>

// Parameters mirror the Grin staging PIHD implementation. PIHD is not part of
// v5.5.2; the client is dormant by default and is only enabled explicitly.
namespace Pihd
{
	static constexpr uint8_t HEADER_SEGMENT_HEIGHT = 9;
	static constexpr uint32_t HEADERS_PER_SEGMENT = 512;
	static constexpr uint64_t FIRST_SEGMENT_START_HEIGHT = 1;
	static constexpr uint32_t MAX_IN_FLIGHT = 8;
	static constexpr uint32_t MAX_IN_FLIGHT_PER_PEER = 2;
	static constexpr uint32_t SEGMENT_TIMEOUT_SECONDS = 20;
	static constexpr uint32_t FAILED_SEGMENT_COOLDOWN_SECONDS = 30;
	static constexpr uint32_t FAILURES_BEFORE_BLOCK = 3;
	static constexpr uint32_t FAILURES_BEFORE_FALLBACK = 3;
	static constexpr uint32_t NO_PROGRESS_FALLBACK_SECONDS = 120;
	static constexpr uint32_t FALLBACK_COOLDOWN_SECONDS = 120;

	enum class EPihdMode
	{
		OFF = 0,
		CLIENT = 1,
		FULL = 2
	};

	static inline uint64_t SegmentStartHeight(const uint64_t idx)
	{
		return idx * (uint64_t)HEADERS_PER_SEGMENT + FIRST_SEGMENT_START_HEIGHT;
	}

	static inline uint64_t FirstSegmentIdxForHeight(const uint64_t height)
	{
		if (height <= FIRST_SEGMENT_START_HEIGHT) {
			return 0;
		}
		return (height - FIRST_SEGMENT_START_HEIGHT) / HEADERS_PER_SEGMENT;
	}
}
