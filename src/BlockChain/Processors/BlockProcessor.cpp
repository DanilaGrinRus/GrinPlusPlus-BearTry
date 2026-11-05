#include "BlockProcessor.h"
#include "BlockHeaderProcessor.h"
#include "../Validators/BlockValidator.h"

#include <Consensus.h>
#include <Core/Exceptions/BlockChainException.h>
#include <Core/Exceptions/BadDataException.h>
#include <Core/Validation/KernelSumValidator.h>
#include <Common/Logger.h>
#include <Common/Util/HexUtil.h>
#include <Common/Util/StringUtil.h>
#include "../../PMMR/TxHashSetImpl.h"
#include <PMMR/KernelIndex.h>
#include <algorithm>
#include <stdexcept>

namespace
{
	// Internal control-flow signal for "the local NRD history is incomplete".
	// It must never escape ProcessBlock: BlockPipe bans on exceptions and on
	// INVALID, so this is converted to EBlockChainStatus::UNKNOWN_ERROR instead.
	class LocalNrdHistoryUnavailableException final : public std::runtime_error
	{
	public:
		explicit LocalNrdHistoryUnavailableException(const Hash& blockHash)
			: std::runtime_error("local NRD recent-kernel history is incomplete; block cannot be validated locally"),
			m_blockHash(blockHash)
		{
		}

		const Hash& GetBlockHash() const noexcept { return m_blockHash; }

	private:
		Hash m_blockHash;
	};
}

BlockProcessor::BlockProcessor(const std::shared_ptr<Locked<ChainState>>& pChainState)
	: m_pChainState(pChainState) { }

EBlockChainStatus BlockProcessor::ProcessBlock(const FullBlock& block)
{	
	m_kernelIndexNeedsRebuild = false;
	const uint64_t candidateHeight = m_pChainState->Read()->GetHeight(EChainType::CANDIDATE);
	const uint64_t horizonHeight = Consensus::GetHorizonHeight(candidateHeight);

	BlockHeaderPtr pHeader = block.GetHeader();
	if (pHeader->GetHeight() <= horizonHeight) {
		throw BAD_DATA_EXCEPTION(EBanReason::BadBlock, "Can't process blocks beyond horizon.");
	}

	// Make sure header is processed and valid before processing block.
	BlockHeaderProcessor(m_pChainState).ProcessSingleHeader(pHeader);

	// Verify block is self-consistent before locking
	BlockValidator::VerifySelfConsistent(block);

	const EBlockChainStatus returnStatus = ProcessBlockInternal(block);
	if (returnStatus == EBlockChainStatus::SUCCESS) {
		LOG_DEBUG_F("Block {} successfully processed.", *pHeader);
	}

	return returnStatus;
}

