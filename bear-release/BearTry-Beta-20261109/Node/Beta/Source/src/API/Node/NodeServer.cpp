#include <API/Node/NodeServer.h>

#include "Handlers/GetConfigHandler.h"
#include "Handlers/UpdateConfigHandler.h"
#include "Handlers/GetHeaderHandler.h"
#include "Handlers/GetBlockHandler.h"
#include "Handlers/GetVersionHandler.h"
#include "Handlers/GetTipHandler.h"
#include "Handlers/PushTransactionHandler.h"
#include "Handlers/GetPoolSizeHandler.h"
#include "Handlers/GetStempoolSizeHandler.h"
#include "Handlers/GetUnconfirmedTransactionsHandler.h"
#include "Handlers/GetStatusOwnerHandler.h"
#include "Handlers/GetPeersOwnerHandler.h"
#include "Handlers/GetConnectedPeersOwnerHandler.h"
#include "Handlers/PeerControlOwnerHandler.h"
#include "Handlers/GetPmmrIndicesHandler.h"
#include "Handlers/GetUnspentOutputsHandler.h"
#include "Handlers/GetOutputsHandler.h"
#include "Handlers/GetKernelHandler.h"
#include "Handlers/GetBlocksHandler.h"

NodeServer::UPtr NodeServer::Create(
    const ServerPtr& pServer,
    const IBlockChain::Ptr& pBlockChain,
    const IP2PServerPtr& pP2PServer,
    const ITransactionPool::Ptr& pTransactionPool,
    const TxHashSetManager::Ptr& pTxHashSetManager,
    const IDatabasePtr& pDatabase)
{
    RPCServer::Ptr pForeignServer = RPCServer::Create(pServer, "/v2/foreign", LoggerAPI::LogFile::NODE);
    pForeignServer->AddMethod("get_header", std::make_shared<GetHeaderHandler>(pBlockChain));
    pForeignServer->AddMethod("get_block", std::make_shared<GetBlockHandler>(pBlockChain));
    pForeignServer->AddMethod("get_version", std::make_shared<GetVersionHandler>(pBlockChain));
    pForeignServer->AddMethod("get_tip", std::make_shared<GetTipHandler>(pBlockChain));
    pForeignServer->AddMethod("push_transaction", std::make_shared<PushTransactionHandler>(pBlockChain, pP2PServer));

    pForeignServer->AddMethod("get_pool_size", std::make_shared<GetPoolSizeHandler>(pTransactionPool));
    pForeignServer->AddMethod("get_stempool_size", std::make_shared<GetStempoolSizeHandler>(pTransactionPool));
    pForeignServer->AddMethod("get_unconfirmed_transactions", std::make_shared<GetUnconfirmedTransactionsHandler>(pTransactionPool));
    pForeignServer->AddMethod("get_pmmr_indices", std::make_shared<GetPmmrIndicesHandler>(pBlockChain));
    pForeignServer->AddMethod("get_unspent_outputs", std::make_shared<GetUnspentOutputsHandler>(pBlockChain, pTxHashSetManager, pDatabase));
    pForeignServer->AddMethod("get_outputs", std::make_shared<GetOutputsHandler>(pBlockChain, pTxHashSetManager, pDatabase));
    pForeignServer->AddMethod("get_kernel", std::make_shared<GetKernelHandler>(pBlockChain, pTxHashSetManager, pDatabase));
    pForeignServer->AddMethod("get_blocks", std::make_shared<GetBlocksHandler>(pBlockChain, pTxHashSetManager, pDatabase));

    pForeignServer->AddMethod("get_config", std::make_shared<GetConfigHandler>());

    RPCServer::Ptr pOwnerServer = RPCServer::Create(pServer, "/v2/owner", LoggerAPI::LogFile::NODE);
    pOwnerServer->AddMethod("get_config", std::make_shared<GetConfigHandler>());
    pOwnerServer->AddMethod("update_config", std::make_shared<UpdateConfigHandler>());
    pOwnerServer->AddMethod("get_status", std::make_shared<GetStatusOwnerHandler>(pBlockChain, pP2PServer));
    pOwnerServer->AddMethod("get_peers", std::make_shared<GetPeersOwnerHandler>(pP2PServer));
    pOwnerServer->AddMethod("get_connected_peers", std::make_shared<GetConnectedPeersOwnerHandler>(pP2PServer));
    pOwnerServer->AddMethod("ban_peer", std::make_shared<PeerControlOwnerHandler>(pP2PServer, true));
    pOwnerServer->AddMethod("unban_peer", std::make_shared<PeerControlOwnerHandler>(pP2PServer, false));

    return std::make_unique<NodeServer>(pForeignServer, pOwnerServer);
}
