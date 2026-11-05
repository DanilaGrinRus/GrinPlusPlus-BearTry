#pragma once

#include <chrono>
#include <cstddef>

// Bound locally requested header responses below the Grin 500/min incoming
// abuse limit while preserving the incoming flood check for all peers.
namespace PeerRatePolicy
{
    using Clock = std::chrono::steady_clock;
    constexpr std::chrono::milliseconds MIN_HEADER_REQUEST_INTERVAL{200};
    constexpr size_t MAX_RECEIVED_PER_MINUTE = 500;

    inline bool HeaderRequestDue(const Clock::time_point now, const Clock::time_point next) noexcept
    {
        return now >= next;
    }

    inline bool IsAbusive(const size_t received) noexcept
    {
        return received > MAX_RECEIVED_PER_MINUTE;
    }
}
