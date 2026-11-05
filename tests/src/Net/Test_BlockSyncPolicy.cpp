#include <catch.hpp>

#include <P2P/SyncStatus.h>
#include "../../../src/Server/Node/SyncProgressDisplay.h"
#include <P2P/Sync/BlockSyncPolicy.h>
#include <P2P/Sync/PeerRatePolicy.h>

#include <chrono>
#include <vector>

TEST_CASE("BlockSyncPolicy::BatchAndWindow")
{
	REQUIRE(BlockSyncPolicy::ComputeBatchLimit(0) == 0);
	REQUIRE(BlockSyncPolicy::ComputeBatchLimit(1) == 10);
	REQUIRE(BlockSyncPolicy::ComputeBatchLimit(4) == 40);
	REQUIRE(BlockSyncPolicy::ComputeBatchLimit(50) == BlockSyncPolicy::MAX_BATCH);

	REQUIRE(BlockSyncPolicy::ComputeRequestCeiling(1000, 900) == 900);
	REQUIRE(BlockSyncPolicy::ComputeRequestCeiling(900, 1000) == 900);
	REQUIRE(BlockSyncPolicy::ComputeRequestCeiling(900, 0) == 900);

	// A peer that reported height 50 must not be asked for block 51.
	REQUIRE(BlockSyncPolicy::ComputePeerRequestCeiling(900, 50) == 50);
	REQUIRE(BlockSyncPolicy::ComputePeerRequestCeiling(900, 0) == 0);
}

TEST_CASE("BlockSyncPolicy::RetryAndBackoff")
{
	REQUIRE(BlockSyncPolicy::RetryDelay(false) == std::chrono::seconds(6));
	REQUIRE(BlockSyncPolicy::RetryDelay(true) == std::chrono::seconds(6));
	REQUIRE(BlockSyncPolicy::RetryBackoff(true, 1) == std::chrono::seconds(12));
	REQUIRE(BlockSyncPolicy::RetryBackoff(false, 0) == std::chrono::seconds(6));
	REQUIRE(BlockSyncPolicy::RetryBackoff(false, 1) == std::chrono::seconds(12));
	REQUIRE(BlockSyncPolicy::RetryBackoff(false, 4) == std::chrono::seconds(60));
	REQUIRE(BlockSyncPolicy::RetryBackoff(false, 20) == std::chrono::seconds(60));
}

TEST_CASE("BlockSyncPolicy::SelectPeerRespectsHeightAndCooldown")
{
	// height 100, 200 (cooling down), 300.
	std::vector<BlockSyncPolicy::PeerCandidate> peers{
		{ 100, false },
		{ 200, true },
		{ 300, false },
	};

	REQUIRE(BlockSyncPolicy::SelectPeer(peers, 150, 0).value() == 2);
	REQUIRE(BlockSyncPolicy::SelectPeer(peers, 150, 1).value() == 2);
	REQUIRE(BlockSyncPolicy::SelectPeer(peers, 250, 0).value() == 2);
	REQUIRE(!BlockSyncPolicy::SelectPeer(peers, 350, 0).has_value());

	// Second candidate is skipped while cooling down, but accepted afterwards.
	REQUIRE(BlockSyncPolicy::SelectPeer(peers, 100, 0).value() == 0);
	REQUIRE(BlockSyncPolicy::SelectPeer(peers, 200, 0).value() == 2);
	REQUIRE(!BlockSyncPolicy::SelectPeer({}, 10, 0).has_value());
}

TEST_CASE("BlockSyncPolicy::FraudHeightNeedsStaleHigherClaim")
{
	using namespace std::chrono;
	REQUIRE(!BlockSyncPolicy::IsProvenFraudHeight(200, 2000, 100, 1000, seconds(10)));
	REQUIRE(!BlockSyncPolicy::IsProvenFraudHeight(100, 1000, 100, 1000, seconds(200)));
	REQUIRE(!BlockSyncPolicy::IsProvenFraudHeight(90, 900, 100, 1000, seconds(200)));
	// Height alone is not fraud: the peer must claim more total difficulty.
	REQUIRE(!BlockSyncPolicy::IsProvenFraudHeight(200, 1000, 100, 1000, seconds(120)));
	REQUIRE(BlockSyncPolicy::IsProvenFraudHeight(200, 2000, 100, 1000, seconds(120)));
}

TEST_CASE("BlockSyncPolicy retry chooses alternatives and eventually retries one peer")
{
	std::vector<BlockSyncPolicy::PeerCandidate> peers{ { 200, true } };
	REQUIRE_FALSE(BlockSyncPolicy::SelectRetryPeer(peers, 100, 0, 0, true).has_value());
	peers[0].Slow = false;
	REQUIRE_FALSE(BlockSyncPolicy::SelectRetryPeer(peers, 100, 0, 0, false).has_value());
	REQUIRE(BlockSyncPolicy::SelectRetryPeer(peers, 100, 0, 0, true).value() == 0);
	peers.push_back({ 200, false });
	REQUIRE(BlockSyncPolicy::SelectRetryPeer(peers, 100, 0, 0, true).value() == 1);
	REQUIRE(BlockSyncPolicy::SelectRetryPeer(peers, 100, 0, 0, false).value() == 1);
	peers[1].Height = 99;
	REQUIRE(BlockSyncPolicy::SelectRetryPeer(peers, 100, 0, 0, true).value() == 0);
	REQUIRE_FALSE(BlockSyncPolicy::SelectRetryPeer(peers, 201, 0, 0, true).has_value());
	REQUIRE_FALSE(BlockSyncPolicy::SelectRetryPeer({}, 100, 0, std::nullopt, true).has_value());
}

