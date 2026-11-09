#include "PeersAPI.h"
#include <Net/PeerAPIPath.h>
#include "../NodeContext.h"

#include <Core/Global.h>
#include <Net/Util/HTTPUtil.h>
#include <Net/Servers/RPC/ApiAccess.h>
#include <Common/Util/StringUtil.h>
#include <json/json.h>
#include <ctime>
#include <utility>
#include <vector>

int PeersAPI::GetDiagnostics_Handler(struct mg_connection* conn, void* pNodeContext)
{
	NodeContext* pServer = (NodeContext*)pNodeContext;
	try
	{
		const std::vector<PeerConstPtr> peers = pServer->m_pP2PServer->GetAllPeers();
		const std::vector<ConnectedPeer> connected = pServer->m_pP2PServer->GetConnectedPeers();
		SyncStatusConstPtr pSyncStatus = pServer->m_pP2PServer->GetSyncStatus();

		const std::string downloadPeer = pSyncStatus != nullptr ? pSyncStatus->GetPibdPeer() : "";
		Json::Value root;

		Json::Value banned(Json::arrayValue);
		Json::Value connectedJson(Json::arrayValue);
		Json::Value versions(Json::objectValue);
		size_t bannedCount = 0;

		for (const PeerConstPtr& peer : peers) {
			const std::string agent = peer->GetUserAgent().empty() ? "unknown" : peer->GetUserAgent();
			versions[agent] = versions.get(agent, 0).asUInt64() + 1;

			if (!peer->IsBanned()) {
				continue;
			}

			Json::Value item = peer->ToJSON();
			item["banned_at"] = Json::UInt64(peer->GetLastBanTime());
			banned.append(item);
			++bannedCount;
		}

		for (const ConnectedPeer& peer : connected) {
			Json::Value item = peer.ToJSON();
			const std::string address = peer.GetIPAddress().Format();
			const Capabilities capabilities = peer.GetPeer()->GetCapabilities();
			item["pibd_capable"] = capabilities.HasCapability(Capabilities::PIBD_HIST) ||
				capabilities.HasCapability(Capabilities::PIBD_HIST_1);
			item["is_download_source"] = !downloadPeer.empty() && address == downloadPeer;
			connectedJson.append(item);
		}

		Json::Value totals;
		const std::pair<size_t, size_t> numConnections = pServer->m_pP2PServer->GetNumberOfConnectedPeers();
		totals["known"] = Json::UInt64(peers.size());
		totals["connected"] = Json::UInt64(connected.size());
		totals["inbound"] = Json::UInt64(numConnections.first);
		totals["outbound"] = Json::UInt64(numConnections.second);
		totals["banned"] = Json::UInt64(bannedCount);

		Json::Value download;
		download["source"] = downloadPeer;
		download["received_segments"] = Json::UInt64(pSyncStatus != nullptr ? pSyncStatus->GetPibdReceived() : 0);
		download["required_segments"] = Json::UInt64(pSyncStatus != nullptr ? pSyncStatus->GetPibdRequired() : 0);
		const uint64_t received = download["received_segments"].asUInt64();
		const uint64_t required = download["required_segments"].asUInt64();
		const time_t started = pSyncStatus != nullptr ? pSyncStatus->GetPibdStarted() : 0;
		const double elapsed = started > 0 ? (double)(std::time(nullptr) - started) : 0.0;
		download["elapsed_sec"] = Json::UInt64(started > 0 ? (uint64_t)elapsed : 0);
		download["percent"] = required > 0 ? (100.0 * (double)received / (double)required) : 0.0;
		download["segments_per_sec"] = elapsed > 0.0 ? (double)received / elapsed : 0.0;
		download["segments_per_min"] = elapsed > 0.0 ? 60.0 * (double)received / elapsed : 0.0;

		std::string status = "UNKNOWN";
		if (pSyncStatus != nullptr) {
			switch (pSyncStatus->GetStatus()) {
				case ESyncStatus::SYNCING_HEADERS: status = "SYNCING_HEADERS"; break;
				case ESyncStatus::SYNCING_TXHASHSET:
				case ESyncStatus::TXHASHSET_SYNC_FAILED: status = "DOWNLOADING_TXHASHSET"; break;
				case ESyncStatus::PROCESSING_TXHASHSET: status = "PROCESSING_TXHASHSET"; break;
				case ESyncStatus::SYNCING_BLOCKS: status = "SYNCING_BLOCKS"; break;
				case ESyncStatus::WAITING_FOR_PEERS: status = "NOT_CONNECTED"; break;
				case ESyncStatus::NOT_SYNCING: status = "FULLY_SYNCED"; break;
			}
		}
		download["status"] = status;
		download["header_height"] = Json::UInt64(pSyncStatus != nullptr ? pSyncStatus->GetHeaderHeight() : 0);
		download["chain_height"] = Json::UInt64(pSyncStatus != nullptr ? pSyncStatus->GetBlockHeight() : 0);
		download["network_height"] = Json::UInt64(pSyncStatus != nullptr ? pSyncStatus->GetNetworkHeight() : 0);

		root["totals"] = totals;
		root["banned"] = banned;
		root["connected"] = connectedJson;
		root["download"] = download;
		root["versions"] = versions;
		return HTTPUtil::BuildSuccessResponse(conn, root.toStyledString());
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Exception thrown: {}", e.what());
		return HTTPUtil::BuildInternalErrorResponse(conn, "Unknown error occurred");
	}
}
//
// Handles requests to retrieve all known peers.
//
// APIs:
// GET /v1/peers/all
//
int PeersAPI::GetAllPeers_Handler(struct mg_connection* conn, void* pNodeContext)
{
	NodeContext* pServer = (NodeContext*)pNodeContext;

	try
	{
		const std::vector<PeerConstPtr> peers = pServer->m_pP2PServer->GetAllPeers();

		Json::Value json(Json::arrayValue);
		for (PeerConstPtr peer : peers)
		{
			json.append(peer->ToJSON());
		}

		return HTTPUtil::BuildSuccessResponse(conn, json.toStyledString());
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Exception thrown: {}", e.what());
		return HTTPUtil::BuildInternalErrorResponse(conn, "Unknown error occurred");
	}
}

