#pragma once

#include <Net/PeerAPIPath.h>
#include <Net/IPAddress.h>

#include <P2P/P2PServer.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <json/json.h>
#include <optional>
#include <string>
#include <vector>

// Grin v5.5.2 owner API: get_peers.
// Optional single param: a peer address to filter by (ip or ip:port; the port is
// ignored because Grin++ identifies peers by IP only).
// Each entry follows api::types::PeerData: addr, ban_reason, capabilities{bits},
// flags, last_banned, last_connected, user_agent.
class GetPeersOwnerHandler : public RPCMethod
{
public:
	GetPeersOwnerHandler(const IP2PServerPtr& pP2PServer)
		: m_pP2PServer(pP2PServer) { }
	~GetPeersOwnerHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		std::string addressFilter;
		if (request.GetParams().has_value()) {
			const Json::Value& params = request.GetParams().value();
			if (!params.isArray()) {
				return request.BuildError("INVALID_PARAMS", "Expected an address string or null.");
			}
			if (!params.empty() && !params[0].isNull()) {
				if (!params[0].isString()) {
					return request.BuildError("INVALID_PARAMS", "Expected an address string or null.");
				}
				addressFilter = params[0].asString();
			}
		}

		Json::Value entries(Json::arrayValue);
		for (const PeerConstPtr& pPeer : m_pP2PServer->GetAllPeers()) {
			if (pPeer == nullptr) {
				continue;
			}

			Json::Value peerJSON = pPeer->ToJSON();
			if (!addressFilter.empty() && !MatchesAddress(addressFilter, peerJSON["addr"].asString())) {
				continue;
			}

			entries.append(peerJSON);
		}

		Json::Value result;
		result["Ok"] = entries;
		return request.BuildResult(result);
	}

	ApiAccess::Level GetAccessLevel() const noexcept final { return ApiAccess::Level::PRIVATE; }

	bool ContainsSecrets() const noexcept final { return false; }

private:
	// Compare parsed addresses, so IPv6 forms and optional ports are handled
	// consistently with the REST peer endpoint.
	static bool MatchesAddress(const std::string& requested, const std::string& actual)
	{
		try {
			return IPAddress::Parse(PeerAPIPath::Address(requested)) == IPAddress::Parse(actual);
		} catch (const std::exception&) {
			return false;
		}
	}

	IP2PServerPtr m_pP2PServer;
};
