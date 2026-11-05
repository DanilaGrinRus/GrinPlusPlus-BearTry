#pragma once

#include "../ConnectionManager.h"
#include "../Pipeline/Pipeline.h"

#include <P2P/SyncStatus.h>
#include <BlockChain/BlockChain.h>
#include <atomic>
#include <thread>

// Forward Declarations
class SyncStatus;
class PibdSync;
class PihdClient;

class Syncer
{
public:
	static std::shared_ptr<Syncer> Create(
		std::weak_ptr<ConnectionManager> pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<Pipeline> pPipeline,
		SyncStatusPtr pSyncStatus,
		std::shared_ptr<PibdSync> pPibdSync,
		std::shared_ptr<PihdClient> pPihdClient = nullptr
	);
	~Syncer();

private:
	Syncer(
		std::weak_ptr<ConnectionManager> pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<Pipeline> pPipeline,
		SyncStatusPtr pSyncStatus,
		std::shared_ptr<PibdSync> pPibdSync,
		std::shared_ptr<PihdClient> pPihdClient
	);

	static void Thread_Sync(Syncer& syncer);
	void UpdateSyncStatus();

	std::weak_ptr<ConnectionManager> m_pConnectionManager;
	IBlockChain::Ptr m_pBlockChain;
	std::shared_ptr<Pipeline> m_pPipeline;
	SyncStatusPtr m_pSyncStatus;
	std::shared_ptr<PibdSync> m_pPibdSync;
	std::shared_ptr<PihdClient> m_pPihdClient;

	std::atomic<bool> m_terminate;
	std::thread m_syncThread;
};