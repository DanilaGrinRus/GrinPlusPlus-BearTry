#include <PMMR/KernelIndex.h>

#include <Core/Models/BlockHeader.h>
#include <Core/Models/TransactionKernel.h>
#include <Database/BlockDb.h>
#include <PMMR/TxHashSet.h>

#include <Common/Logger.h>
#include <Common/Util/ScopedTiming.h>

#include <memory>
#include <string>
#include <vector>

namespace KernelIndex
{
	namespace
	{
		bool StateMatchesHeader(
			const KernelIndexState& state,
			const BlockHeaderPtr& pHeader,
			const std::shared_ptr<ITxHashSet>& pTxHashSet)
		{
			const uint64_t expectedKernels = pHeader->GetNumKernels();
			return state.GetTipHash() == pHeader->GetHash() &&
				state.GetTipHeight() == pHeader->GetHeight() &&
				state.GetKernelRoot() == pHeader->GetKernelRoot() &&
				state.GetKernelCount() == expectedKernels &&
				state.GetEntryCount() == expectedKernels &&
				pTxHashSet->GetNumKernels() == expectedKernels;
		}

		bool StateMatchesTipWithoutTxHashSet(
			const KernelIndexState& state,
			const BlockHeaderPtr& pHeader)
		{
			return state.GetTipHash() == pHeader->GetHash() &&
				state.GetTipHeight() == pHeader->GetHeight() &&
				state.GetKernelRoot() == pHeader->GetKernelRoot() &&
				state.GetKernelCount() == pHeader->GetNumKernels() &&
				state.GetEntryCount() == state.GetKernelCount();
		}

		Hash XorHashes(const Hash& left, const Hash& right)
		{
			auto bytes = left.GetData();
			const auto& other = right.GetData();
			for (size_t i = 0; i < bytes.size() && i < other.size(); ++i) {
				bytes[i] ^= other[i];
			}
			return Hash(bytes);
		}

		void InvalidateState(const std::shared_ptr<IBlockDB>& pBlockDB, const uint64_t generation)
		{
			// Keep the generation number so the next rebuild reuses that slot and
			// clears its (possibly partial) entries. All chain-binding fields are
			// deliberately empty; Ensure() will classify this as stale and rebuild.
			pBlockDB->SetKernelIndexState(KernelIndexState(
				generation, 0, Hash(), Hash(), 0, 0, Hash()));
		}

