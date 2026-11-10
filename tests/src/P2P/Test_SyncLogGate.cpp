#include <catch.hpp>

#include <P2P/Sync/SyncLogGate.h>

// Regression for the task-64 log flood: IsStateSyncDue() runs every ~10 ms and
// logged "Sync peer no longer connected." on every iteration while the 30 s
// re-request guard blocked a new request (15 306 lines / 1.66 MB in 6.5 min).
// Each transition warning must be emitted once per request epoch.
TEST_CASE("SyncLogGate logs each transition once per request epoch")
{
	SyncLogGate gate;

	REQUIRE(gate.ShouldLogPeerLost());
	REQUIRE_FALSE(gate.ShouldLogPeerLost());
	REQUIRE_FALSE(gate.ShouldLogPeerLost());

	REQUIRE(gate.ShouldLogSyncFailed());
	REQUIRE_FALSE(gate.ShouldLogSyncFailed());

	REQUIRE(gate.ShouldLogNoProgress());
	REQUIRE_FALSE(gate.ShouldLogNoProgress());

	REQUIRE(gate.ShouldLogDownloadTimeout());
	REQUIRE_FALSE(gate.ShouldLogDownloadTimeout());

	// A request that was actually sent starts a new epoch: the next transition
	// of every kind is reported again (diagnostics preserved).
	gate.OnRequestSent();
	REQUIRE(gate.ShouldLogPeerLost());
	REQUIRE(gate.ShouldLogSyncFailed());
	REQUIRE(gate.ShouldLogNoProgress());
	REQUIRE(gate.ShouldLogDownloadTimeout());
}

TEST_CASE("SyncLogGate suppresses the flood while the re-request is blocked")
{
	SyncLogGate gate;
	REQUIRE(gate.ShouldLogPeerLost());

	// RequestState was called but its 30 s guard returned false, so no
	// OnRequestSent() happens. The condition stays true for hundreds of sync
	// loop iterations; none of them may log again.
	for (int iteration = 0; iteration < 5000; ++iteration) {
		REQUIRE_FALSE(gate.ShouldLogPeerLost());
	}
	REQUIRE_FALSE(gate.ShouldLogPeerLost());
}

TEST_CASE("SyncLogGate is independent per transition kind")
{
	SyncLogGate gate;
	REQUIRE(gate.ShouldLogSyncFailed());
	// A different transition in the same epoch is still reported once.
	REQUIRE(gate.ShouldLogPeerLost());
	REQUIRE_FALSE(gate.ShouldLogSyncFailed());
	REQUIRE_FALSE(gate.ShouldLogPeerLost());
	// No-progress and timeout were not consumed yet.
	REQUIRE(gate.ShouldLogNoProgress());
	REQUIRE(gate.ShouldLogDownloadTimeout());
	REQUIRE_FALSE(gate.ShouldLogNoProgress());
	REQUIRE_FALSE(gate.ShouldLogDownloadTimeout());
}
