#pragma once

#include "../ChainState.h"

#include <Core/Models/FullBlock.h>
#include <BlockChain/BlockChainStatus.h>

enum class EBlockStatus
{
	NEXT_BLOCK,
	REORG,
	ORPHAN
};

class BlockProcessor
{
	struct BlockProcessingInfo
	{
		EBlockStatus status;
		std::vector<FullBlock::CPtr> reorgBlocks;
	};
public:
	// Result of one NRD-aware apply attempt against the txhashset. Public so the
	// classification/rebuild-once path can be unit-tested without a peer/pipe.
	enum class EApplyNrdAwareResult
	{
		Applied = 0,
		// The block is genuinely invalid (duplicate/bad rule with complete local
		// history, or any other ApplyBlock failure).
		InvalidBlock,
		// The local NRD recent-kernel index could not be rebuilt from local
		// headers/kernels. This is NOT evidence against the peer: the caller must
		// fail without the bad-block/ban classification.
		LocalNrdHistoryUnavailable
	};

	BlockProcessor(const std::shared_ptr<Locked<ChainState>>& pChainState);

	EBlockChainStatus ProcessBlock(const FullBlock& block);

	// True when the incremental kernel index could not be advanced for this
	// block (or a reorg could not be rewound). The caller must run the verified
	// Ensure() fallback after the block transaction is committed.
	bool KernelIndexNeedsRebuild() const noexcept { return m_kernelIndexNeedsRebuild; }

	// Apply one block. If the only reason for failure is a locally incomplete
	// NRD index, rebuild it once from the supplied parent header and retry the
	// apply before classifying the block.
	static EApplyNrdAwareResult ApplyBlockNrdAware(
		ITxHashSet& txHashSet,
		const std::shared_ptr<IBlockDB>& pBlockDB,
		const BlockHeader& previousHeader,
		const FullBlock& block);

private:
	EBlockChainStatus ProcessBlockInternal(const FullBlock& block);
	void HandleReorg(Writer<ChainState> pBatch, const std::vector<FullBlock::CPtr>& reorgBlocks);
	void ValidateAndAddBlock(const FullBlock& block, Writer<ChainState> pLockedState);

	BlockProcessingInfo DetermineBlockStatus(const FullBlock& block, Writer<ChainState> pLockedState);

	std::shared_ptr<Locked<ChainState>> m_pChainState;
	bool m_kernelIndexNeedsRebuild = false;
};