#include <catch.hpp>

#include <Server/Node/NodeClients/RPCNodeClient.h>
#include <Net/Clients/RPC/RPCException.h>
#include <Core/Util/JsonUtil.h>
#include <Crypto/Models/Commitment.h>

#include <optional>
#include <string>
#include <vector>

namespace
{
	Json::Value MakeOkArrayResult()
	{
		Json::Value ok(Json::arrayValue);
		Json::Value result;
		result["Ok"] = ok;
		return result;
	}

	std::vector<Commitment> MakeCommitments(const size_t count)
	{
		static const char* HEX = "0123456789abcdef";
		std::vector<Commitment> commitments;
		commitments.reserve(count);
		for (size_t i = 0; i < count; ++i)
		{
			std::string hex(66, '0');
			hex[0] = '0';
			hex[1] = '8';
			for (int nibble = 0; nibble < 8; ++nibble) {
				hex[65 - nibble] = HEX[(i >> (4 * nibble)) & 0xF];
			}

			commitments.push_back(Commitment::FromHex(hex));
		}

		return commitments;
	}
}

TEST_CASE("RPCNodeClient get_outputs: five params are sent (W-01 regression)")
{
	std::vector<std::string> methods;
	std::vector<Json::Value> capturedParams;

	auto invoker = [&](const std::string& method, const Json::Value* pParams) -> RPC::Response
	{
		methods.push_back(method);
		capturedParams.push_back(pParams != nullptr ? *pParams : Json::Value(Json::nullValue));
		return RPC::Response::BuildResult(Json::Value(1), MakeOkArrayResult());
	};

	const auto pClient = RPCNodeClient::CreateWithTransport(invoker);
	const std::vector<Commitment> commitments = MakeCommitments(300);

	const auto outputs = pClient->GetOutputsByCommitment(commitments);
	REQUIRE(outputs.empty());

	// Grin++ rejects more than 128 commitments per request; the client must chunk.
	REQUIRE(methods.size() == 3);
	for (size_t i = 0; i < methods.size(); ++i)
	{
		REQUIRE(methods[i] == "get_outputs");
		REQUIRE(capturedParams[i].isArray());
		REQUIRE(capturedParams[i].size() == 5);
		REQUIRE(capturedParams[i][0].isArray());
		REQUIRE(capturedParams[i][1].isNull());
		REQUIRE(capturedParams[i][2].isNull());
		REQUIRE(capturedParams[i][3].isBool());
		REQUIRE(capturedParams[i][3].asBool() == false);
		REQUIRE(capturedParams[i][4].isBool());
		REQUIRE(capturedParams[i][4].asBool() == false);
	}

	REQUIRE(capturedParams[0][0].size() == 128);
	REQUIRE(capturedParams[1][0].size() == 128);
	REQUIRE(capturedParams[2][0].size() == 44);

	// An empty commitment set must not hit the node at all.
	methods.clear();
	capturedParams.clear();
	const auto pEmptyClient = RPCNodeClient::CreateWithTransport(invoker);
	REQUIRE(pEmptyClient->GetOutputsByCommitment({}).empty());
	REQUIRE(methods.empty());
}

