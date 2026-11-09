#include <catch.hpp>

#include <asio.hpp>

#include <API/Node/Handlers/GetBlocksHandler.h>
#include <API/Node/Handlers/GetOutputsHandler.h>
#include <API/Node/Handlers/GetPeersOwnerHandler.h>

#include <Core/Util/JsonUtil.h>
#include <Net/Servers/RPC/ApiAccess.h>
#include <Net/Servers/RPC/RPCServer.h>
#include <Net/Servers/Server.h>
#include <P2P/P2PServer.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
	// Minimal IP2PServer good enough for parameter-validation tests. Invalid
	// parameters must be rejected before any peer data is touched.
	class EmptyPeers final : public IP2PServer
	{
	public:
		SyncStatusConstPtr GetSyncStatus() const final { return nullptr; }
		std::pair<size_t, size_t> GetNumberOfConnectedPeers() const final { return { 0, 0 }; }
		std::vector<PeerConstPtr> GetAllPeers() const final { return {}; }
		std::vector<ConnectedPeer> GetConnectedPeers() const final { return {}; }
		std::optional<PeerConstPtr> GetPeer(const IPAddress&) const final { return std::nullopt; }
		void BanPeer(const IPAddress&, const EBanReason) final { }
		void UnbanPeer(const IPAddress&) final { }
		bool UnbanAllPeers() final { return true; }
		void BroadcastTransaction(const TransactionPtr&) final { }
	};

	Json::Value MakeParams(const std::string& json)
	{
		Json::Value params;
		REQUIRE(JsonUtil::Parse(json, params));
		return params;
	}

	std::string PostRpc(
		const uint16_t port,
		const std::string& path,
		const std::string& body)
	{
		asio::io_context io;
		asio::ip::tcp::socket socket(io);
		socket.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

		std::string request =
			"POST " + path + " HTTP/1.1\r\n" +
			"Host: 127.0.0.1:" + std::to_string(port) + "\r\n" +
			"Content-Type: application/json-rpc\r\n" +
			"Content-Length: " + std::to_string(body.size()) + "\r\n" +
			"Connection: close\r\n\r\n" + body;

		asio::write(socket, asio::buffer(request));

		asio::streambuf buffer;
		asio::error_code error;
		asio::read(socket, buffer, error);
		if (error != asio::error::eof) {
			REQUIRE_FALSE(error);
		}

		const std::string raw(
			asio::buffers_begin(buffer.data()),
			asio::buffers_end(buffer.data()));
		const size_t headerEnd = raw.find("\r\n\r\n");
		REQUIRE(headerEnd != std::string::npos);
		return raw.substr(headerEnd + 4);
	}

	Json::Value RpcJson(const std::string& response)
	{
		Json::Value json;
		REQUIRE(JsonUtil::Parse(response, json));
		return json;
	}
}

TEST_CASE("Node get_peers rejects a non-string filter instead of silently ignoring it")
{
	const GetPeersOwnerHandler handler(std::make_shared<EmptyPeers>());

	const auto numeric = handler.Handle(
		RPC::Request::BuildRequest("get_peers", MakeParams("[123]"))).ToJSON();
	REQUIRE(numeric.isMember("error"));
	REQUIRE(numeric["error"]["data"]["type"].asString() == "INVALID_PARAMS");

	const auto object = handler.Handle(
		RPC::Request::BuildRequest("get_peers", MakeParams("[{}]"))).ToJSON();
	REQUIRE(object.isMember("error"));
	REQUIRE(object["error"]["data"]["type"].asString() == "INVALID_PARAMS");

	const auto objectParams = handler.Handle(
		RPC::Request::BuildRequest("get_peers", MakeParams("{}"))).ToJSON();
	REQUIRE(objectParams.isMember("error"));
	REQUIRE(objectParams["error"]["data"]["type"].asString() == "INVALID_PARAMS");

	// Null and absent filter remain valid and return the (empty) peer list.
	const auto nullFilter = handler.Handle(
		RPC::Request::BuildRequest("get_peers", MakeParams("[null]"))).ToJSON();
	REQUIRE(nullFilter["result"]["Ok"].isArray());
	const auto noParams = handler.Handle(
		RPC::Request::BuildRequest("get_peers")).ToJSON();
	REQUIRE(noParams["result"]["Ok"].isArray());
}

TEST_CASE("Node get_outputs accepts only null or an array for the commitments slot")
{
	REQUIRE(GetOutputsHandler::HasValidCommitmentsParam(MakeParams("[]")));
	REQUIRE(GetOutputsHandler::HasValidCommitmentsParam(MakeParams("[null]")));
	REQUIRE(GetOutputsHandler::HasValidCommitmentsParam(MakeParams("[[\"08aa\"]]")));
	REQUIRE_FALSE(GetOutputsHandler::HasValidCommitmentsParam(MakeParams("[123]")));
	REQUIRE_FALSE(GetOutputsHandler::HasValidCommitmentsParam(MakeParams("[\"08aa\"]")));
	REQUIRE_FALSE(GetOutputsHandler::HasValidCommitmentsParam(MakeParams("[{}]")));
}

TEST_CASE("RPC server maps JsonCpp parameter type errors to INVALID_PARAMS")
{
	const ServerPtr pServer = Server::Create(EServerType::LOCAL, std::nullopt);
	const auto port = pServer->GetPortNumber();
	const RPCServer::Ptr pRpc =
		RPCServer::Create(pServer, "/v2/foreign", LoggerAPI::LogFile::NODE);
	pRpc->AddMethod(
		"get_blocks",
		std::make_shared<GetBlocksHandler>(IBlockChain::Ptr(), TxHashSetManager::Ptr(), IDatabasePtr()));

	// include_proof must be a boolean; JsonCpp asBool() throws Json::LogicError.
	const std::string wrongType =
		R"({"jsonrpc":"2.0","method":"get_blocks","params":[0,null,null,"oops"],"id":11})";
	const Json::Value wrongTypeReply = RpcJson(PostRpc(port, "/v2/foreign", wrongType));
	REQUIRE(wrongTypeReply["error"]["code"].asInt() == RPC::ErrorCode::INVALID_PARAMS);

	// Missing params is the documented PARAMS_MISSING path, not a chain access.
	const std::string missing =
		R"({"jsonrpc":"2.0","method":"get_blocks","id":12})";
	const Json::Value missingReply = RpcJson(PostRpc(port, "/v2/foreign", missing));
	REQUIRE(missingReply["error"]["code"].asInt() == -150);

	// Unknown methods keep their standard JSON-RPC code.
	const std::string unknown =
		R"({"jsonrpc":"2.0","method":"no_such_method","params":[],"id":13})";
	const Json::Value unknownReply = RpcJson(PostRpc(port, "/v2/foreign", unknown));
	REQUIRE(unknownReply["error"]["code"].asInt() == RPC::ErrorCode::METHOD_NOT_FOUND);
}
