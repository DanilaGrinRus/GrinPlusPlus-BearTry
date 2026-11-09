#pragma once

#include <BlockChain/BlockChain.h>
#include <PMMR/TxHashSet.h>
#include <Core/Models/BlockHeader.h>
#include <Core/Models/DTOs/OutputDTO.h>
#include <Core/Models/Features.h>
#include <Core/Models/FullBlock.h>
#include <Core/Models/TransactionKernel.h>
#include <Core/Serialization/Serializer.h>
#include <Crypto/Crypto.h>
#include <Crypto/Hasher.h>
#include <json/json.h>
#include <ctime>
#include <optional>
#include <string>

// Serializers shared by the grin v5.5.2 blocking/foreign API handlers.
// They produce the api::types printable shapes (BlockHeaderPrintable,
// TxKernelPrintable, OutputPrintable) which differ from the legacy Grin++
// REST JSON (different field names, RFC3339 timestamps, flat feature strings).
namespace PrintableSerialization
{
	inline std::string ToRfc3339(const int64_t timestamp)
	{
		std::time_t time(timestamp);
		std::tm tm = {};
#ifdef _WIN32
		gmtime_s(&tm, &time);
#else
		tm = *gmtime(&time);
#endif

		char buffer[32] = { 0 };
		std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+00:00", &tm);
		return std::string(buffer);
	}

	inline Json::Value HeaderPrintable(const BlockHeader& header)
	{
		Json::Value json;
		json["hash"] = header.GetHash().ToHex();
		json["version"] = Json::UInt(header.GetVersion());
		json["height"] = Json::UInt64(header.GetHeight());
		json["previous"] = header.GetPreviousHash().ToHex();
		json["prev_root"] = header.GetPreviousRoot().ToHex();
		json["timestamp"] = ToRfc3339(header.GetTimestamp());
		json["output_root"] = header.GetOutputRoot().ToHex();
		json["output_mmr_size"] = Json::UInt64(header.GetOutputMMRSize());
		json["range_proof_root"] = header.GetRangeProofRoot().ToHex();
		json["kernel_root"] = header.GetKernelRoot().ToHex();
		json["kernel_mmr_size"] = Json::UInt64(header.GetKernelMMRSize());
		json["nonce"] = Json::UInt64(header.GetNonce());
		json["edge_bits"] = Json::UInt(header.GetProofOfWork().GetEdgeBits());
		json["total_difficulty"] = Json::UInt64(header.GetTotalDifficulty());
		json["secondary_scaling"] = Json::UInt(header.GetScalingDifficulty());
		json["total_kernel_offset"] = header.GetTotalKernelOffset().GetBytes().ToHex();

		Json::Value cuckooSolution(Json::arrayValue);
		for (const uint64_t proofNonce : header.GetProofOfWork().GetProofNonces()) {
			cuckooSolution.append(Json::UInt64(proofNonce));
		}
		json["cuckoo_solution"] = cuckooSolution;

		return json;
	}

	inline Json::Value KernelPrintable(const TransactionKernel& kernel)
	{
		Json::Value json;
		json["features"] = KernelFeatures::ToString(kernel.GetFeatures());
		json["fee_shift"] = Json::UInt(kernel.GetFeeShift());
		json["fee"] = Json::UInt64(kernel.GetFee());
		json["lock_height"] = Json::UInt64(kernel.GetLockHeight());
		json["excess"] = kernel.GetExcessCommitment().ToHex();
		json["excess_sig"] = Crypto::ToCompact(kernel.GetExcessSignature()).ToHex();
		return json;
	}

	// Builds the flat OutputPrintable for an output that is part of a block body
	// (the block already carries features and the range proof).
	inline Json::Value BlockOutputPrintable(
		const TransactionOutput& blockOutput,
		const OutputLocation& location,
		const std::shared_ptr<ITxHashSet>& pTxHashSet,
		const bool includeProof)
	{
		bool spent = true;
		if (pTxHashSet != nullptr) {
			try {
				const OutputDTO liveOutput = pTxHashSet->GetOutput(location);
				spent = liveOutput.GetCommitment() != blockOutput.GetCommitment();
			} catch (const std::exception&) {
				spent = true;
			}
		}

		Serializer proofSerializer;
		blockOutput.GetRangeProof().Serialize(proofSerializer);

		Json::Value json;
		json["output_type"] = (blockOutput.GetFeatures() == EOutputFeatures::COINBASE_OUTPUT)
			? "Coinbase" : "Transaction";
		json["commit"] = blockOutput.GetCommitment().ToHex();
		json["spent"] = spent;
		json["proof"] = includeProof
			? Json::Value(blockOutput.GetRangeProof().Format())
			: Json::Value(Json::nullValue);
		json["proof_hash"] = Hasher::Blake2b(proofSerializer.GetBytes()).ToHex();
		json["block_height"] = Json::UInt64(location.GetBlockHeight());
		json["merkle_proof"] = Json::nullValue;
		json["mmr_index"] = Json::UInt64(location.GetLeafIndex().GetPosition() + 1);
		return json;
	}

	// Builds an OutputPrintable from an OutputLocation. Present PMMR leaves are
	// unspent; spent outputs are pruned from the PMMR and their body is recovered
	// from the block DB with spent=true.
	inline std::optional<Json::Value> OutputPrintable(
		const Commitment& commitment,
		const OutputLocation& location,
		const std::shared_ptr<ITxHashSet>& pTxHashSet,
		const IBlockChain::Ptr& pBlockChain,
		const bool includeProof)
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
			} catch (const std::exception&) {
				spent = true;
			}
		}

		if (spent) {
			std::unique_ptr<FullBlock> pBlock = pBlockChain->GetBlockByHeight(location.GetBlockHeight());
			if (pBlock == nullptr) {
				return std::nullopt;
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
				return std::nullopt;
			}
		}

		if (!rangeProof.has_value()) {
			return std::nullopt;
		}

		Serializer proofSerializer;
		rangeProof->Serialize(proofSerializer);

		Json::Value json;
		json["output_type"] = (features == EOutputFeatures::COINBASE_OUTPUT) ? "Coinbase" : "Transaction";
		json["commit"] = commitment.ToHex();
		json["spent"] = spent;
		if (includeProof) {
			json["proof"] = rangeProof->Format();
		} else {
			json["proof"] = Json::nullValue;
		}
		json["proof_hash"] = Hasher::Blake2b(proofSerializer.GetBytes()).ToHex();
		json["block_height"] = Json::UInt64(location.GetBlockHeight());
		json["merkle_proof"] = Json::nullValue;
		json["mmr_index"] = Json::UInt64(location.GetLeafIndex().GetPosition() + 1);
		return json;
	}
}
