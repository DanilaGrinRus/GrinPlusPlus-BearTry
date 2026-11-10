#pragma once

#include <Wallet/NodeClient.h>
#include <Net/Connections/HttpConnection.h>
#include <Core/Exceptions/UnimplementedException.h>
#include <Common/Logger.h>
#include <Common/Macros.h>
#include "../../../Wallet/SlateBuilder/PostTransactionResult.h"
#include <Common/Util/StringUtil.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// TODO: Implement caching & retry policies
class RPCNodeClient : public INodeClient
{
	enum class ENodeType
	{
		GRINPP,
		RUST
	};

public:
	//
	// Test seam: all JSON-RPC calls go through this function. Production code
	// uses the HttpConnection below; regression tests inject a canned transport
	// and assert the exact method/params sent over the wire.
	//
	using Invoker = std::function<RPC::Response(const std::string& method, const Json::Value* pParams)>;

	RPCNodeClient(const ENodeType type, HttpConnection::UPtr&& pConnection)
		: m_type(type), m_pConnection(std::move(pConnection)) { }

	// Creates a client backed by a test transport instead of a socket.
	static std::shared_ptr<RPCNodeClient> CreateWithTransport(Invoker invoker)
	{
		auto pClient = std::make_shared<RPCNodeClient>(ENodeType::RUST, HttpConnection::UPtr(nullptr));
		pClient->m_pInvoker = std::move(invoker);
		return pClient;
	}

	static INodeClientPtr Create(const std::string& host, const uint16_t port)
	{
		auto pConnection = HttpConnection::Connect(host, port);

		std::optional<RPC::Response> responseOpt;
		try
		{
			// NOTE: HttpConnection::Invoke(location, method) - the location (URL
			// path) must be the foreign API endpoint, the method is the JSON-RPC
			// method name.
			responseOpt.emplace(pConnection->Invoke("/v2/foreign", "get_version"));
		}
		catch (const RPCException& e)
		{
			throw NodeUnavailableException(StringUtil::Format("Failed to query node version: {}", e.what()));
		}

		if (!responseOpt.has_value() || !responseOpt->GetResult().has_value()) {
			throw NodeUnavailableException("Node did not return a version.");
		}

		std::string version;
		try
		{
			const Json::Value& result = responseOpt->GetResult().value();
			if (result.isObject() && result.isMember("Ok")) {
				version = JsonUtil::GetRequiredString(result["Ok"], "node_version");
			} else {
				throw NodeRejectedException("Unexpected get_version response.");
			}
		}
		catch (const NodeException&)
		{
			throw;
		}
		catch (const std::exception& e)
		{
			throw NodeRejectedException(StringUtil::Format("Unexpected get_version response: {}", e.what()));
		}

		ENodeType type = StringUtil::StartsWith(version, "Grin++") ? ENodeType::GRINPP : ENodeType::RUST;

		return std::make_shared<RPCNodeClient>(type, std::move(pConnection));
	}

	//
	// Returns the current confirmed chain height.
	//
	uint64_t GetChainHeight() const final
	{
		return JsonUtil::GetRequiredUInt64(Invoke("get_tip"), "height");
	}

	//
	// Returns the current confirmed chain tip (height + hash) from one get_tip call.
	//
	std::optional<NodeTip> GetTip() const final
	{
		const Json::Value result = Invoke("get_tip");

		NodeTip tip;
		tip.height = JsonUtil::GetRequiredUInt64(result, "height");
		tip.hash = Hash::FromHex(JsonUtil::GetRequiredString(result, "last_block_pushed"));
		return std::make_optional(tip);
	}

	//
	// Returns the current confirmed chain tip header.
	//
	BlockHeaderPtr GetTipHeader() const final
	{
		return GetBlockHeader(GetChainHeight());
	}

	//
	// Returns the header of the confirmed block at the given height, if it exists.
	//
	BlockHeaderPtr GetBlockHeader(const uint64_t height) const final
	{
		Json::Value params = Json::Value(Json::arrayValue);
		params.append(Json::UInt64(height));
		params.append(Json::nullValue);
		params.append(Json::nullValue);

		return BlockHeader::FromJSON(Invoke("get_header", params));
	}

	//
	// Returns the location (block height and mmr index) of each requested output, if it is *unspent*.
	// Commitments are chunked to stay within the Grin++ node limit (128) and the
	// reference wallet's default batch size (500). Grin++ rejects larger sets with
	// INVALID_PARAMS; Rust accepts them but the request becomes unnecessarily large.
	//
	std::map<Commitment, OutputLocation> GetOutputsByCommitment(const std::vector<Commitment>& commitments) const final
	{
		std::map<Commitment, OutputLocation> outputsByCommitment;

		for (size_t offset = 0; offset < commitments.size(); offset += MaxCommitmentsPerRequest)
		{
			const size_t end = (std::min)(commitments.size(), offset + MaxCommitmentsPerRequest);

			Json::Value commitsJson(Json::arrayValue);
			for (size_t i = offset; i < end; ++i) {
				commitsJson.append(commitments[i].ToHex());
			}

			const Json::Value response = Invoke("get_outputs", BuildGetOutputsParams(commitsJson));
			if (!response.isArray()) {
				throw NodeUnavailableException("get_outputs returned a malformed result.");
			}

			for (const auto& output : response)
			{
				if (JsonUtil::GetRequiredBool(output, "spent")) {
					continue;
				}

				Commitment commitment = JsonUtil::GetCommitment(output, "commit");
				outputsByCommitment.insert({ commitment, OutputLocation::FromJSON(output) });
			}
		}

		return outputsByCommitment;
	}