EBlockChainStatus BlockProcessor::ProcessBlockInternal(const FullBlock& block)
{
	auto pBatch = m_pChainState->BatchWrite();
	auto pChainStore = pBatch->GetChainStore();
	auto pOrphanPool = pBatch->GetOrphanPool();
	auto pConfirmedChain = pChainStore->GetConfirmedChain();

	// 1. Check if already part of confirmed chain
	if (pConfirmedChain->IsOnChain(block.GetHeader())) {
		LOG_TRACE_F("Block {} already part of confirmed chain.", block);
		return EBlockChainStatus::ALREADY_EXISTS;
	}

	// 2. Check if block has already been processed.
	if (pBatch->GetBlockDB()->GetBlock(block.GetHash()) != nullptr) {
		LOG_TRACE_F("Block {} already processed.", block);
		return EBlockChainStatus::ALREADY_EXISTS;
	}

	// 3. Orphan if block should be processed as an orphan
	const BlockProcessingInfo info = DetermineBlockStatus(block, pBatch);
	if (info.status == EBlockStatus::ORPHAN) {
		if (pOrphanPool->IsOrphan(block.GetHeight(), block.GetHash())) {
			LOG_TRACE_F("Block {} already processed as an orphan.", block);
			return EBlockChainStatus::ALREADY_EXISTS;
		}

		pOrphanPool->AddOrphanBlock(block);

		return EBlockChainStatus::ORPHANED;
	}

	// A reorg needs the bodies of the blocks above the fork point to rewind the
	// TxHashSet safely. A bodyless/PIBD-like state can legitimately lack them; do
	// not accept the alternative branch and discover an unrecoverable MMR/tip
	// mismatch only on the next restart. Refuse before any state mutation.
	if (info.status == EBlockStatus::REORG)
	{
		const auto pBlockDB = pBatch->GetBlockDB();
		const uint64_t forkHeight = info.reorgBlocks.front()->GetHeight() - 1;
		for (uint64_t height = pConfirmedChain->GetHeight(); height > forkHeight; height--)
		{
			const Hash& hash = pConfirmedChain->GetHash(height);
			const auto pBody = pBlockDB->GetBlock(hash);
			const bool hasBody = pBody != nullptr &&
				(!pBody->GetInputs().empty() || !pBody->GetOutputs().empty() || !pBody->GetKernels().empty());
			if (!hasBody)
			{
				LOG_WARNING_F(
					"Refusing bodyless reorg: body for confirmed block {} at height {} is unavailable; "
					"txhashset rewind cannot be proven without mutating state.",
					hash, height);
				return EBlockChainStatus::LOCAL_REWIND_DATA_MISSING;
			}

			try
			{
				const auto spentPositions = pBlockDB->GetSpentPositions(hash);
				for (const TransactionInput& input : pBody->GetInputs())
				{
					if (spentPositions.find(input.GetCommitment()) == spentPositions.cend())
					{
						LOG_WARNING_F(
							"Refusing reorg: spent position for commitment {} in confirmed block {} "
							"at height {} is missing; local rewind data is incomplete.",
							input.GetCommitment(), hash, height);
						return EBlockChainStatus::LOCAL_REWIND_DATA_MISSING;
					}
				}
			}
			catch (const std::exception& e)
			{
				LOG_WARNING_F(
					"Refusing reorg: spent-position set for confirmed block {} at height {} "
					"could not be read ({}); local rewind data is incomplete.",
					hash, height, e.what());
				return EBlockChainStatus::LOCAL_REWIND_DATA_MISSING;
			}
		}
	}

	try
	{
		if (info.status == EBlockStatus::REORG) {
			HandleReorg(pBatch, info.reorgBlocks);
		} else {
			assert(info.status == EBlockStatus::NEXT_BLOCK);

			ValidateAndAddBlock(block, pBatch);
			pConfirmedChain->AddBlock(block.GetHash(), block.GetHeight());
			pBatch->Commit();
		}
	}
	catch (const LocalNrdHistoryUnavailableException& e)
	{
		// Do NOT rethrow and do NOT return INVALID: BlockPipe::ProcessNewBlock bans
		// the peer on both INVALID and any exception. Returning UNKNOWN_ERROR
		// keeps the peer unbanned. The batched Writer destructor rolls back the
		// uncommitted batch (ChainState::Rollback -> TxHashSet::Rollback restores
		// the NRD index snapshot), so no partially applied state is published.
		LOG_WARNING_F(
			"Block {} is locally unverifiable because the NRD recent-kernel history is incomplete ({}); returning UNKNOWN_ERROR without a bad-block/ban classification.",
			e.GetBlockHash(),
			e.what());
		return EBlockChainStatus::UNKNOWN_ERROR;
	}

	return EBlockChainStatus::SUCCESS;
}

BlockProcessor::EApplyNrdAwareResult BlockProcessor::ApplyBlockNrdAware(
	ITxHashSet& txHashSet,
	const std::shared_ptr<IBlockDB>& pBlockDB,
	const BlockHeader& previousHeader,
	const FullBlock& block)
{
	if (pBlockDB == nullptr) {
		return EApplyNrdAwareResult::InvalidBlock;
	}

	if (txHashSet.ApplyBlock(pBlockDB, block)) {
		return EApplyNrdAwareResult::Applied;
	}

	// Only a locally missing NRD index is not peer evidence. Every other
	// ApplyBlock failure keeps the existing bad-block classification.
	TxHashSet* pConcrete = dynamic_cast<TxHashSet*>(&txHashSet);
	if (pConcrete == nullptr ||
		pConcrete->GetLastNrdValidationResult() != TxHashSet::ENrdValidationResult::MISSING_LOCAL_HISTORY) {
		return EApplyNrdAwareResult::InvalidBlock;
	}

	LOG_WARNING_F(
		"Local NRD recent-kernel index is incomplete; rebuilding at parent {} height {} before retrying block {}.",
		previousHeader.GetHash(),
		previousHeader.GetHeight(),
		block.GetHash());
	try
	{
		// Preserving-baseline: this runs inside an uncommitted ChainState batch
		// (possibly a reorg alternative branch). A later Rollback must restore the
		// index of the last COMMIT, not this intermediate branch.
		pConcrete->RebuildRecentKernelIndexPreservingBaseline(pBlockDB, previousHeader);
	}
	catch (const std::exception& e)
	{
		// Defensive: the rebuild contains local read errors itself
		// (DidLastNrdRebuildThrow), but no exception may ever escape towards
		// BlockPipe, which bans on an escaped exception.
		LOG_ERROR_F(
			"NRD index rebuild threw while validating block {} locally: {}",
			block.GetHash(),
			e.what());
		return EApplyNrdAwareResult::LocalNrdHistoryUnavailable;
	}

	if (!pConcrete->IsRecentNrdIndexComplete()) {
		LOG_ERROR_F(
			"NRD recent-kernel index could not be rebuilt from local headers/kernels at height {} (local_read_error={}); block {} stays unclassified (no ban evidence).",
			previousHeader.GetHeight(),
			pConcrete->DidLastNrdRebuildThrow(),
			block.GetHash());
		return EApplyNrdAwareResult::LocalNrdHistoryUnavailable;
	}

	if (txHashSet.ApplyBlock(pBlockDB, block)) {
		return EApplyNrdAwareResult::Applied;
	}

	// The index is now complete, so a still-failing block is genuinely invalid.
	return EApplyNrdAwareResult::InvalidBlock;
}