		// Extend only a verified ancestor. Entries and the new state are written
		// in the same BlockDB transaction, so a crash cannot publish a partial
		// extension. Large gaps and every reorg use the existing full rebuild.
		bool TryExtendAncestor(
			const std::shared_ptr<IBlockDB>& pBlockDB,
			const std::shared_ptr<ITxHashSet>& pTxHashSet,
			const BlockHeaderPtr& pTip,
			const HeaderLookup& getHeaderByHeight,
			const KernelIndexState& current,
			std::string& reason)
		{
			constexpr uint64_t MAX_HEIGHT_GAP = 4096;
			constexpr uint64_t MAX_KERNEL_GAP = 32768;
			const uint64_t tipHeight = pTip->GetHeight();
			const uint64_t expectedKernels = pTip->GetNumKernels();
			if (current.GetTipHeight() >= tipHeight) {
				reason = "state_at_or_after_chain_tip";
				return false;
			}
			const uint64_t heightGap = tipHeight - current.GetTipHeight();
			if (heightGap > MAX_HEIGHT_GAP) {
				reason = "height_gap=" + std::to_string(heightGap) +
					" exceeds " + std::to_string(MAX_HEIGHT_GAP);
				return false;
			}
			if (current.GetKernelCount() > expectedKernels) {
				reason = "state_kernel_count_ahead";
				return false;
			}
			const uint64_t kernelGap = expectedKernels - current.GetKernelCount();
			if (kernelGap > MAX_KERNEL_GAP) {
				reason = "kernel_gap=" + std::to_string(kernelGap) +
					" exceeds " + std::to_string(MAX_KERNEL_GAP);
				return false;
			}
			if (current.GetEntryCount() != current.GetKernelCount()) {
				reason = "entry_count_mismatch";
				return false;
			}
			if (pTxHashSet->GetNumKernels() != expectedKernels) {
				reason = "txhashset_kernel_count_mismatch";
				return false;
			}

			const BlockHeaderPtr pAncestor = getHeaderByHeight(current.GetTipHeight());
			if (pAncestor == nullptr) {
				reason = "ancestor_header_missing";
				return false;
			}
			if (pAncestor->GetHash() != current.GetTipHash()) {
				reason = "ancestor_hash_mismatch";
				return false;
			}
			if (pAncestor->GetKernelRoot() != current.GetKernelRoot()) {
				reason = "ancestor_kernel_root_mismatch";
				return false;
			}
			if (pAncestor->GetNumKernels() != current.GetKernelCount()) {
				reason = "ancestor_kernel_count_mismatch";
				return false;
			}

			const auto digest = pBlockDB->GetKernelPositionDigest(current.GetGeneration());
			if (digest.first != current.GetEntryCount() || digest.second != current.GetChecksum()) {
				reason = "index_digest_mismatch";
				return false;
			}

			const auto entriesStart = std::chrono::steady_clock::now();
			std::vector<std::pair<Commitment, OutputLocation>> entries;
			entries.reserve(static_cast<size_t>(kernelGap));
			uint64_t leaf = current.GetKernelCount();
			Hash checksum = current.GetChecksum();
			for (uint64_t height = current.GetTipHeight() + 1; height <= tipHeight; ++height) {
				const BlockHeaderPtr pHeader = getHeaderByHeight(height);
				if (pHeader == nullptr || pHeader->GetNumKernels() < leaf ||
					pHeader->GetNumKernels() > expectedKernels) {
					reason = "header_kernel_count_not_monotonic_at_" + std::to_string(height);
					return false;
				}
				for (; leaf < pHeader->GetNumKernels(); ++leaf) {
					const auto pKernel = pTxHashSet->GetKernelByLeafIndex(leaf);
					if (pKernel == nullptr) {
						reason = "kernel_leaf_missing_" + std::to_string(leaf);
						return false;
					}
					const OutputLocation location(LeafIndex::At(leaf), height);
					entries.emplace_back(pKernel->GetExcessCommitment(), location);
					checksum = KernelIndexXorHash(checksum, pKernel->GetExcessCommitment(), location);
				}
			}
			if (leaf != expectedKernels) {
				reason = "extension_leaf_count_mismatch";
				return false;
			}
			const auto entriesMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - entriesStart).count();

			ScopedTiming commitTiming("kernel_index_extend_commit");
			try {
				for (const auto& entry : entries) {
					pBlockDB->AddKernelPosition(current.GetGeneration(), entry.first, entry.second);
				}
				pBlockDB->SetKernelIndexState(KernelIndexState(
					current.GetGeneration(), tipHeight, pTip->GetHash(), pTip->GetKernelRoot(),
					expectedKernels, expectedKernels, checksum));
				pBlockDB->Commit();
				commitTiming.Success();
			} catch (...) {
				pBlockDB->Rollback();
				throw;
			}

