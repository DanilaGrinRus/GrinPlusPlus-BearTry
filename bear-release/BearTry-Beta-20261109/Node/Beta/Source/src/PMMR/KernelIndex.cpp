#include <PMMR/KernelIndex.h>

#include <Core/Models/BlockHeader.h>
#include <Core/Models/TransactionKernel.h>
#include <Database/BlockDb.h>
#include <PMMR/TxHashSet.h>

#include <Common/Logger.h>

#include <memory>
#include <vector>

namespace KernelIndex
{
	namespace
	{
		bool StateMatchesChain(
			const KernelIndexState& state,
			const BlockHeaderPtr& pTip,
			const std::shared_ptr<ITxHashSet>& pTxHashSet)
		{
			const uint64_t expectedKernels = pTip->GetNumKernels();
			return state.GetTipHash() == pTip->GetHash() &&
				state.GetTipHeight() == pTip->GetHeight() &&
				state.GetKernelRoot() == pTip->GetKernelRoot() &&
				state.GetKernelCount() == expectedKernels &&
				state.GetEntryCount() == expectedKernels &&
				pTxHashSet->GetNumKernels() == expectedKernels;
		}
	}

	bool Ensure(
		const std::shared_ptr<IBlockDB>& pBlockDB,
		const std::shared_ptr<ITxHashSet>& pTxHashSet,
		const BlockHeaderPtr& pTip,
		const HeaderLookup& getHeaderByHeight)
	{
		if (pBlockDB == nullptr || pTxHashSet == nullptr || pTip == nullptr) {
			return false;
		}

		const uint64_t expectedKernels = pTip->GetNumKernels();
		const std::unique_ptr<KernelIndexState> pCurrent = pBlockDB->GetKernelIndexState();
		if (pCurrent != nullptr && StateMatchesChain(*pCurrent, pTip, pTxHashSet)) {
			const uint64_t generation = pCurrent->GetGeneration();
			if (pBlockDB->GetKernelPositionCount(generation) == expectedKernels &&
				pBlockDB->GetKernelPositionChecksum(generation) == pCurrent->GetChecksum()) {
				return true;
			}

			LOG_WARNING_F(
				"Kernel index generation {} matches tip {} but its contents are corrupt; rebuilding.",
				generation,
				pTip->GetHash());
		} else if (pCurrent != nullptr) {
			LOG_WARNING_F(
				"Kernel index is stale (indexed tip {}, chain tip {}); rebuilding.",
				pCurrent->GetTipHash(),
				pTip->GetHash());
		} else {
			LOG_INFO("Kernel index state is missing; building it from the chain.");
		}

		const uint64_t generation = (pCurrent != nullptr ? pCurrent->GetGeneration() : 0) + 1;
		const uint64_t previousGeneration = pCurrent != nullptr ? pCurrent->GetGeneration() : 0;

		// Build into an unpublished generation. Any interruption leaves the old
		// state record (and therefore the old complete generation) authoritative.
		pBlockDB->ClearKernelPositions(generation);

		uint64_t leaf = 0;
		Hash checksum;
		for (uint64_t height = 0; height <= pTip->GetHeight(); height++) {
			const BlockHeaderPtr pHeader = getHeaderByHeight(height);
			if (pHeader == nullptr) {
				LOG_WARNING_F("Kernel index rebuild stopped at height {}: header missing.", height);
				return false;
			}

			const uint64_t cumulativeKernels = pHeader->GetNumKernels();
			if (cumulativeKernels < leaf) {
				LOG_WARNING_F("Kernel index rebuild found a non-monotonic kernel count at height {}.", height);
				return false;
			}

			for (; leaf < cumulativeKernels; leaf++) {
				const std::unique_ptr<TransactionKernel> pKernel = pTxHashSet->GetKernelByLeafIndex(leaf);
				if (pKernel == nullptr) {
					LOG_WARNING_F("Kernel index rebuild could not read kernel leaf {}.", leaf);
					return false;
				}

				const OutputLocation location(LeafIndex::At(leaf), height);
				pBlockDB->AddKernelPosition(generation, pKernel->GetExcessCommitment(), location);
				checksum = KernelIndexXorHash(checksum, pKernel->GetExcessCommitment(), location);
			}
		}

		if (leaf != expectedKernels || pTxHashSet->GetNumKernels() != expectedKernels) {
			LOG_WARNING_F(
				"Kernel index rebuild is incomplete: {} leaves written, {} expected.",
				leaf,
				expectedKernels);
			return false;
		}

		// Our BlockDB is a batch writer; make the unpublished generation durable
		// before reading it back. The state record is still not published.
		pBlockDB->Commit();

		// Read the new generation back before publishing it. A short/failed write
		// must not become authoritative.
		const uint64_t verifiedCount = pBlockDB->GetKernelPositionCount(generation);
		const Hash verifiedChecksum = pBlockDB->GetKernelPositionChecksum(generation);
		if (verifiedCount != expectedKernels || verifiedChecksum != checksum) {
			LOG_WARNING_F(
				"Kernel index rebuild verification failed: count {}/{}, checksum {} vs {}.",
				verifiedCount,
				expectedKernels,
				verifiedChecksum,
				checksum);
			return false;
		}

		pBlockDB->SetKernelIndexState(KernelIndexState(
			generation,
			pTip->GetHeight(),
			pTip->GetHash(),
			pTip->GetKernelRoot(),
			expectedKernels,
			expectedKernels,
			checksum));
		pBlockDB->Commit();

		LOG_INFO_F(
			"Kernel index published: generation {}, {} kernels, tip {}.",
			generation,
			expectedKernels,
			pTip->GetHash());

		// Best-effort cleanup of the superseded generation. The active generation
		// is already published, so a cleanup failure cannot expose a partial index.
		try {
			if (previousGeneration != 0 && previousGeneration != generation) {
				pBlockDB->ClearKernelPositions(previousGeneration);
			}
		} catch (const std::exception& e) {
			LOG_WARNING_F("Failed to remove kernel index generation {}: {}", previousGeneration, e.what());
		}

		return true;
	}
}
