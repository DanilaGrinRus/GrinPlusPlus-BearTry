#pragma once

#include <BlockChain/BlockChain.h>
#include <Database/BlockDb.h>
#include <Database/Database.h>
#include <PMMR/TxHashSetManager.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <json/json.h>
#include <algorithm>
#include <optional>

#include "PrintableSerialization.h"

// Grin v5.5.2 foreign API: get_blocks.
// Params: [start_height, end_height?, max?, include_proof?] ->
// BlockListing {last_retrieved_height, blocks: [BlockPrintable]} where
// BlockPrintable is the flat {header, inputs, outputs, kernels} shape.
// max is capped at BLOCK_TRANSFER_LIMIT (1000) like the reference handler.
class GetBlocksHandler : public RPCMethod
{
public:
	GetBlocksHandler(
		const IBlockChain::Ptr& pBlockChain,
		const TxHashSetManager::Ptr& pTxHashSetManager,
		const IDatabasePtr& pDatabase)
		: m_pBlockChain(pBlockChain),
		m_pTxHashSetManager(pTxHashSetManager),
		m_pDatabase(pDatabase) { }
	~GetBlocksHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value()) {
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray() || params.empty() || params[0].isNull()) {
			return request.BuildError("INVALID_PARAMS", "Expected at least 1 parameter: start_height");
		}

		const uint64_t startHeight = JsonUtil::ConvertToUInt64(params[0]);

		std::optional<uint64_t> explicitEndHeight;
		if (params.size() > 1 && !params[1].isNull()) {
			explicitEndHeight = JsonUtil::ConvertToUInt64(params[1]);
		}

		uint64_t max = BlockTransferLimit;
		if (params.size() > 2 && !params[2].isNull()) {
			max = JsonUtil::ConvertToUInt64(params[2]);
		}
		max = (std::min)(max == 0 ? BlockTransferLimit : max, (uint64_t)BlockTransferLimit);

		bool includeProof = false;
		if (params.size() > 3 && !params[3].isNull()) {
			includeProof = params[3].asBool();
		}

		for (int attempt = 0; attempt < 3; attempt++) {
			const auto snapshot = m_pBlockChain->GetApiStateSnapshot();
			if (!snapshot.valid || snapshot.tip == nullptr || snapshot.txHashSet == nullptr) {
				continue;
			}

			const uint64_t endHeight = explicitEndHeight.value_or(snapshot.tip->GetHeight());
			if (endHeight < startHeight) {
				return request.BuildError("INVALID_PARAMS", "end_height is before start_height");
			}
			// max limits returned blocks, not the number of DB lookups for missing
			// historical blocks. Bound work even across sparse/compacted ranges.
			if (endHeight - startHeight >= MaxScannedHeights) {
				return request.BuildError("QUERY_TOO_WIDE", "Request at most 10000 heights per call");
			}

			Json::Value blocks(Json::arrayValue);
			uint64_t lastRetrievedHeight = startHeight;

			for (uint64_t height = startHeight;; height++) {
				lastRetrievedHeight = height;
				// endHeight may be UINT64_MAX; never increment past it (wraparound).
				const bool lastHeight = (height == endHeight);

				std::unique_ptr<FullBlock> pBlock = m_pBlockChain->GetBlockByHeight(height);
				if (pBlock == nullptr) {
					if (lastHeight) {
						break;
					}
					continue;
				}

				Json::Value blockJSON;
				blockJSON["header"] = PrintableSerialization::HeaderPrintable(*pBlock->GetHeader());

				Json::Value inputs(Json::arrayValue);
				for (const TransactionInput& input : pBlock->GetInputs()) {
					inputs.append(input.GetCommitment().ToHex());
				}
				blockJSON["inputs"] = inputs;

				Json::Value outputs(Json::arrayValue);
				const uint64_t count = pBlock->GetOutputs().size();
				const uint64_t cumulative = pBlock->GetHeader()->GetNumOutputs();
				if (cumulative < count) {
					return request.BuildError("INTERNAL_ERROR", "Inconsistent output count in block header");
				}
				uint64_t leaf = cumulative - count;
				for (const TransactionOutput& output : pBlock->GetOutputs()) {
					const OutputLocation location(LeafIndex::At(leaf++), height);
					outputs.append(PrintableSerialization::BlockOutputPrintable(
						output, location, snapshot.txHashSet, includeProof));
				}
				blockJSON["outputs"] = outputs;

				Json::Value kernels(Json::arrayValue);
				for (const TransactionKernel& kernel : pBlock->GetKernels()) {
					kernels.append(PrintableSerialization::KernelPrintable(kernel));
				}
				blockJSON["kernels"] = kernels;

				blocks.append(blockJSON);
				if (blocks.size() >= max || lastHeight) {
					break;
				}
			}

			if (!m_pBlockChain->IsApiStateCurrent(snapshot.sequence)) {
				continue;
			}

			Json::Value listing;
			listing["last_retrieved_height"] = Json::UInt64(lastRetrievedHeight);
			listing["blocks"] = blocks;

			Json::Value result;
			result["Ok"] = listing;
			return request.BuildResult(result);
		}

		return request.BuildError("NODE_BUSY_RETRY", "Chain state changed during the request");
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	static constexpr uint64_t BlockTransferLimit = 1000;
    static constexpr uint64_t MaxScannedHeights = 10000;

	IBlockChain::Ptr m_pBlockChain;
	TxHashSetManager::Ptr m_pTxHashSetManager;
	IDatabasePtr m_pDatabase;
};
