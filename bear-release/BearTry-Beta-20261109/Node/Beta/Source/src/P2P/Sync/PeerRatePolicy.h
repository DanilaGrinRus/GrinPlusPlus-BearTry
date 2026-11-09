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
    // Expected block/header responses during an active catch-up can legitimately
    // exceed the generic 500/min abuse threshold. Keep a separate, bounded
    // allowance; non-sync traffic remains under the original threshold.
    constexpr size_t MAX_SYNC_RECEIVED_PER_MINUTE = 5000;

    inline bool HeaderRequestDue(const Clock::time_point now, const Clock::time_point next) noexcept
    {
        return now >= next;
    }

    inline bool IsAbusive(const size_t received) noexcept
    {
        return received > MAX_RECEIVED_PER_MINUTE;
    }

    // Action for a peer that exceeded the incoming message rate.
    enum class ERateLimitAction
    {
        None,             // below the abuse threshold
        IgnoreLoopback,   // local stand peer: log (throttled), never ban
        IgnoreSyncBurst,  // remote peer: expected sync messages within bound
        Ban               // remote peer: keep the public DoS ban
    };

    // A loopback peer is the local operator/test stand, not a remote DoS vector:
    // a healthy fast local catch-up legitimately delivers more than 500 block
    // messages per minute and must not be banned for it. Observed on the task-70
    // no-fill control: the sole 127.0.0.1 peer was banned for 3 hours
    // (`Banning 127.0.0.1 for exceeding rate limit`) at block height 4,094,423,
    // which prevented reaching FULLY_SYNCED.
    //
    // Public/remote peers keep exactly the pre-existing behaviour and threshold:
    // they are still counted, still classified at >500 messages/minute and still
    // banned with EBanReason::Abusive. Only the loopback decision changed.
    inline ERateLimitAction RateLimitAction(const size_t receivedInLastMinute,
                                            const size_t syncReceivedInLastMinute,
                                            const bool isLoopback,
                                            const bool isSyncing) noexcept
    {
        if (!IsAbusive(receivedInLastMinute))
        {
            return ERateLimitAction::None;
        }

        if (isLoopback)
        {
            return ERateLimitAction::IgnoreLoopback;
        }

        // A remote peer may exceed the generic threshold only while a sync is
        // active and the excess is attributable to expected block/header
        // messages. Non-sync traffic must still fit the original 500/min bound.
        const size_t nonSyncReceived = receivedInLastMinute > syncReceivedInLastMinute
            ? receivedInLastMinute - syncReceivedInLastMinute
            : 0;
        if (isSyncing &&
            syncReceivedInLastMinute <= MAX_SYNC_RECEIVED_PER_MINUTE &&
            nonSyncReceived <= MAX_RECEIVED_PER_MINUTE)
        {
            return ERateLimitAction::IgnoreSyncBurst;
        }

        return ERateLimitAction::Ban;
    }

    // Compatibility overload for non-connection callers/tests: no sync evidence.
    inline ERateLimitAction RateLimitAction(const size_t receivedInLastMinute,
                                            const bool isLoopback) noexcept
    {
        return RateLimitAction(receivedInLastMinute, 0, isLoopback, false);
    }

    // Minimum interval between "loopback peer over rate limit" warnings: the
    // connection ping thread runs every second, so an unthrottled message would
    // recreate the task-66 log flood.
    constexpr std::chrono::seconds LOOPBACK_RATE_LOG_INTERVAL{60};
}
