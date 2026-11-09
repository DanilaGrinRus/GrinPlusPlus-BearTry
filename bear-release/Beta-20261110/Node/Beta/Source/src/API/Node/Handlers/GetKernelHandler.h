#pragma once

#include <BlockChain/BlockChain.h>
#include <Core/Models/Features.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Crypto.h>
#include <Database/Database.h>
#include <Database/BlockDb.h>
#include <Database/KernelIndexState.h>
#include <PMMR/TxHashSetManager.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <API/Wallet/Owner/Models/Errors.h>
#include <json/json.h>
#include <limits>
#include <optional>

// Grin v5.5.2 foreign API: get_kernel.
// Params: [excess, min_height?, max_height?] -> LocatedTxKernel
// {height, mmr_index, tx_kernel {features, fee_shift, fee, lock_height, excess,
// excess_sig}}. The search range is translated to kernel MMR indices exactly like
// chain::get_kernel_height: min -> previous_header.kernel_mmr_size + 1,
// max -> header(max).kernel_mmr_size, and the kernel height is recovered by
// binary-searching headers for the first one whose kernel_mmr_size covers the index.
class GetKernelHandler : public RPCMethod
{
public:
	GetKernelHandler(
		const IBlockChain::Ptr& pBlockChain,
		const TxHashSetManager::Ptr& pTxHashSetManager,
		const IDatabasePtr& pDatabase)
		: m_pBlockChain(pBlockChain), m_pTxHashSetManager(pTxHashSetManager), m_pDatabase(pDatabase) { }
	~GetKernelHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value()) {
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray() || params.empty() || params[0].isNull()) {
			return request.BuildError("INVALID_PARAMS", "Expected at least 1 parameter: excess");
		}

		Commitment excess;
		try {
			excess = JsonUtil::ConvertToCommitment(params[0]);
		} catch (const std::exception&) {
			return request.BuildError("INVALID_PARAMS", "Invalid excess hex string");
		}

		const BlockHeaderPtr pTip = m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTip == nullptr) {
			return request.BuildError("INTERNAL_ERROR", "Failed to find tip.");
		}
		const uint64_t headHeight = pTip->GetHeight();

		std::optional<uint64_t> minIndex;
		if (params.size() > 1 && !params[1].isNull()) {
			const uint64_t minHeight = JsonUtil::ConvertToUInt64(params[1]);
			if (minHeight > 0) {
				if (minHeight > headHeight) {
					return request.BuildError("NOT_FOUND", "Kernel not found");
				}
				const BlockHeaderPtr pPrevious = m_pBlockChain->GetBlockHeaderByHeight(minHeight - 1, EChainType::CONFIRMED);
				if (pPrevious == nullptr) {
					return request.BuildError("NOT_FOUND", "Block header not found for min_height - 1");
				}
				minIndex = pPrevious->GetKernelMMRSize() + 1;
			}
		}

		std::optional<uint64_t> maxIndex;
		if (params.size() > 2 && !params[2].isNull()) {
			const uint64_t maxHeight = JsonUtil::ConvertToUInt64(params[2]);
			if (maxHeight < headHeight) {
				const BlockHeaderPtr pHeader = m_pBlockChain->GetBlockHeaderByHeight(maxHeight, EChainType::CONFIRMED);
				if (pHeader == nullptr) {
					return request.BuildError("NOT_FOUND", "Block header not found for max_height");
				}
				maxIndex = pHeader->GetKernelMMRSize();
			}
		}

		const auto pTxHashSet = m_pTxHashSetManager->GetTxHashSet();
		if (pTxHashSet == nullptr) {
			return request.BuildError("INTERNAL_ERROR", "TxHashSet is not available.");
		}

		const TransactionKernel* pKernel = nullptr;
		std::optional<TransactionKernel> indexedKernel;
		// The direct-scan fallback must also live until serialization; pointing at a
		// block-local optional is a use-after-scope.
		std::optional<TransactionKernel> resolvedKernel;
		uint64_t mmrIndex = 0;
		uint64_t height = 0;

		// Fast path: persistent excess -> PMMR position index, bound to the exact
		// tip/root/count. Any mismatch falls back to the bounded direct scan.
		{
			std::unique_ptr<OutputLocation> pLocation;
			std::unique_ptr<KernelIndexState> pIndexState;
			{
				const auto pBlockDB = m_pDatabase->GetBlockDB()->Read();
				pIndexState = pBlockDB->GetKernelIndexState();
				if (pIndexState != nullptr) {
					pLocation = pBlockDB->GetKernelPosition(pIndexState->GetGeneration(), excess);
				}
			}

			const bool stateMatches =
				pIndexState != nullptr && pLocation != nullptr &&
				pIndexState->GetTipHash() == pTip->GetHash() &&
				pIndexState->GetTipHeight() == headHeight &&
				pIndexState->GetKernelRoot() == pTip->GetKernelRoot() &&
				pIndexState->GetKernelCount() == pTip->GetNumKernels() &&
				pIndexState->GetEntryCount() == pTip->GetNumKernels() &&
				pTxHashSet->GetNumKernels() == pTip->GetNumKernels();

			if (stateMatches) {
				const uint64_t leaf = pLocation->GetLeafIndex().Get();
				const uint64_t indexedPosition = pLocation->GetLeafIndex().GetPosition() + 1;
				const bool inRange =
					(!minIndex.has_value() || indexedPosition >= minIndex.value()) &&
					(!maxIndex.has_value() || indexedPosition <= maxIndex.value());

				std::unique_ptr<TransactionKernel> pIndexedKernel =
					pTxHashSet->GetKernelByLeafIndex(leaf);
				const BlockHeaderPtr pHeightHeader =
					m_pBlockChain->GetBlockHeaderByHeight(pLocation->GetBlockHeight(), EChainType::CONFIRMED);
				if (pIndexedKernel != nullptr && pHeightHeader != nullptr &&
					pIndexedKernel->GetExcessCommitment() == excess && inRange &&
					pLocation->GetBlockHeight() <= headHeight) {
					indexedKernel.emplace(std::move(*pIndexedKernel));
					pKernel = &indexedKernel.value();
					mmrIndex = indexedPosition;
					height = pLocation->GetBlockHeight();
				}
			}
		}

		if (pKernel == nullptr) {
			const auto kernelWithIndex = pTxHashSet->FindKernelByExcess(excess, minIndex, maxIndex);
			if (!kernelWithIndex.has_value()) {
				return request.BuildError("NOT_FOUND", "Kernel not found");
			}

			resolvedKernel.emplace(kernelWithIndex->first);
			pKernel = &resolvedKernel.value();
			mmrIndex = kernelWithIndex->second;
			height = FindHeightForKernelIndex(mmrIndex, headHeight);
			if (height == std::numeric_limits<uint64_t>::max()) {
				return request.BuildError("INTERNAL_ERROR", "Failed to locate the kernel height.");
			}
		}

		const TransactionKernel& kernel = *pKernel;
		Json::Value kernelJSON;
		kernelJSON["features"] = KernelFeatures::ToString(kernel.GetFeatures());
		kernelJSON["fee_shift"] = Json::UInt(kernel.GetFeeShift());
		kernelJSON["fee"] = Json::UInt64(kernel.GetFee());
		kernelJSON["lock_height"] = Json::UInt64(kernel.GetLockHeight());
		kernelJSON["excess"] = kernel.GetExcessCommitment().ToHex();
		kernelJSON["excess_sig"] = Crypto::ToCompact(kernel.GetExcessSignature()).ToHex();

		Json::Value located;
		located["height"] = Json::UInt64(height);
		located["mmr_index"] = Json::UInt64(mmrIndex);
		located["tx_kernel"] = kernelJSON;

		Json::Value result;
		result["Ok"] = located;
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	// First block height whose kernel MMR size covers the given 1-based index.
	uint64_t FindHeightForKernelIndex(const uint64_t mmrIndex, const uint64_t headHeight) const
	{
		uint64_t lo = 0;
		uint64_t hi = headHeight;
		while (lo < hi) {
			const uint64_t mid = lo + (hi - lo) / 2;
			const BlockHeaderPtr pHeader = m_pBlockChain->GetBlockHeaderByHeight(mid, EChainType::CONFIRMED);
			if (pHeader == nullptr) {
				return std::numeric_limits<uint64_t>::max();
			}

			if (pHeader->GetKernelMMRSize() >= mmrIndex) {
				hi = mid;
			} else {
				lo = mid + 1;
			}
		}

		return lo;
	}

	IBlockChain::Ptr m_pBlockChain;
	TxHashSetManager::Ptr m_pTxHashSetManager;
	IDatabasePtr m_pDatabase;
};