TEST_CASE("RPCNodeClient: typed node errors instead of std::exception")
{
	const Commitment commitment = MakeCommitments(1).front();

	// Invalid params (e.g. a client bug such as the old missing params) must be
	// surfaced as a rejected request, not swallowed.
	auto rejectedInvoker = [](const std::string&, const Json::Value*) -> RPC::Response
	{
		return RPC::Response::BuildError(Json::Value(1), -32602, "Invalid params");
	};
	REQUIRE_THROWS_AS(
		RPCNodeClient::CreateWithTransport(rejectedInvoker)->GetChainHeight(),
		NodeRejectedException
	);

	// Busy / internal node errors and transport failures are retryable.
	auto unavailableInvoker = [](const std::string&, const Json::Value*) -> RPC::Response
	{
		return RPC::Response::BuildError(Json::Value(1), -32603, "Internal error");
	};
	REQUIRE_THROWS_AS(
		RPCNodeClient::CreateWithTransport(unavailableInvoker)->GetChainHeight(),
		NodeUnavailableException
	);

	auto throwingInvoker = [](const std::string&, const Json::Value*) -> RPC::Response
	{
		throw RPCException("RPCNodeClientTest", "connection refused", std::nullopt);
	};
	REQUIRE_THROWS_AS(
		RPCNodeClient::CreateWithTransport(throwingInvoker)->GetChainHeight(),
		NodeUnavailableException
	);

	// Malformed result (neither Ok nor Err) is not a valid negative answer.
	auto malformedInvoker = [](const std::string&, const Json::Value*) -> RPC::Response
	{
		return RPC::Response::BuildResult(Json::Value(1), Json::Value(Json::objectValue));
	};
	REQUIRE_THROWS_AS(
		RPCNodeClient::CreateWithTransport(malformedInvoker)->GetChainHeight(),
		NodeUnavailableException
	);

	// Grin++ style NOT_FOUND is a valid negative answer for get_kernel.
	auto grinppNotFoundInvoker = [](const std::string&, const Json::Value*) -> RPC::Response
	{
		Json::Value data(Json::objectValue);
		data["type"] = "NOT_FOUND";
		return RPC::Response::BuildError(Json::Value(1), -100, "Kernel not found", data);
	};
	REQUIRE(
		RPCNodeClient::CreateWithTransport(grinppNotFoundInvoker)->GetKernel(commitment).status
		== EKernelLookupStatus::NOT_FOUND
	);

	// Rust node style {"Err": "NotFound"} is also a valid negative answer.
	auto rustNotFoundInvoker = [](const std::string&, const Json::Value*) -> RPC::Response
	{
		Json::Value err("NotFound");
		Json::Value result;
		result["Err"] = err;
		return RPC::Response::BuildResult(Json::Value(1), result);
	};
	REQUIRE(
		RPCNodeClient::CreateWithTransport(rustNotFoundInvoker)->GetKernel(commitment).status
		== EKernelLookupStatus::NOT_FOUND
	);

	// A found kernel exposes its confirming height.
	auto foundInvoker = [](const std::string&, const Json::Value*) -> RPC::Response
	{
		Json::Value ok(Json::objectValue);
		ok["height"] = Json::UInt64(4242);
		Json::Value result;
		result["Ok"] = ok;
		return RPC::Response::BuildResult(Json::Value(1), result);
	};
	const KernelLookup found = RPCNodeClient::CreateWithTransport(foundInvoker)->GetKernel(commitment);
	REQUIRE(found.status == EKernelLookupStatus::FOUND);
	REQUIRE(found.height == 4242);
}

TEST_CASE("RPCNodeClient: push_transaction transport failure is typed (W1.2)")
{
	// A transport failure while posting must not escape as a raw RPCException
	// (it would bypass the wallet lifecycle handling in finalize/repost).
	auto throwingInvoker = [](const std::string&, const Json::Value*) -> RPC::Response
	{
		throw RPCException("RPCNodeClientTest", "connection refused", std::nullopt);
	};

	const auto pClient = RPCNodeClient::CreateWithTransport(throwingInvoker);
	REQUIRE_THROWS_AS(
		pClient->PostTransaction(std::make_shared<Transaction>(), EPoolType::MEMPOOL),
		NodeUnavailableException
	);
}

TEST_CASE("RPCNodeClient: GetTip reads height and hash from one get_tip call")
{
	std::string method;
	size_t calls = 0;

	auto invoker = [&](const std::string& invokedMethod, const Json::Value*) -> RPC::Response
	{
		method = invokedMethod;
		++calls;

		Json::Value ok(Json::objectValue);
		ok["height"] = Json::UInt64(777);
		ok["last_block_pushed"] = std::string(64, 'a');
		Json::Value result;
		result["Ok"] = ok;
		return RPC::Response::BuildResult(Json::Value(1), result);
	};

	const auto pClient = RPCNodeClient::CreateWithTransport(invoker);
	const auto tip = pClient->GetTip();
	REQUIRE(tip.has_value());
	REQUIRE(tip->height == 777);
	REQUIRE(method == "get_tip");
	REQUIRE(calls == 1);
}
