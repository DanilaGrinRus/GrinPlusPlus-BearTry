#include <catch.hpp>

#include <asio.hpp>

#include <Common/Logger.h>
#include <Net/Servers/Server.h>
#include <Net/Servers/RPC/ApiAccess.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <Net/Servers/RPC/RPCServer.h>

#include <filesystem.h>
#include <fstream>
#include <string>

namespace
{
	class PrivateProbeMethod final : public RPCMethod
	{
	public:
		RPC::Response Handle(const RPC::Request& request) const final
		{
			Json::Value result;
			result["Ok"] = "private_ok";
			return request.BuildResult(result);
		}

		ApiAccess::Level GetAccessLevel() const noexcept final { return ApiAccess::Level::PRIVATE; }
		bool ContainsSecrets() const noexcept final { return false; }
	};

	class PublicProbeMethod final : public RPCMethod
	{
	public:
		RPC::Response Handle(const RPC::Request& request) const final
		{
			Json::Value result;
			result["Ok"] = "public_ok";
			return request.BuildResult(result);
		}

		bool ContainsSecrets() const noexcept final { return false; }
	};

	std::string ReadSecret(const fs::path& path)
	{
		std::ifstream in(path);
		REQUIRE(in.good());
		std::string token;
		std::getline(in, token);
		while (!token.empty() && (token.back() == '\r' || token.back() == '\n')) {
			token.pop_back();
		}
		return token;
	}

	std::string Post(
		const uint16_t port,
		const std::string& body,
		const std::string& token)
	{
		asio::io_context io;
		asio::ip::tcp::socket socket(io);
		socket.connect(asio::ip::tcp::endpoint(
			asio::ip::make_address("127.0.0.1"), port));

		std::string request =
			"POST /v2/owner HTTP/1.1\r\n" +
			std::string("Host: 127.0.0.1:") + std::to_string(port) + "\r\n" +
			"Content-Type: application/json-rpc\r\n" +
			"Content-Length: " + std::to_string(body.size()) + "\r\n";
		if (!token.empty()) {
			request += "Authorization: Bearer " + token + "\r\n";
		}
		request += "\r\n" + body;

		asio::write(socket, asio::buffer(request));

		asio::streambuf buffer;
		asio::read_until(socket, buffer, "\r\n\r\n");

		std::istream stream(&buffer);
		std::string line;
		std::getline(stream, line); // status line
		size_t contentLength = 0;
		while (std::getline(stream, line) && line != "\r") {
			const std::string prefix = "Content-Length:";
			if (line.size() >= prefix.size() &&
				line.compare(0, prefix.size(), prefix) == 0) {
				contentLength = static_cast<size_t>(std::stoul(line.substr(prefix.size())));
			}
		}

		std::string response;
		response.append(asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data()));
		if (response.size() < contentLength) {
			const size_t remaining = contentLength - response.size();
			std::string tail(remaining, '\0');
			asio::read(socket, asio::buffer(tail));
			response += tail;
		}
		return response;
	}
}

TEST_CASE("Owner RPC methods reject missing/wrong token and accept valid token")
{
	const fs::path secretDir = fs::temp_directory_path() / "rpc-owner-auth-test";
	std::error_code error;
	fs::remove_all(secretDir, error);
	fs::create_directories(secretDir, error);
	REQUIRE_FALSE(error);

	ApiAccess::Initialize(secretDir);
	REQUIRE(ApiAccess::IsInitialized());
	const std::string token = ReadSecret(ApiAccess::GetSecretPath());
	REQUIRE(token.size() == 64);

	uint16_t port = 0;
	{
		const ServerPtr pServer = Server::Create(EServerType::LOCAL, std::nullopt);
		port = pServer->GetPortNumber();
		const RPCServer::Ptr pRpc =
			RPCServer::Create(pServer, "/v2/owner", LoggerAPI::LogFile::NODE);
		pRpc->AddMethod("private_probe", std::make_shared<PrivateProbeMethod>());
		pRpc->AddMethod("public_probe", std::make_shared<PublicProbeMethod>());

		const std::string privateBody =
			R"({"jsonrpc":"2.0","method":"private_probe","params":[],"id":1})";
		const std::string publicBody =
			R"({"jsonrpc":"2.0","method":"public_probe","params":[],"id":2})";

		REQUIRE(Post(port, privateBody, "").find("-32001") != std::string::npos);
		REQUIRE(Post(port, privateBody, "0000000000000000000000000000000000000000000000000000000000000000")
			.find("-32001") != std::string::npos);
		REQUIRE(Post(port, privateBody, token).find("private_ok") != std::string::npos);
		REQUIRE(Post(port, publicBody, "").find("public_ok") != std::string::npos);
	}

	fs::remove_all(secretDir, error);
}
