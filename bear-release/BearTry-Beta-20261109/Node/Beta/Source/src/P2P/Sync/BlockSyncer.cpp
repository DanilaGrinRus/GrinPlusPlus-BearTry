#include "BlockSyncer.h"

#include "../ConnectionManager.h"
#include "../Messages/GetBlockMessage.h"

#include <Common/Logger.h>
#include <Common/Util/StringUtil.h>
#include <Crypto/CSPRNG.h>
#include <P2P/SyncStatus.h>

namespace
{
	std::string PeerKey(const PeerPtr& pPeer)
	{
		return pPeer != nullptr ? pPeer->GetIPAddress().Format() : std::string();
	}
}

bool BlockSyncer::SyncBlocks(const SyncStatus& syncStatus, const bool startup)
{
	const uint64_t chainHeight = syncStatus.GetBlockHeight();
	const uint64_t networkHeight = syncStatus.GetNetworkHeight();

	if (networkHeight >= (chainHeight + 5) || (startup && networkHeight > chainHeight)) {
		if (IsBlockSyncDue(syncStatus)) {
			RequestBlocks(syncStatus);
			m_lastHeight = chainHeight;
		}

		return true;
	}

	return false;
}

bool BlockSyncer::IsBlockSyncDue(const SyncStatus& syncStatus)
{
	const uint64_t chainHeight = syncStatus.GetBlockHeight();

	const uint64_t blocksDownloaded =
		chainHeight > m_lastHeight ? (chainHeight - m_lastHeight) : 0;
	if (blocksDownloaded > 0) {
		m_progressSinceLastRequest = true;
		LOG_TRACE_F("{} blocks received since last check.", blocksDownloaded);
	}

	// Remove downloaded blocks from the queue and clear their peers' cooldown.
	for (uint64_t i = m_lastHeight; i <= chainHeight; i++) {
		const auto iter = m_requestedBlocks.find(i);
		if (iter != m_requestedBlocks.cend()) {
			m_slowPeers.MarkProgress(PeerKey(iter->second.PEER));
			m_requestedBlocks.erase(iter);
		}
	}

	m_lastHeight = chainHeight;
	m_slowPeers.Cleanup(BlockSyncPolicy::Clock::now());

	// Check if blocks were received, and we're ready to request next batch.
	if (m_requestedBlocks.size() < BlockSyncPolicy::REQUEST_WINDOW) {
		LOG_TRACE("Requesting next batch.");
		return true;
	}

	// Make sure we have valid requests for the first window of blocks.
	const std::vector<std::pair<uint64_t, Hash>> blocksNeeded =
		m_pBlockChain->GetBlocksNeeded(BlockSyncPolicy::REQUEST_WINDOW);
	for (const auto& needed : blocksNeeded) {
		const auto requestedBlock = m_requestedBlocks.find(needed.first);
		if (requestedBlock == m_requestedBlocks.cend()) {
			LOG_TRACE_F("Block {} not requested yet.", needed.first);
			return true;
		}

		const std::string key = PeerKey(requestedBlock->second.PEER);
		if (m_slowPeers.IsSlow(key, BlockSyncPolicy::Clock::now())) {
			LOG_TRACE_F("Waiting on block {} from cooling-down peer {}.", needed.first, key);
			return true;
		}

		if (requestedBlock->second.TIMEOUT < BlockSyncPolicy::Clock::now()) {
			if (m_pPipeline->GetBlockPipe()->IsProcessingBlock(needed.second)) {
				continue;
			}

			// Count and log a timeout once per request, not on every sync tick.
			if (!requestedBlock->second.TIMEOUT_COUNTED) {
				requestedBlock->second.TIMEOUT_COUNTED = true;
				syncStatus.IncrementBlockTimeouts();
				m_slowPeers.MarkSlow(key, BlockSyncPolicy::Clock::now());
				LOG_WARNING_F("Block {} request to {} exceeded its deadline.", needed.first, key);
			}
			return true;
		}
	}

	return false;
}

