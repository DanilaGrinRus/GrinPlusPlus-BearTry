#include <catch.hpp>

#include <Core/Exceptions/DeserializationException.h>
#include <Net/Servers/RPC/RPCServer.h>

#include <string>

//
// Regression (W-07): a request body that fails JSON parsing used to be embedded
// verbatim into the exception message, which RPCServer then wrote to Wallet.log
// and returned in the RPC error. Bodies contain session tokens and armored
// slatepacks, so the message must only describe the failure.
//
TEST_CASE("Malformed JSON body is not echoed into the error (log/reply) message")
{
	const std::string body =
		"{\"method\":\"send\",\"params\":{\"session_token\":\"c2VjcmV0LXRva2Vu\","
		"\"slatepack\":\"BEGINSLATEPACK. SECRETPAYLOAD. ENDSLATEPACK.\"}";

	const std::string description = RPC::FormatInvalidJsonBodyError(body);

	// Only the size is reported.
	REQUIRE(description.find(std::to_string(body.size())) != std::string::npos);

	// No part of the request may leak through the message.
	REQUIRE(description.find("session_token") == std::string::npos);
	REQUIRE(description.find("c2VjcmV0LXRva2Vu") == std::string::npos);
	REQUIRE(description.find("BEGINSLATEPACK") == std::string::npos);
	REQUIRE(description.find("SECRETPAYLOAD") == std::string::npos);
	REQUIRE(description.find("send") == std::string::npos);

	// The failure path still produces an exception (RPCServer turns it into a
	// JSON-RPC error; that behaviour is unchanged).
	std::string logged;
	try
	{
		throw DESERIALIZATION_EXCEPTION(RPC::FormatInvalidJsonBodyError(body));
	}
	catch (const std::exception& e)
	{
		logged = e.what();
	}

	REQUIRE(!logged.empty());
	REQUIRE(logged.find("session_token") == std::string::npos);
	REQUIRE(logged.find("BEGINSLATEPACK") == std::string::npos);
	REQUIRE(logged.find(std::to_string(body.size())) != std::string::npos);
}