//
// Handles requests to retrieve all connected peers.
//
// APIs:
// GET /v1/peers/connected
//
int PeersAPI::GetConnectedPeers_Handler(struct mg_connection* conn, void* pNodeContext)
{
	NodeContext* pServer = (NodeContext*)pNodeContext;

	try
	{
		const std::vector<ConnectedPeer> connectedPeers = pServer->m_pP2PServer->GetConnectedPeers();

		Json::Value json(Json::arrayValue);
		for (const ConnectedPeer& connectedPeer : connectedPeers)
		{
			json.append(connectedPeer.ToJSON());
		}

		return HTTPUtil::BuildSuccessResponse(conn, json.toStyledString());
	}
	catch (std::exception & e)
	{
		LOG_ERROR_F("Exception thrown: {}", e.what());
		return HTTPUtil::BuildInternalErrorResponse(conn, "Unknown error occurred");
	}
}

//
// Handles requests to retrieve, ban, or unban a peer.
//
// APIs:
// GET /v1/peers/a.b.c.d
// POST /v1/peers/a.b.c.d/ban
// POST /v1/peers/a.b.c.d/unban
//
int PeersAPI::Peer_Handler(struct mg_connection* conn, void* pNodeContext)
{
	NodeContext* pServer = (NodeContext*)pNodeContext;
	const std::string requestedPeer = HTTPUtil::GetURIParam(conn, "/v1/peers/");
	if (requestedPeer.empty())
	{
		return GetAllPeers_Handler(conn, pNodeContext);
	}

	try
	{
		const HTTP::EHTTPMethod method = HTTPUtil::GetHTTPMethod(conn);

		const std::string ipAddressStr = ParseIPAddress(requestedPeer);
		IPAddress ipAddress = IPAddress::Parse(ipAddressStr);

		const std::string commandStr = ParseCommand(requestedPeer);
		if (commandStr == "ban" && method == HTTP::EHTTPMethod::POST)
		{
			if (!ApiAccess::Check(conn, ApiAccess::Level::ADMIN, Global::GetConfig().GetRestAPIPort()))
			{
				return HTTPUtil::BuildUnauthorizedResponse(conn, "Unauthorized");
			}

			pServer->m_pP2PServer->BanPeer(ipAddress, EBanReason::ManualBan);
			return HTTPUtil::BuildSuccessResponse(conn, "");
		}
		else if (commandStr == "unban" && method == HTTP::EHTTPMethod::POST)
		{
			if (!ApiAccess::Check(conn, ApiAccess::Level::ADMIN, Global::GetConfig().GetRestAPIPort()))
			{
				return HTTPUtil::BuildUnauthorizedResponse(conn, "Unauthorized");
			}

			pServer->m_pP2PServer->UnbanPeer(ipAddress);

			return HTTPUtil::BuildSuccessResponse(conn, "");
		}
		else if (commandStr == "" && method == HTTP::EHTTPMethod::GET)
		{
			std::optional<PeerConstPtr> peerOpt = pServer->m_pP2PServer->GetPeer(ipAddress);
			if (peerOpt.has_value())
			{
				return HTTPUtil::BuildSuccessResponse(
					conn,
					peerOpt.value()->ToJSON().toStyledString()
				);
			}
		}
		else
		{
			return HTTPUtil::BuildBadRequestResponse(conn, "Invalid command.");
		}
	}
	catch (const DeserializationException&)
	{
		return HTTPUtil::BuildBadRequestResponse(conn, "Invalid IP address.");
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Exception thrown: {}", e.what());
		return HTTPUtil::BuildInternalErrorResponse(conn, "Peer operation failed.");
	}

	return HTTPUtil::BuildNotFoundResponse(conn, "Peer not found.");
}

std::string PeersAPI::ParseIPAddress(const std::string& request)
{
	return PeerAPIPath::Address(request);
}

std::string PeersAPI::ParseCommand(const std::string& request)
{
	const std::size_t slashPos = request.find("/");
	if (slashPos != std::string::npos)
	{
		return request.substr(slashPos + 1);
	}

	return "";
}