bool BlockSyncer::RequestBlocks(const SyncStatus& syncStatus)
{
	LOG_TRACE("Requesting blocks.");

	const auto pConnectionManager = m_pConnectionManager.lock();
	if (pConnectionManager == nullptr) {
		return false;
	}

	const std::vector<ConnectionPtr> connections = pConnectionManager->GetMostWorkConnections();
	if (connections.empty()) {
		LOG_DEBUG("No most-work peers found.");
		syncStatus.SetWaitingReason("block sync: no most-work peer connection");
		return false;
	}

	const auto steadyNow = BlockSyncPolicy::Clock::now();

	std::vector<BlockSyncPolicy::PeerCandidate> candidates;
	candidates.reserve(connections.size());
	for (const ConnectionPtr& pConnection : connections) {
		BlockSyncPolicy::PeerCandidate candidate;
		candidate.Height = pConnection->GetHeight();
		candidate.Slow = m_slowPeers.IsSlow(pConnection->GetIPAddress().Format(), steadyNow);
		candidates.push_back(candidate);
	}

	const uint32_t batch = BlockSyncPolicy::ComputeBatchLimit(connections.size());
	const uint64_t networkHeight = syncStatus.GetNetworkHeight();

	std::vector<std::pair<uint64_t, Hash>> blocksNeeded =
		m_pBlockChain->GetBlocksNeeded((uint64_t)batch * 2);
	if (blocksNeeded.empty()) {
		LOG_TRACE("No blocks needed.");
		return false;
	}

	size_t rotation = 0;
	if (connections.size() > 1) {
		rotation = CSPRNG::GenerateRandom(0, connections.size() - 1);
	}

	std::vector<std::pair<uint64_t, Hash>> blocksToRequest;
	blocksToRequest.reserve(batch);

	for (size_t blockIndex = 0; blockIndex < blocksNeeded.size(); blockIndex++) {
		const uint64_t height = blocksNeeded[blockIndex].first;
		const Hash& hash = blocksNeeded[blockIndex].second;

		// Never ask for blocks beyond the height the network told us about.
		if (networkHeight != 0 && height > networkHeight) {
			break;
		}

		if (m_pPipeline->GetBlockPipe()->IsProcessingBlock(hash) ||
			m_pBlockChain->HasBlock(height, hash)) {
			continue;
		}

		const auto requested = m_requestedBlocks.find(height);
		if (requested != m_requestedBlocks.end()) {
			const std::string oldKey = PeerKey(requested->second.PEER);
			const bool deadlineExpired = requested->second.TIMEOUT < steadyNow;
			const bool coolingDown = m_slowPeers.IsSlow(oldKey, steadyNow);
			if (!deadlineExpired && !coolingDown) {
				continue;
			}

			if (m_pPipeline->GetBlockPipe()->IsProcessingBlock(hash)) {
				continue;
			}

			if (deadlineExpired && !requested->second.TIMEOUT_COUNTED) {
				requested->second.TIMEOUT_COUNTED = true;
				syncStatus.IncrementBlockTimeouts();
				m_slowPeers.MarkSlow(oldKey, steadyNow);
				LOG_WARNING_F("Block {} request to {} exceeded its deadline (attempt {}).",
					height, oldKey, requested->second.ATTEMPTS + 1);
			}

			bool reassigned = false;
			std::optional<size_t> original;
			for (size_t i = 0; i < connections.size(); ++i) {
				const std::string key = connections[i]->GetIPAddress().Format();
				candidates[i].Slow = m_slowPeers.IsSlow(key, steadyNow);
				if (key == oldKey) original = i;
			}
			for (size_t attempt = 0; attempt < connections.size(); attempt++) {
				const size_t start = (rotation + attempt + 1) % connections.size();
				const auto index = BlockSyncPolicy::SelectRetryPeer(candidates, height, start, original, deadlineExpired);
				if (!index.has_value()) {
					break;
				}

				const ConnectionPtr& pConnection = connections[*index];

				const GetBlockMessage getBlockMessage(hash);
				if (pConnectionManager->SendMessageToPeer(getBlockMessage, pConnection->GetPeer())) {
					RequestedBlock updated = requested->second;
					updated.PEER = pConnection->GetPeer();
					updated.TIMEOUT = steadyNow +
						BlockSyncPolicy::RetryBackoff(
							m_progressSinceLastRequest,
							requested->second.ATTEMPTS + 1);
					updated.RETRIED = true;
					updated.TIMEOUT_COUNTED = false;
					updated.ATTEMPTS = requested->second.ATTEMPTS + 1;
					const uint32_t attemptNumber = updated.ATTEMPTS + 1;
					m_requestedBlocks[height] = std::move(updated);
					syncStatus.IncrementBlockRetries();
					if (pConnection->GetIPAddress().Format() != oldKey) {
						syncStatus.IncrementBlockReassignments();
						if (!deadlineExpired) syncStatus.IncrementBlockCooldownReassignments();
					}
					LOG_DEBUG_F("Block {} retry: {} -> {}, reason {}, attempt {}.", height, oldKey,
						pConnection->GetIPAddress().Format(), deadlineExpired ? "deadline" : "cooldown", attemptNumber);
					rotation = (*index + 1) % connections.size();
					reassigned = true;
					break;
				}

				m_slowPeers.MarkSlow(pConnection->GetIPAddress().Format(), steadyNow);
				candidates[*index].Slow = true;
			}

			if (!reassigned) {
				LOG_DEBUG_F("Block {} has no alternative peer right now; will retry.", height);
			}

			continue;
		}

		blocksToRequest.emplace_back(blocksNeeded[blockIndex]);
		if (blocksToRequest.size() >= batch) {
			break;
		}
	}

	size_t assigned = 0;
	for (const auto& block : blocksToRequest) {
		const auto index = BlockSyncPolicy::SelectPeer(candidates, block.first, rotation);
		if (!index.has_value()) {
			// All connected peers reported a height below this block; leave it
			// pending and try again once a taller peer connects or they catch up.
			LOG_DEBUG_F(
				"No peer can serve block {} yet (requested up to {}).",
				block.first,
				networkHeight);
			continue;
		}

		const ConnectionPtr& pConnection = connections[*index];
		const GetBlockMessage getBlockMessage(block.second);
		if (!pConnectionManager->SendMessageToPeer(getBlockMessage, pConnection->GetPeer())) {
			m_slowPeers.MarkSlow(pConnection->GetIPAddress().Format(), steadyNow);
			continue;
		}

		RequestedBlock blockRequested;
		blockRequested.PEER = pConnection->GetPeer();
		blockRequested.BLOCK_HEIGHT = block.first;
		blockRequested.TIMEOUT = steadyNow +
			BlockSyncPolicy::RetryDelay(m_progressSinceLastRequest);
		blockRequested.RETRIED = false;
		blockRequested.TIMEOUT_COUNTED = false;
		blockRequested.ATTEMPTS = 0;

		m_requestedBlocks[block.first] = std::move(blockRequested);
		syncStatus.IncrementBlockRequests();
		LOG_DEBUG_F("Block {} requested from {}, deadline in {}s.", block.first,
			pConnection->GetIPAddress().Format(), BlockSyncPolicy::RetryDelay(m_progressSinceLastRequest).count());
		assigned++;
		rotation = (*index + 1) % connections.size();
	}

	m_progressSinceLastRequest = false;

	if (assigned > 0) {
		LOG_TRACE_F("{} blocks requested from {} peers.", assigned, connections.size());
	}

	return true;
}
