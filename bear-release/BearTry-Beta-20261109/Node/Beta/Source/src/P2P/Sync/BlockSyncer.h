#pragma once

#include "BlockSyncPolicy.h"

#include "../ConnectionManager.h"
#include "../Pipeline/Pipeline.h"

#include <BlockChain/BlockChain.h>

#include <chrono>
#include <cstdint>
#include <unordered_map>

// Forward Declarations
class SyncStatus;

// Downloads blocks from peers that can actually serve the requested height.
//
// Policy (REQ_G38_FIX.md):
//   * requests are capped by min(candidate tip, network height, per-connection
//     height) and by a batch limit;
//   * a timeout reassigns the block and counts it, it never bans the peer;
//   * a peer that stopped producing blocks gets a short in-memory cooldown;
//   * repeated retries use backoff and peer rotation.
class BlockSyncer
{
public:
	BlockSyncer(
		const std::weak_ptr<ConnectionManager>& pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		const std::shared_ptr<Pipeline>& pPipeline
	) : m_pConnectionManager(pConnectionManager),
		m_pBlockChain(pBlockChain),
		m_pPipeline(pPipeline)
	{
	}

	bool SyncBlocks(const SyncStatus& syncStatus, const bool startup);

private:
	bool IsBlockSyncDue(const SyncStatus& syncStatus);
	bool RequestBlocks(const SyncStatus& syncStatus);

	std::weak_ptr<ConnectionManager> m_pConnectionManager;
	IBlockChain::Ptr m_pBlockChain;
	std::shared_ptr<Pipeline> m_pPipeline;

	uint64_t m_lastHeight = 0;
	bool m_progressSinceLastRequest = false;

	struct RequestedBlock
	{
		PeerPtr PEER;
		uint64_t BLOCK_HEIGHT = 0;
		std::chrono::time_point<std::chrono::system_clock> TIMEOUT;
		bool RETRIED = false;
		bool TIMEOUT_COUNTED = false;
		uint32_t ATTEMPTS = 0;
	};
	std::unordered_map<uint64_t, RequestedBlock> m_requestedBlocks;
	SlowPeerTracker m_slowPeers;
};
