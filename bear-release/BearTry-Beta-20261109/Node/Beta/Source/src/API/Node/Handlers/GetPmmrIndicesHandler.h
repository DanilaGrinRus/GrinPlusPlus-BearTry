#pragma once

#include <BlockChain/BlockChain.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <json/json.h>
#include <optional>

// Grin v5.5.2 foreign API: get_pmmr_indices.
// Maps a block height range to PMMR index range (no output bodies):
// start = output_mmr_size(height-1)+1 (0 for height 0), end = output_mmr_size(height).
// Matches chain::block_height_range_to_pmmr_indices exactly.
class GetPmmrIndicesHandler : public RPCMethod
{
public:
	GetPmmrIndicesHandler(const IBlockChain::Ptr& pBlockChain)
		: m_pBlockChain(pBlockChain) { }
	~GetPmmrIndicesHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value()) {
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray() || params.empty() || params[0].isNull()) {
			return request.BuildError("INVALID_PARAMS", "Expected at least 1 parameter: start_block_height");
		}

		const uint64_t startBlockHeight = JsonUtil::ConvertToUInt64(params[0]);

		uint64_t endBlockHeight = 0;
		if (params.size() > 1 && !params[1].isNull()) {
			endBlockHeight = JsonUtil::ConvertToUInt64(params[1]);
		} else {
			const BlockHeaderPtr pTip = m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
			if (pTip == nullptr) {
				return request.BuildError("INTERNAL_ERROR", "Failed to find tip.");
			}
			endBlockHeight = pTip->GetHeight();
		}

		if (endBlockHeight < startBlockHeight) {
			return request.BuildError("INVALID_PARAMS", "end_block_height is before start_block_height");
		}

		uint64_t startMmrSize = 0;
		if (startBlockHeight > 0) {
			const BlockHeaderPtr pPrevious = m_pBlockChain->GetBlockHeaderByHeight(startBlockHeight - 1, EChainType::CONFIRMED);
			if (pPrevious == nullptr) {
				return request.BuildError("NOT_FOUND", "Block header not found for start_block_height - 1");
			}
			startMmrSize = pPrevious->GetOutputMMRSize() + 1;
		}

		const BlockHeaderPtr pEnd = m_pBlockChain->GetBlockHeaderByHeight(endBlockHeight, EChainType::CONFIRMED);
		if (pEnd == nullptr) {
			return request.BuildError("NOT_FOUND", "Block header not found for end_block_height");
		}
		const uint64_t endMmrSize = pEnd->GetOutputMMRSize();

		Json::Value listing;
		listing["last_retrieved_index"] = Json::UInt64(startMmrSize);
		listing["highest_index"] = Json::UInt64(endMmrSize);
		listing["outputs"] = Json::Value(Json::arrayValue);

		Json::Value result;
		result["Ok"] = listing;
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IBlockChain::Ptr m_pBlockChain;
};
