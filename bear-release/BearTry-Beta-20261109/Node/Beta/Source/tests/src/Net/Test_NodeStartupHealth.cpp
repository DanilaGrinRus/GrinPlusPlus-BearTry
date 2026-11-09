#include <catch.hpp>
#include <asio.hpp>
#include <Net/Servers/Server.h>
#include <Net/Servers/NodeStartupHealth.h>
#include <string>

namespace
{
    std::string HealthRequest(const uint16_t port, const std::string& method)
    {
        asio::io_context io;
        asio::ip::tcp::socket socket(io);
        socket.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        const std::string request = method + " /v1/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        asio::write(socket, asio::buffer(request));
        asio::streambuf response;
        asio::error_code error;
        asio::read(socket, response, error);
        if (error != asio::error::eof) {
            REQUIRE_FALSE(error);
        }
        return std::string(asio::buffers_begin(response.data()), asio::buffers_end(response.data()));
    }
}

TEST_CASE("Node health responds while starting and after readiness")
{
    const auto pHealth = std::make_shared<NodeStartupHealth>();
    const ServerPtr pServer = Server::Create(EServerType::LOCAL, std::nullopt);
    pServer->AddListener("/v1/health", NodeStartupHealth::Handle, pHealth.get());
    const auto starting = HealthRequest(pServer->GetPortNumber(), "GET");
    REQUIRE(starting.find("200 OK") != std::string::npos);
    REQUIRE(starting.find("\"starting\"") != std::string::npos);
    pHealth->ready.store(true);
    const auto ready = HealthRequest(pServer->GetPortNumber(), "GET");
    REQUIRE(ready.find("200 OK") != std::string::npos);
    REQUIRE(ready.find("\"ready\"") != std::string::npos);
    REQUIRE(HealthRequest(pServer->GetPortNumber(), "POST").find("404") != std::string::npos);
}
