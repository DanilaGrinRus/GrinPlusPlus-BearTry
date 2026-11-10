#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Policy for block download: what may be requested, which peer may serve it,
// when to retry and when a peer deserves a ban. Kept free of networking types so
// the decisions are unit-testable (REQ_G38_FIX.md §4).
//
// Behaviour mirrored from grin v5.5.2 body_sync.rs / header_sync.rs:
//   * a timeout is never fraud: reassign the block and count it;
//   * FraudHeight requires a peer that claims more work than we have and has
//     made no progress for a long time;
//   * requests are limited by the candidate tip, the network height and the
//     per-connection height;
//   * each block has at least 6 s to arrive, including during progress;
//   * a "slow" peer is only cooling down briefly, never a ban reason.
class BlockSyncPolicy
{
public:
	using Clock = std::chrono::steady_clock;

	static constexpr uint32_t MAX_BATCH = 100;
	static constexpr uint32_t BATCH_PER_PEER = 10;
	static constexpr uint32_t REQUEST_WINDOW = 15;

	static constexpr std::chrono::seconds RETRY_DELAY{ 6 };
	static constexpr std::chrono::seconds SLOW_PEER_COOLDOWN{ 60 };
	static constexpr std::chrono::seconds FRAUD_STALE{ 120 };

	struct PeerCandidate
	{
		uint64_t Height = 0;
		bool Slow = false;
	};

	static uint32_t ComputeBatchLimit(const size_t numPeers) noexcept
	{
		if (numPeers == 0) {
			return 0;
		}

		const uint64_t byPeers = (uint64_t)numPeers * BATCH_PER_PEER;
		return (uint32_t)(std::min)((uint64_t)MAX_BATCH, byPeers);
	}

	// Highest block height at which a request is meaningful right now.
	static uint64_t ComputeRequestCeiling(
		const uint64_t candidateTip,
		const uint64_t networkHeight) noexcept
	{
		if (networkHeight == 0) {
			return candidateTip;
		}

		return (std::min)(candidateTip, networkHeight);
	}

	// Highest block we may ask a specific peer for; 0 means "do not ask".
	static uint64_t ComputePeerRequestCeiling(
		const uint64_t requestCeiling,
		const uint64_t peerHeight) noexcept
	{
		if (peerHeight == 0) {
			return 0;
		}

		return (std::min)(requestCeiling, peerHeight);
	}

	static std::chrono::seconds RetryDelay(const bool /*progressSinceLastAttempt*/) noexcept
	{
		// A batch can contain multiple responses per peer. A one-second deadline
		// per block penalizes later responses while the batch is still flowing.
		return RETRY_DELAY;
	}

	// Exponential backoff for repeated retries of the same block, capped at 60 s.
	static std::chrono::seconds RetryBackoff(
		const bool progressSinceLastAttempt,
		const uint32_t attempts) noexcept
	{
		const uint32_t shift = (std::min)(attempts, (uint32_t)4);
		const int64_t base = RetryDelay(progressSinceLastAttempt).count();
		return std::chrono::seconds((std::min)((int64_t)60, base << shift));
	}

	// Selects a peer for a block at the given height, rotating from rotationStart
	// and skipping peers that are cooling down or cannot serve that height.
	static std::optional<size_t> SelectPeer(
		const std::vector<PeerCandidate>& peers,
		const uint64_t blockHeight,
		const size_t rotationStart) noexcept
	{
		if (peers.empty() || blockHeight == 0) {
			return std::nullopt;
		}

		const size_t count = peers.size();
		for (size_t step = 0; step < count; step++) {
			const size_t index = (rotationStart + step) % count;
			const PeerCandidate& peer = peers[index];
			if (peer.Slow || peer.Height < blockHeight) {
				continue;
			}

			return index;
		}

		return std::nullopt;
	}

	// Prefer a different eligible peer; retry the original only after its
	// deadline and cooldown have elapsed. No retry is sent to a cooled peer.
	static std::optional<size_t> SelectRetryPeer(
		const std::vector<PeerCandidate>& peers, const uint64_t height,
		const size_t rotationStart, const std::optional<size_t> original,
		const bool deadlineExpired) noexcept
	{
		if (peers.empty() || height == 0) return std::nullopt;
		for (size_t step = 0; step < peers.size(); ++step) {
			const size_t index = (rotationStart + step) % peers.size();
			if ((!original.has_value() || index != *original) &&
				!peers[index].Slow && peers[index].Height >= height) return index;
		}
		if (deadlineExpired && original.has_value() && *original < peers.size() &&
			!peers[*original].Slow && peers[*original].Height >= height) return original;
		return std::nullopt;
	}

	// Mirrors grin v5.5.2 header_sync.rs:135-141: fraud is only proven when
	// the peer claims strictly more TOTAL DIFFICULTY than our header head and
	// has been unable to produce headers for FRAUD_STALE seconds. Height alone
	// is not enough (a peer can report a higher height at the same/earlier TD),
	// and the difficulty must be known.
	static bool IsProvenFraudHeight(
		const uint64_t /*peerClaimedHeight*/,
		const uint64_t peerClaimedDifficulty,
		const uint64_t /*ourHeaderHeight*/,
		const uint64_t ourHeaderDifficulty,
		const std::chrono::seconds secondsWithoutProgress) noexcept
	{
		if (secondsWithoutProgress < FRAUD_STALE || peerClaimedDifficulty == 0) {
			return false;
		}

		return peerClaimedDifficulty > ourHeaderDifficulty;
	}
};

// Short cooldown for unproductive peers. Unlike Peer::Ban it is not persisted,
// does not last hours and is cleared as soon as the peer produces a block.
class SlowPeerTracker
{
public:
	using Clock = std::chrono::steady_clock;

	void MarkSlow(const std::string& peer, const Clock::time_point now)
	{
		m_until[peer] = now + BlockSyncPolicy::SLOW_PEER_COOLDOWN;
	}

	void MarkProgress(const std::string& peer)
	{
		m_until.erase(peer);
	}

	bool IsSlow(const std::string& peer, const Clock::time_point now) const
	{
		const auto iter = m_until.find(peer);
		if (iter == m_until.cend()) {
			return false;
		}

		return iter->second > now;
	}

	void Cleanup(const Clock::time_point now)
	{
		for (auto iter = m_until.begin(); iter != m_until.end();) {
			if (iter->second <= now) {
				iter = m_until.erase(iter);
			} else {
				++iter;
			}
		}
	}

	size_t Size() const noexcept { return m_until.size(); }

private:
	std::unordered_map<std::string, Clock::time_point> m_until;
};
