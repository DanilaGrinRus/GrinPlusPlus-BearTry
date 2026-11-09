#pragma once

#include <P2P/P2PServer.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <json/json.h>

// Grin v5.5.2 owner API: get_connected_peers.
// Each entry follows api::types::PeerData with the connected-only fields:
// addr, capabilities{bits}, direction, height, total_difficulty, user_agent,
// version.
class GetConnectedPeersOwnerHandler : public RPCMethod
{
public:
	GetConnectedPeersOwnerHandler(const IP2PServerPtr& pP2PServer)
		: m_pP2PServer(pP2PServer) { }
	~GetConnectedPeersOwnerHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		Json::Value entries(Json::arrayValue);
		for (const ConnectedPeer& connectedPeer : m_pP2PServer->GetConnectedPeers()) {
			Json::Value peerJSON = connectedPeer.ToJSON();
			// grin exposes the negotiated protocol version as "version".
			peerJSON["version"] = peerJSON["protocol_version"];
			entries.append(peerJSON);
		}

		Json::Value result;
		result["Ok"] = entries;
		return request.BuildResult(result);
	}

	ApiAccess::Level GetAccessLevel() const noexcept final { return ApiAccess::Level::PRIVATE; }

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IP2PServerPtr m_pP2PServer;
};
