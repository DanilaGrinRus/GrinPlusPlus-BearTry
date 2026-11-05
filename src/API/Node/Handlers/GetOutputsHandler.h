#pragma once

#include <BlockChain/BlockChain.h>
#include <Core/Exceptions/TxHashSetException.h>
#include <Core/Models/Features.h>
#include <Core/Models/FullBlock.h>
#include <Core/Models/TransactionOutput.h>
#include <Core/Serialization/Serializer.h>
#include <Crypto/Hasher.h>
#include <Database/BlockDb.h>
#include <Database/Database.h>
#include <PMMR/TxHashSetManager.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <json/json.h>
#include <optional>
#include <string>
#include <vector>
#include <limits>
#include "PrintableSerialization.h"

// Grin v5.5.2 foreign API: get_outputs.
// Params: [[commits], start_height?, end_height?, include_proof?, include_merkle_proof?]
// and returns a flat array of OutputPrintable entries (not an OutputListing).
// Outputs still present in the output PMMR are unspent; spent outputs are pruned
// from the PMMR, so their body is read back from the block DB and spent=true.
// A height range is capped at MaxBlockRange blocks per request to bound the work.
class GetOutputsHandler : public RPCMethod
{
public:
	GetOutputsHandler(
		const IBlockChain::Ptr& pBlockChain,
		const TxHashSetManager::Ptr& pTxHashSetManager,
		const IDatabasePtr& pDatabase)
		: m_pBlockChain(pBlockChain),
		m_pTxHashSetManager(pTxHashSetManager),
		m_pDatabase(pDatabase) { }
	~GetOutputsHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value()) {
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray()) {
			return request.BuildError("INVALID_PARAMS", "Expected an array of parameters");
		}

		bool includeProof = false;
		if (params.size() > 3 && !params[3].isNull()) {
			includeProof = params[3].asBool();
		}

		for (int attempt = 0; attempt < 3; attempt++) {
			const auto snapshot = m_pBlockChain->GetApiStateSnapshot();
			if (!snapshot.valid || snapshot.tip == nullptr || snapshot.txHashSet == nullptr) {
				continue;
			}

			Json::Value outputs(Json::arrayValue);

			// By commitment. The DB lock is scoped to the lookup; it is never held
			// while a ChainState lock is acquired (that was the BlockDB/ChainState
			// ABBA path in A2).
			if (params.size() > 0 && params[0].isArray()) {
				if (params[0].size() > MaxCommitments) {
					return request.BuildError("INVALID_PARAMS", "Too many commitments (max 128)");
				}
				for (const Json::Value& commitJSON : params[0]) {
					if (!commitJSON.isString()) {
						return request.BuildError("INVALID_PARAMS", "Expected commitment hex strings");
					}

					std::unique_ptr<Commitment> pCommitment;
					try {
						pCommitment = std::make_unique<Commitment>(JsonUtil::ConvertToCommitment(commitJSON));
					} catch (const std::exception&) {
						return request.BuildError("INVALID_PARAMS", "Invalid commitment hex string");
					}
					const Commitment& commitment = *pCommitment;

					std::unique_ptr<OutputLocation> pLocation;
					{
						const auto pBlockDB = m_pDatabase->GetBlockDB()->Read();
						pLocation = pBlockDB->GetOutputPosition(commitment);
					}
					if (pLocation == nullptr) {
						continue; // Ignored like the reference client.
					}

					Json::Value outputJSON;
					std::string buildError;
					if (BuildOutput(commitment, *pLocation, snapshot.txHashSet, includeProof, outputJSON, buildError)) {
						outputs.append(outputJSON);
					} else if (!buildError.empty()) {
						return request.BuildError("INTERNAL_ERROR", buildError);
					}
				}
			}

			// By block height range.
			const bool hasRange = params.size() > 1 && !params[1].isNull();
			if (hasRange) {
				uint64_t startHeight = JsonUtil::ConvertToUInt64(params[1]);
				uint64_t endHeight = (params.size() > 2 && !params[2].isNull())
					? JsonUtil::ConvertToUInt64(params[2])
					: startHeight;
				if (endHeight < startHeight) {
					endHeight = startHeight;
				}
				// Clamp the width without overflowing when startHeight is near UINT64_MAX.
				const uint64_t maxSpan = MaxBlockRange - 1;
				if (endHeight - startHeight > maxSpan) {
					endHeight = (startHeight > (std::numeric_limits<uint64_t>::max)() - maxSpan)
						? (std::numeric_limits<uint64_t>::max)()
						: startHeight + maxSpan;
				}

				for (uint64_t height = startHeight;; height++) {
					// endHeight may be UINT64_MAX; never increment past it (wraparound).
					const bool lastHeight = (height == endHeight);
					std::unique_ptr<FullBlock> pBlock = m_pBlockChain->GetBlockByHeight(height);
					if (pBlock == nullptr) {
						if (lastHeight) {
							break;
						}
						continue;
					}

					const uint64_t count = pBlock->GetOutputs().size();
					const uint64_t cumulative = pBlock->GetHeader()->GetNumOutputs();
					if (cumulative < count) {
						return request.BuildError("INTERNAL_ERROR", "Inconsistent output count in block header");
					}
					uint64_t leaf = cumulative - count;
					for (const TransactionOutput& output : pBlock->GetOutputs()) {
						const OutputLocation location(LeafIndex::At(leaf++), height);
						try {
							outputs.append(PrintableSerialization::BlockOutputPrintable(
								output, location, snapshot.txHashSet, includeProof));
						} catch (const std::exception& e) {
							return request.BuildError("INTERNAL_ERROR", e.what());
						}
					}

					if (lastHeight) {
						break;
					}
				}
			}

			if (!m_pBlockChain->IsApiStateCurrent(snapshot.sequence)) {
				continue;
			}

			Json::Value result;
			result["Ok"] = outputs;
			return request.BuildResult(result);
		}

		return request.BuildError("NODE_BUSY_RETRY", "Chain state changed during the request");
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	static constexpr uint64_t MaxBlockRange = 100;
    static constexpr Json::ArrayIndex MaxCommitments = 128;

	bool BuildOutput(
		const Commitment& commitment,
		const OutputLocation& location,
		const std::shared_ptr<ITxHashSet>& pTxHashSet,
		const bool includeProof,
		Json::Value& outputJSON,
		std::string& error) const
	{
		EOutputFeatures features = EOutputFeatures::DEFAULT;
		std::optional<RangeProof> rangeProof;

		bool spent = true;
		if (pTxHashSet != nullptr) {
			try {
				const OutputDTO output = pTxHashSet->GetOutput(location);
				features = output.GetFeatures();
				rangeProof = output.GetRangeProof();
				spent = false;
			} catch (const TxHashSetException&) {
				// Expected: the output was pruned/spent; recover it from the block.
				spent = true;
			} catch (const std::exception& e) {
				error = e.what();
				return false;
			}
		}

		if (spent) {
			// The output was pruned from the PMMR; recover its body from the block DB.
			std::unique_ptr<FullBlock> pBlock = m_pBlockChain->GetBlockByHeight(location.GetBlockHeight());
			if (pBlock == nullptr) {
				return false;
			}

			bool found = false;
			for (const TransactionOutput& output : pBlock->GetOutputs()) {
				if (output.GetCommitment() == commitment) {
					features = output.GetFeatures();
					rangeProof = output.GetRangeProof();
					found = true;
					break;
				}
			}

			if (!found) {
				return false;
			}
		}

		if (!rangeProof.has_value()) {
			return false;
		}

		Serializer proofSerializer;
		rangeProof->Serialize(proofSerializer);

		outputJSON["output_type"] = (features == EOutputFeatures::COINBASE_OUTPUT) ? "Coinbase" : "Transaction";
		outputJSON["commit"] = commitment.ToHex();
		outputJSON["spent"] = spent;
		if (includeProof) {
			outputJSON["proof"] = rangeProof->Format();
		} else {
			outputJSON["proof"] = Json::nullValue;
		}
		outputJSON["proof_hash"] = Hasher::Blake2b(proofSerializer.GetBytes()).ToHex();
		outputJSON["block_height"] = Json::UInt64(location.GetBlockHeight());
		outputJSON["merkle_proof"] = Json::nullValue;
		outputJSON["mmr_index"] = Json::UInt64(location.GetLeafIndex().GetPosition() + 1);
		return true;
	}

	IBlockChain::Ptr m_pBlockChain;
	TxHashSetManager::Ptr m_pTxHashSetManager;
	IDatabasePtr m_pDatabase;
};
