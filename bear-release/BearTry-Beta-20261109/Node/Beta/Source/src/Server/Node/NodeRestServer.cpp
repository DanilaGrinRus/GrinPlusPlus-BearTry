#include "NodeRestServer.h"
#include "NodeContext.h"

#include "API/HeaderAPI.h"
#include "API/BlockAPI.h"
#include "API/ServerAPI.h"
#include "API/ChainAPI.h"
#include "API/PeersAPI.h"
#include "API/TxHashSetAPI.h"

#include <Common/Logger.h>
#include <Core/Global.h>
#include <Net/Util/HTTPUtil.h>
#include <Net/Servers/RPC/ApiAccess.h>

static int Shutdown_Handler(struct mg_connection* conn, void*)
{
	if (HTTPUtil::GetHTTPMethod(conn) != HTTP::EHTTPMethod::POST)
	{
		return HTTPUtil::BuildNotFoundResponse(conn, "Not Found");
	}
	if (!ApiAccess::Check(conn, ApiAccess::Level::ADMIN, Global::GetConfig().GetRestAPIPort()))
	{
		return HTTPUtil::BuildUnauthorizedResponse(conn, "Unauthorized");
	}

	Global::Shutdown();
	return HTTPUtil::BuildSuccessResponse(conn, "");
}

ServerPtr NodeRestServer::StartInitializing(const Config& config, const std::shared_ptr<NodeStartupHealth>& pHealth)
{
	ServerPtr pServer = Server::Create(EServerType::LOCAL, std::make_optional<uint16_t>(config.GetRestAPIPort()));
	pServer->AddListener("/v1/health", NodeStartupHealth::Handle, pHealth.get());
	return pServer;
}

NodeRestServer::UPtr NodeRestServer::Create(const Config& config, std::shared_ptr<NodeContext> pNodeContext, const ServerPtr& pServer, const std::shared_ptr<NodeStartupHealth>& pHealth)
{
	ApiAccess::Initialize(config.GetDataDirectory());
	NodeServer::UPtr pV2Server = NodeServer::Create(
		pServer,
		pNodeContext->m_pBlockChain,
		pNodeContext->m_pP2PServer,
		pNodeContext->m_pTransactionPool,
		pNodeContext->m_pTxHashSetManager,
		pNodeContext->m_pDatabase
	);

	/* Add v1 handlers */
	pServer->AddListener("/v1/status", ServerAPI::GetStatus_Handler, pNodeContext.get());
	pServer->AddListener("/v1/resync", ServerAPI::ResyncChain_Handler, pNodeContext.get());
	pServer->AddListener("/v1/headers/", HeaderAPI::GetHeader_Handler, pNodeContext.get());
	pServer->AddListener("/v1/blocks/", BlockAPI::GetBlock_Handler, pNodeContext.get());
	pServer->AddListener("/v1/chain/outputs/byids", ChainAPI::GetChainOutputsByIds_Handler, pNodeContext.get());
	pServer->AddListener("/v1/chain/outputs/byheight", ChainAPI::GetChainOutputsByHeight_Handler, pNodeContext.get());
	pServer->AddListener("/v1/chain", ChainAPI::GetChain_Handler, pNodeContext.get());
	pServer->AddListener("/v1/peers/all", PeersAPI::GetAllPeers_Handler, pNodeContext.get());
	pServer->AddListener("/v1/peers/connected", PeersAPI::GetConnectedPeers_Handler, pNodeContext.get());
	pServer->AddListener("/v1/peers/diagnostics", PeersAPI::GetDiagnostics_Handler, pNodeContext.get());
	pServer->AddListener("/v1/peers/", PeersAPI::Peer_Handler, pNodeContext.get());
	pServer->AddListener("/v1/txhashset/roots", TxHashSetAPI::GetRoots_Handler, pNodeContext.get());
	pServer->AddListener("/v1/txhashset/lastkernels", TxHashSetAPI::GetLastKernels_Handler, pNodeContext.get());
	pServer->AddListener("/v1/txhashset/lastoutputs", TxHashSetAPI::GetLastOutputs_Handler, pNodeContext.get());
	pServer->AddListener("/v1/txhashset/lastrangeproofs", TxHashSetAPI::GetLastRangeproofs_Handler, pNodeContext.get());
	pServer->AddListener("/v1/txhashset/outputs", TxHashSetAPI::GetOutputs_Handler, pNodeContext.get());
	pServer->AddListener("/v1/shutdown", Shutdown_Handler, pNodeContext.get());
	pServer->AddListener("/v1/", ServerAPI::V1_Handler, pNodeContext.get());

	auto pNodeRestServer = std::make_unique<NodeRestServer>(pNodeContext, pHealth, std::move(pV2Server));
	pHealth->ready.store(true);
	return pNodeRestServer;
}