BlockProcessor::BlockProcessingInfo BlockProcessor::DetermineBlockStatus(const FullBlock& block, Writer<ChainState> pBatch)
{
	auto pOrphanPool = pBatch->GetOrphanPool();
	auto pBlockDB = pBatch->GetBlockDB();
	auto pChainStore = pBatch->GetChainStore();
	auto pConfirmedChain = pChainStore->GetConfirmedChain();

	auto pStoreHeader = pBlockDB->GetBlockHeader(block.GetHash());
	if (pStoreHeader == nullptr) {
		LOG_TRACE_F("Header has not yet been validated. Treating {} as orphan.", block);
		return { EBlockStatus::ORPHAN, {} };
	}

	if (pConfirmedChain->GetTipHash() == block.GetPreviousHash()) {
		LOG_TRACE_F("Block {} is the next block. Processing it now.", block);
		return { EBlockStatus::NEXT_BLOCK, {} };
	}

	auto pForkBlock = std::make_shared<const FullBlock>(block);

	std::vector<FullBlock::CPtr> reorgBlocks({ pForkBlock });
	while (!pConfirmedChain->IsOnChain(pForkBlock->GetHeight() - 1, pForkBlock->GetPreviousHash()))
	{
		Hash previousHash = pForkBlock->GetPreviousHash();
		pForkBlock = pOrphanPool->GetOrphanBlock(pForkBlock->GetHeight() - 1, previousHash);
		if (pForkBlock == nullptr) {
			pForkBlock = pBlockDB->GetBlock(previousHash);
		}

		if (pForkBlock == nullptr) {
			LOG_TRACE_F("Missing previous block. Treating {} as orphan.", block);
			return { EBlockStatus::ORPHAN, {} };
		}

		reorgBlocks.push_back(pForkBlock);
	}

	std::reverse(reorgBlocks.begin(), reorgBlocks.end());

	if (pConfirmedChain->GetHeight() >= reorgBlocks.front()->GetHeight()) {
		LOG_WARNING_F("Fork detected at height {}.", reorgBlocks.front()->GetHeight());
	}

	return { EBlockStatus::REORG, reorgBlocks };
}

void BlockProcessor::HandleReorg(Writer<ChainState> pBatch, const std::vector<FullBlock::CPtr>& reorgBlocks)
{
	const uint64_t totalDifficulty = pBatch->GetTotalDifficulty(EChainType::CONFIRMED);

	auto pTxHashSet = pBatch->GetTxHashSetManager()->GetTxHashSet();
	if (pTxHashSet == nullptr) {
		LOG_ERROR("TxHashSet is null. Still syncing?");
		throw BLOCK_CHAIN_EXCEPTION("TxHashSet is null");
	}

	auto pBlockDB = pBatch->GetBlockDB();
	auto pCommonHeader = pBlockDB->GetBlockHeader(reorgBlocks.front()->GetPreviousHash());
	if (pCommonHeader == nullptr) {
		LOG_ERROR_F("Failed to find header {}", reorgBlocks.front()->GetPreviousHash());
		throw BLOCK_CHAIN_EXCEPTION("Failed to find header.");
	}

	// TODO: Add rewound transactions back to TxPool

	pTxHashSet->Rewind(pBlockDB, *pCommonHeader);

	// Rewind the kernel index to the same verified ancestor before the fork
	// blocks are re-applied. The pre-reorg confirmed tip is the expected state
	// tip; if it does not match, leave the index untouched and let the caller
	// run the full verified rebuild after this batch commits.
	{
		auto pConfirmedChainBefore = pBatch->GetChainStore()->GetConfirmedChain();
		const BlockHeaderPtr pOldTipHeader = pBlockDB->GetBlockHeader(pConfirmedChainBefore->GetTipHash());
		auto getHeaderByHeight = [&pBlockDB, &pConfirmedChainBefore](const uint64_t height) -> BlockHeaderPtr {
			const auto pIndex = pConfirmedChainBefore->GetByHeight(height);
			return pIndex == nullptr ? nullptr : pBlockDB->GetBlockHeader(pIndex->GetHash());
		};
		if (pOldTipHeader == nullptr ||
			!KernelIndex::RewindToAncestor(pBlockDB, pCommonHeader, pOldTipHeader, getHeaderByHeight)) {
			LOG_WARNING("Kernel index could not be rewound with the reorg; a verified rebuild is required after commit.");
			m_kernelIndexNeedsRebuild = true;
		}
	}

	for (const FullBlock::CPtr& pBlock : reorgBlocks)
	{
		ValidateAndAddBlock(*pBlock, pBatch);
	}

	if (reorgBlocks.back()->GetTotalDifficulty() > totalDifficulty)
	{
		auto pConfirmedChain = pBatch->GetChainStore()->GetConfirmedChain();
		pConfirmedChain->Rewind(reorgBlocks.front()->GetHeight() - 1);
		for (const FullBlock::CPtr& pBlock : reorgBlocks)
		{
			pConfirmedChain->AddBlock(pBlock->GetHash(), pBlock->GetHeight());
		}

		pBatch->Commit();
	}
	else
	{
		// All blocks are valid, but since total difficulty did not increase, rollback all changes.
		// We will then re-add the blocks to the BlockDB so we know they've been validated.
		pBatch->Rollback();

		for (const FullBlock::CPtr& pBlock : reorgBlocks)
		{
			pBlockDB->AddBlock(*pBlock);
		}

		pBlockDB->Commit();
	}
}

