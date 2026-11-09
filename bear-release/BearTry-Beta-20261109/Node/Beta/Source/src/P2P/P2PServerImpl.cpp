#include "P2PServerImpl.h"

#include "Seed/Seeder.h"
#include "Pipeline/Pipeline.h"
#include "PIBD/PibdSync.h"
#include <Common/Util/FileUtil.h>
#include "Sync/Syncer.h"
#include "Sync/PihdClient.h"
#include "Messages/TransactionKernelMessage.h"

#include <Core/Context.h>
#include <BlockChain/BlockChain.h>

P2PServer::P2PServer(
	SyncStatusConstPtr pSyncStatus,
	const std::shared_ptr<Locked<PeerManager>>& pPeerManager,
	ConnectionManagerPtr pConnectionManager,
	std::shared_ptr<Pipeline> pPipeline,
	std::unique_ptr<Seeder>&& pSeeder,
	std::shared_ptr<Syncer> pSyncer,
	std::shared_ptr<Dandelion> pDandelion)
	: m_pSyncStatus(pSyncStatus),
	m_pPeerManager(pPeerManager),
	m_pConnectionManager(pConnectionManager),
	m_pPipeline(pPipeline),
	m_pSeeder(std::move(pSeeder)),
	m_pSyncer(pSyncer),
	m_pDandelion(pDandelion)
{

}

P2PServer::~P2PServer()
{
	LOG_INFO("Shutting down P2P server");
	m_pDandelion.reset();
	m_pSyncer.reset();
	m_pSeeder.reset();
	m_pPipeline.reset();
	m_pConnectionManager->Shutdown();
}

std::shared_ptr<P2PServer> P2PServer::Create(
	const Context::Ptr& pContext,
	const std::shared_ptr<IBlockChain>& pBlockChain,
	std::shared_ptr<Locked<TxHashSetManager>> pTxHashSetManager,
	std::shared_ptr<IDatabase> pDatabase,
	const ITransactionPool::Ptr& pTransactionPool)
{
	// Sync Status: seed the handshake/Ping values from the loaded chain. The
	// header status feeds Hand/Shake and Ping advertisement (header head, i.e.
	// CANDIDATE, matching the headers served by LocateHeaders); the block status
	// remains the confirmed body tip for body-sync accounting.
	//
	// Seed from ONE tip-header snapshot per chain, never from split
	// GetHeight+GetTotalDifficulty reads: two calls can straddle a tip change and
	// advertise a mixed height/difficulty pair.
	SyncStatusPtr pSyncStatus(new SyncStatus());
	const auto pCandidateHead = pBlockChain->GetTipBlockHeader(EChainType::CANDIDATE);
	if (pCandidateHead != nullptr)
	{
		pSyncStatus->UpdateHeaderStatus(pCandidateHead->GetHeight(), pCandidateHead->GetTotalDifficulty());
	}
	// If the candidate tip header is not readable yet, leave the header status at
	// zero; the sync loop refreshes it from ChainState under a read lease and
	// Hand/Shake/Ping will use that coherent snapshot instead of a split read.

	const auto pConfirmedHead = pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
	if (pConfirmedHead != nullptr)
	{
		pSyncStatus->UpdateBlockStatus(
			pConfirmedHead->GetHeight(),
			pConfirmedHead->GetTotalDifficulty(),
			pConfirmedHead->GetTimestamp());
	}

	// Peer Manager
	std::shared_ptr<Locked<PeerManager>> peerManager = PeerManager::Create(
		pContext,
		pDatabase->GetPeerDB()
	);

	// Connection Manager
	ConnectionManagerPtr pConnectionManager = ConnectionManager::Create();
	auto pPihdClient = std::make_shared<PihdClient>(pConnectionManager, pBlockChain);

	const Config& config = pContext->GetConfig();

	// Pipeline
	std::shared_ptr<Pipeline> pPipeline = Pipeline::Create(
		config,
		pConnectionManager,
		pBlockChain,
		pSyncStatus
	);

	// PIBD state sync service (download -> desegmenter -> activation).
	const fs::path txHashSetPath = config.GetTxHashSetPath();
	const fs::path pibdStagingPath = txHashSetPath.parent_path() / "pibd_staging";
	const fs::path pibdResumePath = txHashSetPath.parent_path() / "pibd_resume";
	std::shared_ptr<PibdSync> pPibdSync =
		std::make_shared<PibdSync>(txHashSetPath, pibdStagingPath, pibdResumePath);

	// Seeder
	std::unique_ptr<Seeder> pSeeder = Seeder::Create(
		*pConnectionManager,
		*peerManager,
		pBlockChain,
		pPipeline,
		pSyncStatus,
		config,
		pPibdSync,
		pPihdClient
	);

	// Syncer
	std::shared_ptr<Syncer> pSyncer = Syncer::Create(
		pConnectionManager,
		pBlockChain,
		pPipeline,
		pSyncStatus,
		pPibdSync,
		pPihdClient
	);

	// Dandelion
	std::shared_ptr<Dandelion> pDandelion = Dandelion::Create(
		config,
		*pConnectionManager,
		pBlockChain,
		pTxHashSetManager,
		pTransactionPool,
		pDatabase->GetBlockDB()
	);

	// P2P Server
	return std::shared_ptr<P2PServer>(new P2PServer(
		pSyncStatus,
		peerManager,
		pConnectionManager,
		pPipeline,
		std::move(pSeeder),
		pSyncer,
		pDandelion
	));
}

std::pair<size_t, size_t> P2PServer::GetNumberOfConnectedPeers() const
{
	return std::make_pair(
		m_pConnectionManager->GetNumInbound(),
		m_pConnectionManager->GetNumOutbound()
	);
}

std::vector<PeerConstPtr> P2PServer::GetAllPeers() const
{
	return m_pPeerManager->Read()->GetAllPeers();
}

std::vector<ConnectedPeer> P2PServer::GetConnectedPeers() const
{
	return m_pConnectionManager->GetConnectedPeers();
}

std::optional<PeerConstPtr> P2PServer::GetPeer(const IPAddress& address) const
{
	return m_pPeerManager->Read()->GetPeer(address);
}

void P2PServer::BanPeer(const IPAddress& address, const EBanReason banReason)
{
	m_pPeerManager->Write()->BanPeer(address, banReason);
}

void P2PServer::UnbanPeer(const IPAddress& address)
{
	m_pPeerManager->Write()->UnbanPeer(address);
}

bool P2PServer::UnbanAllPeers()
{
	m_pPeerManager->Write()->UnbanAllPeers();
	return true;
}

void P2PServer::BroadcastTransaction(const TransactionPtr& pTransaction)
{
	for (auto& kernel : pTransaction->GetKernels())
	{
		const TransactionKernelMessage message(kernel.GetHash());
		m_pConnectionManager->BroadcastMessage(message, 0);
	}
}

namespace P2PAPI
{
	std::shared_ptr<IP2PServer> StartP2PServer(
		const Context::Ptr& pContext,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<Locked<TxHashSetManager>> pTxHashSetManager,
		IDatabasePtr pDatabase,
		const ITransactionPool::Ptr& pTransactionPool)
	{
		return P2PServer::Create(pContext, pBlockChain, pTxHashSetManager, pDatabase, pTransactionPool);
	}
}
