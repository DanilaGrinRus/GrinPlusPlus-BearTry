#pragma once

#include "BlockSyncPolicy.h"
#include "PeerRatePolicy.h"
#include "PihdClient.h"
#include "../ConnectionManager.h"

#include <BlockChain/BlockChain.h>
#include <chrono>

// Forward Declarations
class SyncStatus;

class HeaderSyncer
{
public:
	HeaderSyncer(
		const std::weak_ptr<ConnectionManager>& pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		const std::shared_ptr<PihdClient>& pPihdClient = nullptr)
		: m_pConnectionManager(pConnectionManager), m_pBlockChain(pBlockChain), m_pPihdClient(pPihdClient)
	{
		m_timeout = std::chrono::system_clock::now();
		m_lastHeight = 0;
		m_pPeer = nullptr;
		m_retried = false;
		m_lastProgress = BlockSyncPolicy::Clock::now();
		m_lastProgressHeight = 0;
	}

	bool SyncHeaders(const SyncStatus& syncStatus, const bool startup);

private:
	bool IsHeaderSyncDue(const SyncStatus& syncStatus);
	bool RequestHeaders(const SyncStatus& syncStatus);

	std::weak_ptr<ConnectionManager> m_pConnectionManager;
	IBlockChain::Ptr m_pBlockChain;
	std::shared_ptr<PihdClient> m_pPihdClient;

	std::chrono::time_point<std::chrono::system_clock> m_timeout;
	uint64_t m_lastHeight;
	PeerPtr m_pPeer;
	bool m_retried;
	PeerRatePolicy::Clock::time_point m_nextHeaderRequest{};
	BlockSyncPolicy::Clock::time_point m_lastProgress;
	uint64_t m_lastProgressHeight;
};