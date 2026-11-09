#include <catch.hpp>

#include <P2P/Sync/PeerRatePolicy.h>

// Regression for task-72: on a localhost catch-up the target received
// >500 block messages/minute from its sole 127.0.0.1 peer and the rate limiter
// banned that healthy peer for 3 hours ("Banning 127.0.0.1 for exceeding rate
// limit"), which stopped block sync short of FULLY_SYNCED.
//
// Public/remote peers must keep exactly the previous behaviour: the same
// 500 messages/minute threshold and the same ban action.
TEST_CASE("PeerRatePolicy keeps the public incoming abuse threshold")
{
	REQUIRE(PeerRatePolicy::MAX_RECEIVED_PER_MINUTE == 500);
	REQUIRE_FALSE(PeerRatePolicy::IsAbusive(0));
	REQUIRE_FALSE(PeerRatePolicy::IsAbusive(499));
	REQUIRE_FALSE(PeerRatePolicy::IsAbusive(500));
	REQUIRE(PeerRatePolicy::IsAbusive(501));
	REQUIRE(PeerRatePolicy::IsAbusive(100000));
}

TEST_CASE("RateLimitAction bans remote peers above the threshold")
{
	REQUIRE(PeerRatePolicy::RateLimitAction(500, false) == PeerRatePolicy::ERateLimitAction::None);
	REQUIRE(PeerRatePolicy::RateLimitAction(501, false) == PeerRatePolicy::ERateLimitAction::Ban);
	REQUIRE(PeerRatePolicy::RateLimitAction(1200, false) == PeerRatePolicy::ERateLimitAction::Ban);
	REQUIRE(PeerRatePolicy::RateLimitAction(100000, false) == PeerRatePolicy::ERateLimitAction::Ban);
}

TEST_CASE("RateLimitAction never bans a loopback peer for rate alone (task-72)")
{
	// Below the threshold nothing happens for either kind of peer.
	REQUIRE(PeerRatePolicy::RateLimitAction(500, true) == PeerRatePolicy::ERateLimitAction::None);

	// A healthy fast local catch-up: >500 messages/minute on loopback must be
	// logged but must NOT produce the 3-hour Abusive ban.
	REQUIRE(PeerRatePolicy::RateLimitAction(501, true) == PeerRatePolicy::ERateLimitAction::IgnoreLoopback);
	REQUIRE(PeerRatePolicy::RateLimitAction(766, true) == PeerRatePolicy::ERateLimitAction::IgnoreLoopback);
	REQUIRE(PeerRatePolicy::RateLimitAction(5000, true) == PeerRatePolicy::ERateLimitAction::IgnoreLoopback);
}

TEST_CASE("RateLimitAction is only loopback-exempt, not threshold-exempt")
{
	// The same rate yields opposite actions depending on peer locality: the
	// public DoS protection is unchanged, only the local stand is exempt.
	const size_t rates[] = { 501, 700, 5000 };
	for (const size_t rate : rates) {
		REQUIRE(PeerRatePolicy::RateLimitAction(rate, false) == PeerRatePolicy::ERateLimitAction::Ban);
		REQUIRE(PeerRatePolicy::RateLimitAction(rate, true) == PeerRatePolicy::ERateLimitAction::IgnoreLoopback);
	}

	// The loopback warning is throttled to once per minute (the ping thread runs
	// every second; an unthrottled warning recreated the task-66 log flood).
	REQUIRE(PeerRatePolicy::LOOPBACK_RATE_LOG_INTERVAL.count() == 60);
}
