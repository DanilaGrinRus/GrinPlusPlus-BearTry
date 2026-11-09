#include "Node.h"
#include "SyncProgressDisplay.h"
#include "NodeRestServer.h"
#include "NodeClients/DefaultNodeClient.h"
#include "../console.h"

#include <Consensus.h>
#include <Core/Context.h>
#include <P2P/P2PServer.h>
#include <Core/Config.h>
#include <BlockChain/BlockChain.h>
#include <Database/Database.h>
#include <Common/Logger.h>
#include <Common/Util/ScopedTiming.h>
#include <PMMR/TxHashSetManager.h>

#include <iostream>
#include <thread>

Node::Node(
	const Context::Ptr& pContext,
	std::unique_ptr<NodeRestServer>&& pNodeRestServer,
	std::shared_ptr<DefaultNodeClient> pNodeClient)
	: m_pContext(pContext),
	m_pNodeRestServer(std::move(pNodeRestServer)),
	m_pNodeClient(pNodeClient)
{

}

Node::~Node()
{
	LOG_INFO("Shutting down node daemon");
}

std::unique_ptr<Node> Node::Create(const Context::Ptr& pContext)
{
	auto pHealth = std::make_shared<NodeStartupHealth>();
	ServerPtr pServer;
	{
		ScopedTiming timing("node_rest_initializing");
		pServer = NodeRestServer::StartInitializing(pContext->GetConfig(), pHealth);
		timing.Success();
	}

	std::shared_ptr<DefaultNodeClient> pNodeClient;
	{
		ScopedTiming timing("node_chain_and_p2p_open");
		pNodeClient = DefaultNodeClient::Create(pContext);
		timing.Success();
	}

	std::unique_ptr<NodeRestServer> pNodeRestServer;
	{
		ScopedTiming timing("node_rest_full");
		pNodeRestServer = NodeRestServer::Create(
			pContext->GetConfig(),
			pNodeClient->GetNodeContext(),
			pServer,
			pHealth
		);
		timing.Success();
	}

	LOG_INFO("startup_timing name=health_and_status_ready outcome=success");

	return std::make_unique<Node>(
		pContext,
		std::move(pNodeRestServer),
		pNodeClient
	);
}

std::shared_ptr<INodeClient> Node::GetNodeClient() const
{
	return m_pNodeClient;
}

void Node::UpdateDisplay(const int secondsRunning)
{
	SyncStatusConstPtr pSyncStatus = m_pNodeClient->GetP2PServer()->GetSyncStatus();

	IO::Clear();

	std::cout << "Time Running: " << secondsRunning << "s";

	const ESyncStatus status = pSyncStatus->GetStatus();
	if (status == ESyncStatus::NOT_SYNCING)
	{
		std::cout << "\nStatus: Running";
	}
	else if (status == ESyncStatus::WAITING_FOR_PEERS)
	{
		std::cout << "\nStatus: Waiting for Peers";
	}
	else if (status == ESyncStatus::SYNCING_HEADERS)
	{
		const uint64_t networkHeight = pSyncStatus->GetNetworkHeight();
		const uint64_t percentage = networkHeight > 0 ? (pSyncStatus->GetHeaderHeight() * 100 / networkHeight) : 0;
		std::cout << "\nStatus: Syncing Headers (" << percentage << "%)";
	}
	else if (status == ESyncStatus::SYNCING_TXHASHSET)
	{
		std::cout << "\n" << FormatTxHashSetProgress(*pSyncStatus);
	}
	else if (status == ESyncStatus::PROCESSING_TXHASHSET)
	{
		std::cout << "\nStatus: Validating TxHashSet (" << (int)pSyncStatus->GetProcessingStatus() << "%)";
	}
	else if (status == ESyncStatus::TXHASHSET_SYNC_FAILED)
	{
		std::cout << "\nStatus: TxHashSet Sync Failed - Trying Again";
	}
	else if (status == ESyncStatus::SYNCING_BLOCKS)
	{
		std::cout << "\nStatus: Syncing blocks";
	}

	std::cout << "\nNumConnections: " << pSyncStatus->GetNumActiveConnections();
	std::cout << "\nHeader Height: " << pSyncStatus->GetHeaderHeight();
	std::cout << "\nHeader Difficulty: " << pSyncStatus->GetHeaderDifficulty();
	std::cout << "\nBlock Height: " << pSyncStatus->GetBlockHeight();
	std::cout << "\nBlock Difficulty: " << pSyncStatus->GetBlockDifficulty();
	std::cout << "\nNetwork Height: " << pSyncStatus->GetNetworkHeight();
	std::cout << "\nNetwork Difficulty: " << pSyncStatus->GetNetworkDifficulty();
	std::cout << "\n\nPress Ctrl-C to exit...";

	IO::Flush();
}