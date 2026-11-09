#include "ServerAPI.h"
#include <API/Node/NodeStatus.h>
#include <BlockChain/DbRecovery.h>
#include "../NodeContext.h"

#include <Core/Global.h>
#include <Net/Util/HTTPUtil.h>
#include <Net/Servers/RPC/ApiAccess.h>
#include <P2P/Common.h>
#include <json/json.h>

int ServerAPI::V1_Handler(struct mg_connection* conn, void*)
{
	if (HTTPUtil::GetHTTPMethod(conn) == HTTP::EHTTPMethod::GET)
	{
		Json::Value json;
		json.append("GET /v1/status");
		json.append("GET|POST /v1/resync");
		json.append("POST /v1/shutdown");
		json.append("GET /v1/headers/<hash|height>");
		json.append("GET /v1/blocks/<hash|height|output commit>?compact");
		json.append("GET /v1/chain");
		json.append("GET /v1/chain/outputs/byids?id=xxx,yyy&id=zzz");
		json.append("GET /v1/chain/outputs/byheight?start_height=100&end_height=200");
		json.append("GET /v1/peers/all");
		json.append("GET /v1/peers/connected");
		json.append("GET /v1/peers/diagnostics");
		json.append("GET /v1/peers/<a.b.c.d>");
		json.append("POST /v1/peers/<a.b.c.d>/ban");
		json.append("POST /v1/peers/<a.b.c.d>/unban");
		json.append("GET /v1/txhashset/roots");
		json.append("GET /v1/txhashset/lastkernels?n=###");
		json.append("GET /v1/txhashset/lastoutputs?n=###");
		json.append("GET /v1/txhashset/lastrangeproofs?n=###");
		json.append("GET /v1/txhashset/outputs?start_index=1&max=100");
		json.append("GET /v1/");

		return HTTPUtil::BuildSuccessResponse(conn, json.toStyledString());
	}
	else
	{
		return HTTPUtil::BuildNotFoundResponse(conn, "Not Found");
	}
}

int ServerAPI::GetStatus_Handler(struct mg_connection* conn, void* pNodeContext)
{
	NodeContext* pServer = (NodeContext*)pNodeContext;
	
	auto pTip = pServer->m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
	if (pTip == nullptr)
	{
		return HTTPUtil::BuildInternalErrorResponse(conn, "Failed to find tip.");
	}

	Json::Value statusNode;
	statusNode["protocol_version"] = P2P::PROTOCOL_VERSION;
	statusNode["user_agent"] = P2P::USER_AGENT;

	SyncStatusConstPtr pSyncStatus = pServer->m_pP2PServer->GetSyncStatus();
	statusNode["sync_status"] = GetStatusString(*pSyncStatus);
	statusNode["waiting_reason"] = pSyncStatus->GetWaitingReason();
	statusNode["db_repaired"] = Json::UInt64(DbRecovery::GetRepairCount());
	statusNode["db_last_repair"] = Json::Int64(DbRecovery::GetLastRepairTime());

	Json::Value blockSyncNode;
	blockSyncNode["requests"] = Json::UInt64(pSyncStatus->GetBlockRequests());
	blockSyncNode["timeouts"] = Json::UInt64(pSyncStatus->GetBlockTimeouts());
	blockSyncNode["reassignments"] = Json::UInt64(pSyncStatus->GetBlockReassignments());
	blockSyncNode["bans_fraud_height"] = Json::UInt64(pSyncStatus->GetBansFraudHeight());
	blockSyncNode["bans_invalid_data"] = Json::UInt64(pSyncStatus->GetBansInvalidData());
	statusNode["block_sync"] = blockSyncNode;

	Json::Value stateNode;
	stateNode["downloaded"] = pSyncStatus->GetDownloaded();
	stateNode["download_size"] = pSyncStatus->GetDownloadSize();
	stateNode["processing_status"] = pSyncStatus->GetProcessingStatus();
	statusNode["state"] = stateNode;

	Json::Value networkNode;
	networkNode["height"] = pSyncStatus->GetNetworkHeight();
	networkNode["total_difficulty"] = pSyncStatus->GetNetworkDifficulty();
	const std::pair<size_t, size_t> numConnections = pServer->m_pP2PServer->GetNumberOfConnectedPeers();
	networkNode["num_inbound"] = Json::UInt64(numConnections.first);
	networkNode["num_outbound"] = Json::UInt64(numConnections.second);
	statusNode["network"] = networkNode;

	Json::Value tipNode;
	tipNode["height"] = pTip->GetHeight();
	tipNode["hash"] = pTip->GetHash().ToHex();
	tipNode["previous_hash"] = pTip->GetPreviousHash().ToHex();
	tipNode["total_difficulty"] = pTip->GetTotalDifficulty();
	statusNode["chain"] = tipNode;

	const uint64_t headerHeight = pServer->m_pBlockChain->GetHeight(EChainType::CANDIDATE);
	statusNode["header_height"] = headerHeight;

	// Compatibility contract: keep all legacy fields above for existing
	// clients, and expose the exact Rust Status object under "rust". This avoids
	// two different builders for the same live snapshot.
	const Json::Value rustStatus = NodeStatus::Build(pServer->m_pBlockChain, pServer->m_pP2PServer);
	if (!rustStatus.isNull()) {
		statusNode["rust"] = rustStatus;
		statusNode["tip"] = rustStatus["tip"];
		statusNode["connections"] = rustStatus["connections"];
		statusNode["chain_type"] = rustStatus["chain"];
		statusNode["sync_status_rust"] = rustStatus["sync_status"];
	}

	return HTTPUtil::BuildSuccessResponse(conn, statusNode.toStyledString());
}

std::string ServerAPI::GetStatusString(const SyncStatus& syncStatus)
{
	const ESyncStatus status = syncStatus.GetStatus();
	switch (status)
	{
		case ESyncStatus::SYNCING_HEADERS:
		{
			return "SYNCING_HEADERS";
		}
		case ESyncStatus::SYNCING_TXHASHSET:
		case ESyncStatus::TXHASHSET_SYNC_FAILED:
		{
			return "DOWNLOADING_TXHASHSET";
		}
		case ESyncStatus::PROCESSING_TXHASHSET:
		{
			return "PROCESSING_TXHASHSET";
		}
		case ESyncStatus::SYNCING_BLOCKS:
		{
			return "SYNCING_BLOCKS";
		}
		case ESyncStatus::WAITING_FOR_PEERS:
		{
			return "NOT_CONNECTED";
		}
		case ESyncStatus::NOT_SYNCING:
		{
			return "FULLY_SYNCED";
		}
	}

	return "UNKNOWN";
}

int ServerAPI::ResyncChain_Handler(struct mg_connection* conn, void* pNodeContext)
{
	if (!ApiAccess::Check(conn, ApiAccess::Level::ADMIN, Global::GetConfig().GetRestAPIPort()))
	{
		return HTTPUtil::BuildUnauthorizedResponse(conn, "Unauthorized");
	}

	NodeContext* pServer = (NodeContext*)pNodeContext;

	pServer->m_pBlockChain->ResyncChain();
	pServer->m_pP2PServer->UnbanAllPeers();

	return HTTPUtil::BuildSuccessResponse(conn, "");
}