	//
	// Returns a vector containing block ids and their outputs for the given range.
	//
	std::vector<BlockWithOutputs> GetBlockOutputs(const uint64_t startHeight, const uint64_t maxHeight) const final
	{
		UNUSED(startHeight);
		UNUSED(maxHeight);

		throw UNIMPLEMENTED_EXCEPTION;
	}

	//
	// Legacy leaf-index scan. Kept for source compatibility; the wallet now uses
	// the position-based contract below. The remote API has always used PMMR
	// positions, so both entry points share the same implementation.
	//
	std::unique_ptr<OutputRange> GetOutputsByLeafIndex(const uint64_t startIndex, const uint64_t maxNumOutputs) const final
	{
		return GetOutputsByPosition(startIndex, maxNumOutputs);
	}

	//
	// Position-based unspent-output scan (get_unspent_outputs).
	//
	std::unique_ptr<OutputRange> GetOutputsByPosition(const uint64_t startPosition, const uint64_t maxNumOutputs) const final
	{
		Json::Value params(Json::arrayValue);
		params.append(Json::UInt64(startPosition));
		params.append(Json::nullValue);
		params.append(Json::UInt64(maxNumOutputs));
		params.append(true);

		const Json::Value response = Invoke("get_unspent_outputs", params);
		return std::make_unique<OutputRange>(OutputRange::FromJSON(response));
	}

	//
	// Output-PMMR size at the end of the given block height.
	//
	std::optional<uint64_t> GetOutputMMRSizeAtHeight(const uint64_t height) const final
	{
		Json::Value params(Json::arrayValue);
		params.append(Json::UInt64(height));
		params.append(Json::UInt64(height));

		try
		{
			const Json::Value result = Invoke("get_pmmr_indices", params);
			return std::make_optional(JsonUtil::GetRequiredUInt64(result, "highest_index"));
		}
		catch (const NodeException&)
		{
			return std::nullopt;
		}
	}

	//
	// Kernel lookup. A negative answer is a valid result and must not throw:
	// the refresh logic uses it to distinguish "spent" from "reverted".
	//
	KernelLookup GetKernel(const Commitment& excess) const final
	{
		Json::Value params(Json::arrayValue);
		params.append(excess.ToHex());
		params.append(Json::nullValue);
		params.append(Json::nullValue);

		std::optional<RPC::Response> responseOpt;
		try
		{
			responseOpt.emplace(Call("get_kernel", &params));
		}
		catch (const RPCException&)
		{
			return KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
		}

		if (!responseOpt.has_value())
		{
			return KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
		}

		const RPC::Response& response = responseOpt.value();
		if (response.GetError().has_value())
		{
			const RPC::Error& error = response.GetError().value();
			if (ErrorTypeEquals(error, "NOT_FOUND") ||
				error.GetMsg().find("not found") != std::string::npos ||
				error.GetMsg().find("Not found") != std::string::npos)
			{
				return KernelLookup{ EKernelLookupStatus::NOT_FOUND, 0 };
			}

			return KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
		}

		if (!response.GetResult().has_value())
		{
			return KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
		}

		const Json::Value& result = response.GetResult().value();
		if (result.isObject() && result.isMember("Ok") && result["Ok"].isObject())
		{
			KernelLookup lookup;
			lookup.status = EKernelLookupStatus::FOUND;
			try {
				lookup.height = JsonUtil::GetRequiredUInt64(result["Ok"], "height");
			} catch (const std::exception&) {
				lookup.height = 0;
			}
			return lookup;
		}

		if (result.isObject() && result.isMember("Err"))
		{
			const std::string serialized = JsonUtil::WriteCondensed(result["Err"]);
			if (serialized.find("NotFound") != std::string::npos) {
				return KernelLookup{ EKernelLookupStatus::NOT_FOUND, 0 };
			}
		}

		return KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
	}

	//
	// Posts the transaction to the P2P Network.
	//
	bool PostTransaction(TransactionPtr pTransaction, const EPoolType poolType) final
	{
		Json::Value params(Json::arrayValue);
		params.append(pTransaction->ToJSON());
		params.append(poolType == EPoolType::MEMPOOL);

		std::optional<RPC::Response> response;
		try
		{
			response.emplace(Call("push_transaction", &params));
		}
		catch (const std::exception& e)
		{
			// Transport failure is a node-unavailable condition, not a normal
			// rejection. Callers (WalletManager::Finalize/RepostTx) must see a
			// typed exception so they can keep the wallet state consistent and
			// the API returns a deterministic error.
			throw NodeUnavailableException(
				std::string("push_transaction transport failure: ") + e.what()
			);
		}

		const PostTransactionOutcome outcome = ParsePostTransactionResponse(response.value());
		if (outcome.status != EPostTransactionStatus::POSTED)
		{
			LOG_ERROR_F("push_transaction was not accepted: {}", outcome.error);
			return false;
		}

		return true;
	}

