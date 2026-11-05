#include <catch.hpp>

#include <P2P/SyncStatus.h>

TEST_CASE("TxHashSet request identity distinguishes current, late and unsolicited")
{
	SyncStatus status;
	status.RecordTxHashSetRequest("peerA", "hashA", 100);

	REQUIRE(status.IsCurrentTxHashSetRequest("peerA", "hashA"));
	REQUIRE(status.WasTxHashSetRequested("peerA", "hashA"));
	REQUIRE_FALSE(status.IsCurrentTxHashSetRequest("peerA", "hashB"));
	REQUIRE_FALSE(status.WasTxHashSetRequested("peerB", "hashA"));
	REQUIRE_FALSE(status.WasTxHashSetRequested("peerA", "hashB"));

	// Rotating to another peer makes the first response late, not unsolicited.
	status.RecordTxHashSetRequest("peerB", "hashB", 200);
	REQUIRE_FALSE(status.IsCurrentTxHashSetRequest("peerA", "hashA"));
	REQUIRE(status.WasTxHashSetRequested("peerA", "hashA"));
	REQUIRE(status.IsCurrentTxHashSetRequest("peerB", "hashB"));
	REQUIRE_FALSE(status.WasTxHashSetRequested("peerC", "hashC"));

	// History is bounded; the oldest identity falls out.
	for (int i = 0; i < 10; ++i) {
		status.RecordTxHashSetRequest(
			"peer" + std::to_string(i),
			"hash" + std::to_string(i),
			static_cast<uint64_t>(i));
	}
	REQUIRE_FALSE(status.WasTxHashSetRequested("peerA", "hashA"));
	REQUIRE(status.WasTxHashSetRequested("peer9", "hash9"));
	REQUIRE(status.IsCurrentTxHashSetRequest("peer9", "hash9"));
}