void BlockProcessor::ValidateAndAddBlock(const FullBlock& block, Writer<ChainState> pBatch)
{
	auto pOrphanPool = pBatch->GetOrphanPool();
	auto pBlockDB = pBatch->GetBlockDB();
	auto pTxHashSet = pBatch->GetTxHashSetManager()->GetTxHashSet();
	auto pTxPool = pBatch->GetTransactionPool();

	const Hash& previousHash = block.GetPreviousHash();
	auto pPreviousHeader = pBlockDB->GetBlockHeader(previousHash);
	if (pPreviousHeader == nullptr) {
		LOG_ERROR_F("Failed to find header {}", previousHash);
		throw BLOCK_CHAIN_EXCEPTION("Previous header not found.");
	}

	if (pTxHashSet == nullptr) {
		throw BAD_DATA_EXCEPTION_F(EBanReason::BadBlock, "Failed to apply block {} to the TxHashSet.", block);
	}

	const EApplyNrdAwareResult applyResult =
		ApplyBlockNrdAware(*pTxHashSet, pBlockDB, *pPreviousHeader, block);
	if (applyResult == EApplyNrdAwareResult::LocalNrdHistoryUnavailable) {
		// Caught in ProcessBlockInternal and converted to UNKNOWN_ERROR (no ban).
		throw LocalNrdHistoryUnavailableException(block.GetHash());
	}

	if (applyResult != EApplyNrdAwareResult::Applied) {
		throw BAD_DATA_EXCEPTION_F(EBanReason::BadBlock, "Failed to apply block {} to the TxHashSet.", block);
	}

	BlockValidator::VerifySelfConsistent(block);

	if (!pTxHashSet->ValidateRoots(*block.GetHeader())) {
		throw BAD_DATA_EXCEPTION_F(EBanReason::BadBlock, "Failed to validate TxHashSet roots for block {}.", block);
	}

	std::unique_ptr<BlockSums> pPreviousBlockSums = pBlockDB->GetBlockSums(previousHash);
	if (pPreviousBlockSums == nullptr) {
		LOG_WARNING_F("Failed to retrieve block sums for block {}", previousHash);
		throw BLOCK_CHAIN_EXCEPTION("Failed to retrieve block sums.");
	}

	BlockSums blockSums = KernelSumValidator::ValidateKernelSums(
		block.GetTransactionBody(),
		0 - Consensus::REWARD,
		block.GetTotalKernelOffset(),
		std::make_optional(*pPreviousBlockSums)
	);

	pBlockDB->AddBlockSums(block.GetHash(), blockSums);
	pBlockDB->AddBlock(block);
	pOrphanPool->RemoveOrphan(block.GetHeight(), block.GetHash());
	pTxPool->ReconcileBlock(pBlockDB, pTxHashSet, block);

	// Incremental index maintenance: entries and the new state share the block's
	// BlockDB transaction, so a crash cannot publish a block with a partial index.
	if (!KernelIndex::AppendBlock(pBlockDB, pTxHashSet, pPreviousHeader, block.GetHeader())) {
		LOG_WARNING_F(
			"Kernel index could not be extended for block {} height {}; a verified rebuild is required after commit.",
			block.GetHash(),
			block.GetHeight());
		m_kernelIndexNeedsRebuild = true;
	}
}