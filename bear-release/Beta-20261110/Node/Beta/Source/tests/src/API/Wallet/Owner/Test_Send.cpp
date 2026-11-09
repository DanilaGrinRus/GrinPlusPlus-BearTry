#include <catch.hpp>

#include <Net/Clients/RPC/RPCClient.h>
#include <API/Wallet/Owner/Models/SendResponse.h>

#include <TestServer.h>
#include <TestChain.h>
#include <Comparators/JsonComparator.h>
#include <optional>

TEST_CASE("API: send - Slatepack")
{
    TestServer::Ptr pTestServer = TestServer::CreateWithWallet();

    auto pSenderWallet = pTestServer->CreateUser("Alice", "P@ssw0rd123!", UseTor::YES).wallet;
    auto pReceiverWallet = pTestServer->CreateUser("Bob", "P@ssw0rd123!", UseTor::NO).wallet;

    TestChain chain(pTestServer->GetBlockChain());
    chain.MineChain(pSenderWallet, 30);

    // Send
    SendCriteria criteria(
        pSenderWallet->GetToken(),
        1'000'000'000,
        1'000'000,
        (uint8_t)1,
        SelectionStrategyDTO(ESelectionStrategy::SMALLEST, {}),
        std::make_optional<std::string>(pReceiverWallet->GetSlatepackAddress().ToString()),
        std::nullopt
    );

    // W1/F-03 contract: an unreachable Tor recipient is a real error. The
    // handler must NOT return a legacy SENT success with an error field (UI
    // v1.2.8 ignores that field and would show a false "sent").
    const RPC::Response rpcResponse = pTestServer->InvokeOwnerRPC("send", criteria.ToJSON());
    REQUIRE(!rpcResponse.GetResult().has_value());
    REQUIRE(rpcResponse.GetError().has_value());
}