	//
	// Builds the exact JSON-RPC params for a get_outputs call. Public so
	// regression tests can pin the 5-element contract that Grin v5.5.2 requires.
	//
	static Json::Value BuildGetOutputsParams(const Json::Value& commitsJson)
	{
		Json::Value params(Json::arrayValue);
		params.append(commitsJson);
		params.append(Json::nullValue);
		params.append(Json::nullValue);
		params.append(false); // include_proof
		params.append(false); // include_merkle_proof
		return params;
	}

	static constexpr size_t MaxCommitmentsPerRequest = 128;

private:
	RPC::Response Call(const std::string& method, const Json::Value* pParams) const
	{
		if (m_pInvoker)
		{
			return m_pInvoker(method, pParams);
		}

		if (pParams != nullptr)
		{
			return m_pConnection->Invoke("/v2/foreign", method, *pParams);
		}

		return m_pConnection->Invoke("/v2/foreign", method);
	}

	Json::Value Invoke(const std::string& method, const Json::Value& params) const
	{
		std::optional<RPC::Response> responseOpt;
		try
		{
			responseOpt.emplace(Call(method, &params));
		}
		catch (const RPCException& e)
		{
			throw NodeUnavailableException(StringUtil::Format("RPC call '{}' failed: {}", method, e.what()));
		}

		if (!responseOpt.has_value())
		{
			throw NodeUnavailableException(StringUtil::Format("RPC call '{}' returned no response.", method));
		}

		return ExtractOk(method, responseOpt.value());
	}

	Json::Value Invoke(const std::string& method) const
	{
		std::optional<RPC::Response> responseOpt;
		try
		{
			responseOpt.emplace(Call(method, nullptr));
		}
		catch (const RPCException& e)
		{
			throw NodeUnavailableException(StringUtil::Format("RPC call '{}' failed: {}", method, e.what()));
		}

		if (!responseOpt.has_value())
		{
			throw NodeUnavailableException(StringUtil::Format("RPC call '{}' returned no response.", method));
		}

		return ExtractOk(method, responseOpt.value());
	}

	static Json::Value ExtractOk(const std::string& method, const RPC::Response& response)
	{
		if (response.GetError().has_value())
		{
			ThrowForRpcError(response.GetError().value());
		}

		if (!response.GetResult().has_value())
		{
			throw NodeUnavailableException(StringUtil::Format("RPC call '{}' returned no result.", method));
		}

		const Json::Value& result = response.GetResult().value();
		if (result.isObject())
		{
			if (result.isMember("Ok"))
			{
				return result["Ok"];
			}

			if (result.isMember("Err"))
			{
				ThrowForResultError(result["Err"]);
			}
		}

		throw NodeUnavailableException(StringUtil::Format("RPC call '{}' returned a malformed result.", method));
	}

	static bool ErrorTypeEquals(const RPC::Error& error, const std::string& expectedType)
	{
		const auto& data = error.GetData();
		if (!data.has_value() || !data.value().isObject())
		{
			return false;
		}

		const Json::Value& type = data.value()["type"];
		return type.isString() && type.asString() == expectedType;
	}

	static void ThrowForRpcError(const RPC::Error& error)
	{
		const std::string message = StringUtil::Format(
			"Node RPC error {}: {}",
			error.GetCode(),
			error.GetMsg()
		);

		if (ErrorTypeEquals(error, "NOT_FOUND")) {
			throw NodeNotFoundException(message);
		}

		if (ErrorTypeEquals(error, "NODE_BUSY_RETRY") ||
			error.GetCode() == RPC::ErrorCode::INTERNAL_ERROR) {
			throw NodeUnavailableException(message);
		}

		if (ErrorTypeEquals(error, "PARAMS_MISSING") ||
			ErrorTypeEquals(error, "INVALID_PARAMS") ||
			error.GetCode() == RPC::ErrorCode::INVALID_PARAMS ||
			error.GetCode() == RPC::ErrorCode::METHOD_NOT_FOUND) {
			throw NodeRejectedException(message);
		}

		throw NodeUnavailableException(message);
	}

	static void ThrowForResultError(const Json::Value& errorValue)
	{
		const std::string serialized = JsonUtil::WriteCondensed(errorValue);

		if (serialized.find("NotFound") != std::string::npos) {
			throw NodeNotFoundException(StringUtil::Format("Node returned error result: {}", serialized));
		}

		if (serialized.find("Argument") != std::string::npos) {
			throw NodeRejectedException(StringUtil::Format("Node returned error result: {}", serialized));
		}

		throw NodeUnavailableException(StringUtil::Format("Node returned error result: {}", serialized));
	}

	ENodeType m_type;
	HttpConnection::UPtr m_pConnection;
	Invoker m_pInvoker;
};