			LOG_INFO_F(
				"kernel_index_extension generation={} from_height={} to_height={} new_kernels={} entries_ms={} tip={}",
				current.GetGeneration(),
				current.GetTipHeight(),
				tipHeight,
				entries.size(),
				entriesMs,
				pTip->GetHash());
			return true;
		}
	}

	bool AppendBlock(
		const std::shared_ptr<IBlockDB>& pBlockDB,
		const std::shared_ptr<ITxHashSet>& pTxHashSet,
		const BlockHeaderPtr& pPreviousHeader,
		const BlockHeaderPtr& pHeader)
	{
		if (pBlockDB == nullptr || pTxHashSet == nullptr ||
			pPreviousHeader == nullptr || pHeader == nullptr) {
			return false;
		}

		const std::unique_ptr<KernelIndexState> pState = pBlockDB->GetKernelIndexState();
		if (pState == nullptr) {
			LOG_DEBUG("kernel_index_append skipped: state missing");
			return false;
		}

		// The fast path must start exactly at the parent header. No digest scan is
		// done per block; the full digest is checked at startup and by Ensure().
		if (!StateMatchesTipWithoutTxHashSet(*pState, pPreviousHeader)) {
			LOG_DEBUG_F(
				"kernel_index_append skipped: state tip {}/{} != parent {}/{}",
				pState->GetTipHash(),
				pState->GetTipHeight(),
				pPreviousHeader->GetHash(),
				pPreviousHeader->GetHeight());
			return false;
		}

		const uint64_t newKernelCount = pHeader->GetNumKernels();
		if (newKernelCount < pState->GetKernelCount() ||
			pTxHashSet->GetNumKernels() != newKernelCount) {
			LOG_DEBUG_F(
				"kernel_index_append skipped: kernel counts state={} header={} txhashset={}",
				pState->GetKernelCount(),
				newKernelCount,
				pTxHashSet->GetNumKernels());
			return false;
		}

		std::vector<std::pair<Commitment, OutputLocation>> entries;
		entries.reserve(static_cast<size_t>(newKernelCount - pState->GetKernelCount()));
		Hash checksum = pState->GetChecksum();
		for (uint64_t leaf = pState->GetKernelCount(); leaf < newKernelCount; ++leaf) {
			const std::unique_ptr<TransactionKernel> pKernel = pTxHashSet->GetKernelByLeafIndex(leaf);
			if (pKernel == nullptr) {
				LOG_DEBUG_F("kernel_index_append skipped: kernel leaf {} missing", leaf);
				return false;
			}
			const OutputLocation location(LeafIndex::At(leaf), pHeader->GetHeight());
			entries.emplace_back(pKernel->GetExcessCommitment(), location);
			checksum = KernelIndexXorHash(checksum, pKernel->GetExcessCommitment(), location);
		}

		for (const auto& entry : entries) {
			pBlockDB->AddKernelPosition(pState->GetGeneration(), entry.first, entry.second);
		}
		pBlockDB->SetKernelIndexState(KernelIndexState(
			pState->GetGeneration(),
			pHeader->GetHeight(),
			pHeader->GetHash(),
			pHeader->GetKernelRoot(),
			newKernelCount,
			newKernelCount,
			checksum));

		LOG_DEBUG_F(
			"kernel_index_append height={} new_kernels={} total_kernels={} tip={}",
			pHeader->GetHeight(),
			entries.size(),
			newKernelCount,
			pHeader->GetHash());
		return true;
	}

	bool RewindToAncestor(
		const std::shared_ptr<IBlockDB>& pBlockDB,
		const BlockHeaderPtr& pAncestor,
		const BlockHeaderPtr& pExpectedTip,
		const HeaderLookup& getHeaderByHeight)
	{
		if (pBlockDB == nullptr || pAncestor == nullptr || pExpectedTip == nullptr) {
			return false;
		}

		const std::unique_ptr<KernelIndexState> pState = pBlockDB->GetKernelIndexState();
		if (pState == nullptr || !StateMatchesTipWithoutTxHashSet(*pState, pExpectedTip)) {
			LOG_WARNING("kernel_index_rewind skipped: index state does not match the pre-reorg tip");
			return false;
		}
		if (pAncestor->GetHeight() > pExpectedTip->GetHeight() ||
			pAncestor->GetNumKernels() > pState->GetKernelCount()) {
			LOG_WARNING("kernel_index_rewind skipped: invalid ancestor height/kernel count");
			return false;
		}

		// Verify the target is an ancestor of the pre-reorg tip via header links.
		Hash expectedHash = pAncestor->GetHash();
		for (uint64_t height = pAncestor->GetHeight() + 1; height <= pExpectedTip->GetHeight(); ++height) {
			const BlockHeaderPtr pHeader = getHeaderByHeight(height);
			if (pHeader == nullptr || pHeader->GetPreviousHash() != expectedHash) {
				LOG_WARNING_F("kernel_index_rewind skipped: header link missing at height {}", height);
				return false;
			}
			expectedHash = pHeader->GetHash();
		}
		if (expectedHash != pExpectedTip->GetHash()) {
			LOG_WARNING("kernel_index_rewind skipped: ancestor chain does not end at the pre-reorg tip");
			return false;
		}

		const uint64_t targetKernelCount = pAncestor->GetNumKernels();
		const uint64_t expectedRemoved = pState->GetEntryCount() - targetKernelCount;
		const auto removed = pBlockDB->RemoveKernelPositionsFrom(
			pState->GetGeneration(), targetKernelCount);
		if (removed.first != expectedRemoved) {
			// Never leave a partially rewound index active: invalidate the state in
			// the same transaction so the caller's fallback rebuild starts clean.
			InvalidateState(pBlockDB, pState->GetGeneration());
			LOG_ERROR_F(
				"kernel_index_rewind removed {} entries, expected {}; invalidated generation {}",
				removed.first,
				expectedRemoved,
				pState->GetGeneration());
			return false;
		}

		const Hash checksum = XorHashes(pState->GetChecksum(), removed.second);
		pBlockDB->SetKernelIndexState(KernelIndexState(
			pState->GetGeneration(),
			pAncestor->GetHeight(),
			pAncestor->GetHash(),
			pAncestor->GetKernelRoot(),
			targetKernelCount,
			targetKernelCount,
			checksum));

		LOG_INFO_F(
			"kernel_index_rewind generation={} from_height={} to_height={} removed={} remaining={} tip={}",
			pState->GetGeneration(),
			pState->GetTipHeight(),
			pAncestor->GetHeight(),
			removed.first,
			targetKernelCount,
			pAncestor->GetHash());
		return true;
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
		if (pCurrent == nullptr) {
			LOG_INFO_F(
				"kernel_index_check state=missing chain_height={} chain_kernels={} txhashset_kernels={}",
				pTip->GetHeight(),
				expectedKernels,
				pTxHashSet->GetNumKernels());
		} else {
			LOG_INFO_F(
				"kernel_index_check state=present generation={} indexed_height={} indexed_kernels={} entries={} chain_height={} chain_kernels={} txhashset_kernels={}",
				pCurrent->GetGeneration(),
				pCurrent->GetTipHeight(),
				pCurrent->GetKernelCount(),
				pCurrent->GetEntryCount(),
				pTip->GetHeight(),
				expectedKernels,
				pTxHashSet->GetNumKernels());
		}

		if (pCurrent != nullptr && StateMatchesHeader(*pCurrent, pTip, pTxHashSet)) {
			const uint64_t generation = pCurrent->GetGeneration();
			ScopedTiming verifyTiming("kernel_index_digest_verify");
			const auto digest = pBlockDB->GetKernelPositionDigest(generation);
			verifyTiming.Success();
			if (digest.first == expectedKernels && digest.second == pCurrent->GetChecksum()) {
				LOG_INFO_F(
					"kernel_index_check result=current generation={} kernels={} tip={}",
					generation,
					expectedKernels,
					pTip->GetHash());
				return true;
			}

			LOG_WARNING_F(
				"Kernel index generation {} matches tip {} but its contents are corrupt; rebuilding.",
				generation,
				pTip->GetHash());
		} else if (pCurrent != nullptr) {
			std::string extendReason;
			if (TryExtendAncestor(pBlockDB, pTxHashSet, pTip, getHeaderByHeight, *pCurrent, extendReason)) {
				LOG_INFO_F(
					"kernel_index_check result=extended generation={} kernels={} tip={}",
					pCurrent->GetGeneration(),
					expectedKernels,
					pTip->GetHash());
				return true;
			}
			LOG_WARNING_F(
				"Kernel index is stale (indexed tip {}, height {}, kernel_count {}; chain tip {}, height {}, kernel_count {}); rebuilding. extend_rejected={}",
				pCurrent->GetTipHash(),
				pCurrent->GetTipHeight(),
				pCurrent->GetKernelCount(),
				pTip->GetHash(),
				pTip->GetHeight(),
				expectedKernels,
				extendReason);
		} else {
			LOG_INFO_F(
				"Kernel index state is missing; building it for tip {} height {} ({} kernels).",
				pTip->GetHash(),
				pTip->GetHeight(),
				expectedKernels);
		}

		const uint64_t generation = (pCurrent != nullptr ? pCurrent->GetGeneration() : 0) + 1;
		const uint64_t previousGeneration = pCurrent != nullptr ? pCurrent->GetGeneration() : 0;

		// Build into an unpublished generation. Any interruption leaves the old
		// state record (and therefore the old complete generation) authoritative.
		{
			ScopedTiming clearTiming("kernel_index_rebuild_clear_generation");
			pBlockDB->ClearKernelPositions(generation);
			clearTiming.Success();
		}

		const auto rebuildStart = std::chrono::steady_clock::now();
		uint64_t leaf = 0;
		Hash checksum;
		const uint64_t progressStep = 250000;
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
				if (leaf != 0 && leaf % progressStep == 0) {
					const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now() - rebuildStart).count();
					LOG_INFO_F(
						"kernel_index_rebuild_progress generation={} kernels={} total={} elapsed_ms={}",
						generation,
						leaf,
						expectedKernels,
						elapsedMs);
				}
			}
		}
		if (leaf != expectedKernels || pTxHashSet->GetNumKernels() != expectedKernels) {
			LOG_WARNING_F(
				"Kernel index rebuild is incomplete: {} leaves written, {} expected.",
				leaf,
				expectedKernels);
			return false;
		}
		const auto entriesMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - rebuildStart).count();
		LOG_INFO_F(
			"kernel_index_rebuild_entries generation={} kernels={} elapsed_ms={}",
			generation,
			expectedKernels,
			entriesMs);

		{
			ScopedTiming commitTiming("kernel_index_rebuild_commit_entries");
			// Our BlockDB is a batch writer; make the unpublished generation durable
			// before reading it back. The state record is still not published.
			pBlockDB->Commit();
			commitTiming.Success();
		}

		{
			ScopedTiming verifyTiming("kernel_index_rebuild_verify");
			// Read the new generation back before publishing it. A short/failed write
			// must not become authoritative.
			const auto digest = pBlockDB->GetKernelPositionDigest(generation);
			const uint64_t verifiedCount = digest.first;
			const Hash verifiedChecksum = digest.second;
			if (verifiedCount != expectedKernels || verifiedChecksum != checksum) {
				LOG_WARNING_F(
					"Kernel index rebuild verification failed: count {}/{}, checksum {} vs {}.",
					verifiedCount,
					expectedKernels,
					verifiedChecksum,
					checksum);
				return false;
			}
			verifyTiming.Success();
		}

		{
			ScopedTiming publishTiming("kernel_index_rebuild_publish");
			pBlockDB->SetKernelIndexState(KernelIndexState(
				generation,
				pTip->GetHeight(),
				pTip->GetHash(),
				pTip->GetKernelRoot(),
				expectedKernels,
				expectedKernels,
				checksum));
			pBlockDB->Commit();
			publishTiming.Success();
		}

		LOG_INFO_F(
			"Kernel index published: generation {}, {} kernels, tip {}.",
			generation,
			expectedKernels,
			pTip->GetHash());

		// Best-effort cleanup of the superseded generation. The active generation
		// is already published, so a cleanup failure cannot expose a partial index.
		{
			ScopedTiming cleanupTiming("kernel_index_rebuild_cleanup");
			try {
				if (previousGeneration != 0 && previousGeneration != generation) {
					pBlockDB->ClearKernelPositions(previousGeneration);
				}
				cleanupTiming.Success();
			} catch (const std::exception& e) {
				LOG_WARNING_F("Failed to remove kernel index generation {}: {}", previousGeneration, e.what());
			}
		}

		return true;
	}
}
