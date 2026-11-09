#pragma once

#include "KernelMMR.h"
#include "OutputPMMR.h"
#include "RangeProofPMMR.h"

#include <PMMR/TxHashSet.h>
#include <Core/Config.h>
#include <cstddef>
#include <shared_mutex>
#include <string>
#include <map>
#include <vector>

class TxHashSet : public ITxHashSet
{
public:
	struct RecentKernelPosition
	{
		uint64_t MmrPosition = 0;
		uint64_t Height = 0;
	};

	// Outcome of the NRD pre-check for the most recent ApplyBlock call.
	// Diagnostic only: consensus behavior is the bool returned by ApplyBlock.
	enum class ENrdValidationResult
	{
		NONE = 0,
		ACCEPTED,
		INVALID_RELATIVE_HEIGHT,
		DUPLICATE_IN_BLOCK,
		MISSING_LOCAL_HISTORY
	};

	TxHashSet(
		std::shared_ptr<KernelMMR> pKernelMMR,
		std::shared_ptr<OutputPMMR> pOutputPMMR,
		std::shared_ptr<RangeProofPMMR> pRangeProofPMMR,
		BlockHeaderPtr pBlockHeader
	);
	virtual ~TxHashSet() = default;

	BlockHeaderPtr GetFlushedBlockHeader() const noexcept final { return m_pBlockHeaderBackup; }

	bool IsValid(std::shared_ptr<const IBlockDB> pBlockDB, const Transaction& transaction) const final;
	std::unique_ptr<BlockSums> ValidateTxHashSet(const BlockHeader& header, const IBlockChain& blockChain, SyncStatus& syncStatus) final;
	bool ApplyBlock(std::shared_ptr<IBlockDB> pBlockDB, const FullBlock& block) final;
	bool ValidateRoots(const BlockHeader& blockHeader) const final;
	TxHashSetRoots GetRoots(const std::shared_ptr<const IBlockDB>& pBlockDB, const TransactionBody& body) final;
	void SaveOutputPositions(const Chain::CPtr& pChain, std::shared_ptr<IBlockDB> pBlockDB) const final;

	std::vector<Hash> GetLastKernelHashes(const uint64_t numberOfKernels) const final;
	std::vector<Hash> GetLastOutputHashes(const uint64_t numberOfOutputs) const final;
	std::vector<Hash> GetLastRangeProofHashes(const uint64_t numberOfRangeProofs) const final;
	OutputRange GetOutputsByLeafIndex(std::shared_ptr<const IBlockDB> pBlockDB, const uint64_t startIndex, const uint64_t maxNumOutputs) const final;
	std::vector<OutputDTO> GetOutputsByMMRIndex(std::shared_ptr<const IBlockDB> pBlockDB, const uint64_t startIndex, const uint64_t lastIndex) const final;
	uint64_t GetNumKernels() const final;
	std::unique_ptr<TransactionKernel> GetKernelByLeafIndex(const uint64_t leafIndex) const final;
	void RebuildRecentKernelIndex(
		const std::shared_ptr<const IBlockDB>& pBlockDB,
		const BlockHeader& tip) final;

	// Same rebuild, but does NOT publish the result as the last-commit backup.
	// Use inside an uncommitted ChainState batch (e.g. an alternative reorg
	// branch): a later Rollback must restore the index of the last COMMIT, not
	// this intermediate branch. A rebuild exception is contained here and
	// reported via DidLastNrdRebuildThrow(); it never escapes to P2P callers.
	void RebuildRecentKernelIndexPreservingBaseline(
		const std::shared_ptr<const IBlockDB>& pBlockDB,
		const BlockHeader& tip);

	OutputDTO GetOutput(const OutputLocation& location) const final;
	std::optional<std::pair<TransactionKernel, uint64_t>> FindKernelByExcess(
		const Commitment& excess,
		const std::optional<uint64_t>& minIndex,
		const std::optional<uint64_t>& maxIndex
	) const final;

	void Rewind(std::shared_ptr<IBlockDB> pBlockDB, const BlockHeader& header) final;
	void Commit() final;
	void Rollback() noexcept final;
	void Compact() final;

	std::shared_ptr<KernelMMR> GetKernelMMR() { return m_pKernelMMR; }
	std::shared_ptr<OutputPMMR> GetOutputPMMR() { return m_pOutputPMMR; }
	std::shared_ptr<RangeProofPMMR> GetRangeProofPMMR() { return m_pRangeProofPMMR; }

	// Read-only NRD index introspection for tests and diagnostics. The index is
	// only published (complete == true) after a full successful rebuild or after
	// normal block application on top of a complete index.
	bool IsRecentNrdIndexComplete() const noexcept { return m_recentNrdIndexComplete; }
	ENrdValidationResult GetLastNrdValidationResult() const noexcept { return m_lastNrdValidationResult; }
	// True when the most recent rebuild could not complete because a local read
	// threw. Callers must classify this as local-unverifiable (no peer ban).
	bool DidLastNrdRebuildThrow() const noexcept { return m_lastNrdRebuildThrew; }
	std::size_t GetRecentNrdKernelCount() const noexcept;
	std::vector<RecentKernelPosition> GetRecentNrdEntries(const Commitment& excess) const;

private:
	std::shared_ptr<KernelMMR> m_pKernelMMR;
	std::shared_ptr<OutputPMMR> m_pOutputPMMR;
	std::shared_ptr<RangeProofPMMR> m_pRangeProofPMMR;

	BlockHeaderPtr m_pBlockHeader;
	BlockHeaderPtr m_pBlockHeaderBackup;

	std::map<Commitment, std::vector<RecentKernelPosition>> m_recentNrdKernels;
	std::map<Commitment, std::vector<RecentKernelPosition>> m_recentNrdKernelsBackup;
	bool m_recentNrdIndexComplete = false;
	bool m_recentNrdIndexCompleteBackup = false;
	ENrdValidationResult m_lastNrdValidationResult = ENrdValidationResult::NONE;

	bool CheckAndStageNrd(const TransactionKernel& kernel, const uint64_t height);
	void ApplyNrd(const TransactionKernel& kernel, const uint64_t position, const uint64_t height);
	void RemoveNrdAtOrAbove(const Commitment& excess, const uint64_t minimumPosition);
	void PublishRecentNrdIndexAsBackup();
	void RebuildRecentKernelIndexInternal(
		const std::shared_ptr<const IBlockDB>& pBlockDB,
		const BlockHeader& tip,
		const bool publishBaseline);
	bool m_lastNrdRebuildThrew = false;
};