TEST_CASE("SlowPeerTracker::CooldownExpiresAndProgressClearsIt")
{
	SlowPeerTracker tracker;
	const auto now = SlowPeerTracker::Clock::now();

	REQUIRE(!tracker.IsSlow("1.2.3.4", now));
	tracker.MarkSlow("1.2.3.4", now);
	REQUIRE(tracker.IsSlow("1.2.3.4", now));
	REQUIRE(tracker.Size() == 1);

	// Cooldown is not a ban: it expires after the TTL.
	REQUIRE(!tracker.IsSlow("1.2.3.4", now + BlockSyncPolicy::SLOW_PEER_COOLDOWN + std::chrono::seconds(1)));

	tracker.MarkSlow("1.2.3.4", now);
	tracker.MarkProgress("1.2.3.4");
	REQUIRE(!tracker.IsSlow("1.2.3.4", now));
	REQUIRE(tracker.Size() == 0);
}

TEST_CASE("SyncStatus::BlockSyncCountersAndWaitingReason")
{
	SyncStatus status;
	REQUIRE(status.GetBlockRequests() == 0);

	status.IncrementBlockRequests(3);
	status.IncrementBlockTimeouts(2);
	status.IncrementBlockReassignments(1);
	status.IncrementBlockCooldownReassignments(1);
	status.IncrementBlockRetries(2);
	status.IncrementBansFraudHeight(4);
	status.IncrementBansInvalidData(5);
	REQUIRE(status.GetBlockRequests() == 3);
	REQUIRE(status.GetBlockTimeouts() == 2);
	REQUIRE(status.GetBlockReassignments() == 1);
	REQUIRE(status.GetBlockCooldownReassignments() == 1);
	REQUIRE(status.GetBlockRetries() == 2);
	REQUIRE(status.GetBansFraudHeight() == 4);
	REQUIRE(status.GetBansInvalidData() == 5);

	status.SetWaitingReason("no most-work peer connection");
	REQUIRE(status.GetWaitingReason() == "no most-work peer connection");
	REQUIRE(status.GetCurrentWaitingReason().empty());
	status.UpdateStatus(ESyncStatus::WAITING_FOR_PEERS);
	REQUIRE(status.GetCurrentWaitingReason() == "no most-work peer connection");
	status.UpdateStatus(ESyncStatus::SYNCING_HEADERS);
	REQUIRE(status.GetCurrentWaitingReason().empty());

	SyncStatus copy(status);
	REQUIRE(copy.GetBlockRequests() == 3);
	REQUIRE(copy.GetBlockCooldownReassignments() == 1);
	REQUIRE(copy.GetBlockRetries() == 2);
	REQUIRE(copy.GetWaitingReason() == "no most-work peer connection");
}

TEST_CASE("Peer rate policy protects incoming traffic and paces header requests")
{
    using namespace std::chrono;
    const PeerRatePolicy::Clock::time_point start{};
    const auto next = start + PeerRatePolicy::MIN_HEADER_REQUEST_INTERVAL;
    REQUIRE_FALSE(PeerRatePolicy::HeaderRequestDue(start + milliseconds(199), next));
    REQUIRE(PeerRatePolicy::HeaderRequestDue(start + milliseconds(200), next));
    REQUIRE_FALSE(PeerRatePolicy::IsAbusive(500));
    REQUIRE(PeerRatePolicy::IsAbusive(501));
    // A full minute of locally paced requests leaves room below the peer
    // abuse limit for ping/pong and other valid inbound messages.
    REQUIRE(static_cast<size_t>(duration_cast<milliseconds>(minutes(1)).count() /
        PeerRatePolicy::MIN_HEADER_REQUEST_INTERVAL.count()) < PeerRatePolicy::MAX_RECEIVED_PER_MINUTE);
}

TEST_CASE("Console distinguishes PIBD segment progress from legacy ZIP bytes")
{
    SyncStatus status;
    status.UpdateDownloaded(12);
    status.UpdateDownloadSize(24);
    REQUIRE(FormatTxHashSetProgress(status) == "Status: Syncing TxHashSet 12/24(50%)");
    status.SetPibdStarted(1);
    status.SetPibdProgress(0, 0);
    REQUIRE(FormatTxHashSetProgress(status) == "Status: Syncing PIBD segments 0/pending");
    status.SetPibdProgress(3, 10);
    REQUIRE(FormatTxHashSetProgress(status) == "Status: Syncing PIBD segments 3/10 (30%)");
}