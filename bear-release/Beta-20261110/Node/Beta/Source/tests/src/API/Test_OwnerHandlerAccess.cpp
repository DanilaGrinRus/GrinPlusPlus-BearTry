#include <catch.hpp>

#include <API/Node/Handlers/GetConnectedPeersOwnerHandler.h>
#include <API/Node/Handlers/GetPeersOwnerHandler.h>
#include <API/Node/Handlers/PeerControlOwnerHandler.h>
#include <API/Node/Handlers/GetStatusOwnerHandler.h>
#include <API/Node/Handlers/GetConfigHandler.h>
#include <API/Node/Handlers/UpdateConfigHandler.h>

#include <Net/Servers/RPC/ApiAccess.h>
#include <Net/Servers/RPC/RPCMethod.h>

namespace
{
	class KnownPeers final : public IP2PServer
	{
	public:
		KnownPeers()
		{
			auto peer = std::make_shared<Peer>(IPAddress::Parse("2001:db8::42"));
			peer->UpdateLastContactTime();
			m_peers.push_back(peer);
		}
		SyncStatusConstPtr GetSyncStatus() const final { return nullptr; }
		std::pair<size_t, size_t> GetNumberOfConnectedPeers() const final { return {0, 0}; }
		std::vector<PeerConstPtr> GetAllPeers() const final { return m_peers; }
		std::vector<ConnectedPeer> GetConnectedPeers() const final
		{
			auto peer = std::make_shared<Peer>(IPAddress::Parse("2001:db8::42"));
			peer->UpdateVersion(7);
			return { ConnectedPeer(peer, EDirection::OUTBOUND, 13414) };
		}
		std::optional<PeerConstPtr> GetPeer(const IPAddress&) const final { return std::nullopt; }
		void BanPeer(const IPAddress& address, const EBanReason) final { lastAddress = address.Format(); banned = true; }
		void UnbanPeer(const IPAddress& address) final { lastAddress = address.Format(); banned = false; }
		bool UnbanAllPeers() final { return true; }
		void BroadcastTransaction(const TransactionPtr&) final { }
		std::string lastAddress;
		bool banned = false;
	private:
		std::vector<PeerConstPtr> m_peers;
	};

	class PublicProbeMethod final : public RPCMethod
	{
	public:
		RPC::Response Handle(const RPC::Request& request) const final
		{
			Json::Value result;
			result["Ok"] = "public_probe";
			return request.BuildResult(result);
		}
		bool ContainsSecrets() const noexcept final { return false; }
	};
}

TEST_CASE("Owner get_peers matches equivalent IPv6 addresses")
{
	const GetPeersOwnerHandler handler(std::make_shared<KnownPeers>());
	Json::Value params(Json::arrayValue);
	params.append("2001:0db8::42");
	const Json::Value response = handler.Handle(RPC::Request::BuildRequest("get_peers", params)).ToJSON();
	REQUIRE(response["result"]["Ok"].isArray());
	REQUIRE(response["result"]["Ok"].size() == 1);
	REQUIRE(response["result"]["Ok"][0]["addr"].asString() == "2001:db8::42");
	params[0] = "[2001:db8::42]:13414";
	REQUIRE(handler.Handle(RPC::Request::BuildRequest("get_peers", params)).ToJSON()["result"]["Ok"].size() == 1);
	params[0] = "2001:db8::43";
	REQUIRE(handler.Handle(RPC::Request::BuildRequest("get_peers", params)).ToJSON()["result"]["Ok"].empty());
}

TEST_CASE("Owner peer responses distinguish stored and connected peers")
{
	const auto server = std::make_shared<KnownPeers>();
	const GetPeersOwnerHandler storedHandler(server);
	const GetConnectedPeersOwnerHandler connectedHandler(server);
	const Json::Value params(Json::arrayValue);
	const Json::Value stored = storedHandler.Handle(RPC::Request::BuildRequest("get_peers", params)).ToJSON()["result"]["Ok"];
	const Json::Value connected = connectedHandler.Handle(RPC::Request::BuildRequest("get_connected_peers", params)).ToJSON()["result"]["Ok"];
	REQUIRE(stored.size() == 1);
	REQUIRE(stored[0].isMember("flags"));
	REQUIRE_FALSE(stored[0].isMember("protocol_version"));
	REQUIRE(connected.size() == 1);
	REQUIRE(connected[0]["version"].asUInt() == 7);
	REQUIRE(connected[0]["direction"].asString() == "Outbound");
	REQUIRE_FALSE(connected[0].isMember("protocol_version"));
	REQUIRE_FALSE(connected[0].isMember("flags"));
	REQUIRE_FALSE(connected[0].isMember("ban_reason"));
	REQUIRE_FALSE(connected[0].isMember("last_connected"));
	REQUIRE_FALSE(connected[0].isMember("last_banned"));
}
TEST_CASE("Owner peer control requires admin and parses addresses")
{
	const auto server = std::make_shared<KnownPeers>();
	const PeerControlOwnerHandler ban(server, true);
	const PeerControlOwnerHandler unban(server, false);
	REQUIRE(ban.GetAccessLevel() == ApiAccess::Level::ADMIN);
	REQUIRE(unban.GetAccessLevel() == ApiAccess::Level::ADMIN);
	Json::Value params(Json::arrayValue);
	params.append("[2001:db8::42]:13414");
	const Json::Value result = ban.Handle(RPC::Request::BuildRequest("ban_peer", params)).ToJSON();
	REQUIRE(result["result"].isMember("Ok"));
	REQUIRE(server->banned);
	REQUIRE(server->lastAddress == "2001:db8::42");
	params[0] = "2001:db8::42";
	unban.Handle(RPC::Request::BuildRequest("unban_peer", params));
	REQUIRE_FALSE(server->banned);
	params[0] = "[::1]:0";
	REQUIRE(ban.Handle(RPC::Request::BuildRequest("ban_peer", params)).ToJSON().isMember("error"));
	REQUIRE_FALSE(server->banned);
}

// Security regression: owner read methods used to inherit the RPCMethod PUBLIC
// default and returned data without Authorization. The central RPCServer gate
// rejects PRIVATE/ADMIN methods with RPC::ErrorCode::UNAUTHORIZED.
TEST_CASE("Owner handlers require PRIVATE access; config levels unchanged")
{
	const GetStatusOwnerHandler status{ IBlockChain::Ptr(), IP2PServerPtr() };
	const GetPeersOwnerHandler peers{ IP2PServerPtr() };
	const GetConnectedPeersOwnerHandler connected{ IP2PServerPtr() };

	REQUIRE(status.GetAccessLevel() == ApiAccess::Level::PRIVATE);
	REQUIRE(peers.GetAccessLevel() == ApiAccess::Level::PRIVATE);
	REQUIRE(connected.GetAccessLevel() == ApiAccess::Level::PRIVATE);

	const GetConfigHandler config;
	const UpdateConfigHandler updateConfig;
	REQUIRE(config.GetAccessLevel() == ApiAccess::Level::PRIVATE);
	REQUIRE(updateConfig.GetAccessLevel() == ApiAccess::Level::ADMIN);

	const PublicProbeMethod publicProbe;
	REQUIRE(publicProbe.GetAccessLevel() == ApiAccess::Level::PUBLIC);
}
