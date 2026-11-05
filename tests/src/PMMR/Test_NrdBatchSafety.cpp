#include "Test_NrdSupport.h"

#include <BlockChain/Processors/BlockProcessor.h>

#include <initializer_list>
#include <stdexcept>
#include <vector>

using namespace NrdTest;

namespace
{
	std::vector<TransactionKernel> Kernels(std::initializer_list<TransactionKernel> kernels)
	{
		return std::vector<TransactionKernel>(kernels);
	}

	FullBlock MakeNextBlock(ChainBuilder& builder, std::vector<TransactionKernel> kernels)
	{
		const uint64_t nextHeight = builder.Tip()->GetHeight() + 1;
		const BlockHeaderPtr pHeader = MakeHeader(
			kPostHf3HeaderVersion,
			nextHeight,
			builder.Tip()->GetHash(),
			builder.KernelCount() + kernels.size(),
			nextHeight);

		return FullBlock(
			pHeader,
			TransactionBody(
				std::vector<TransactionInput>{},
				std::vector<TransactionOutput>{},
				std::move(kernels)));
	}

	BlockProcessor::EApplyNrdAwareResult ApplyAware(ChainBuilder& builder, const FullBlock& block)
	{
		const std::shared_ptr<IBlockDB> pBlockDB = builder.BlockDB();
		return BlockProcessor::ApplyBlockNrdAware(
			builder.TxHashSetRef(),
			pBlockDB,
			*builder.Tip(),
			block);
	}
}

// Finding 1: an NRD rebuild on an uncommitted alternative reorg branch must not
// overwrite the last-commit backup. If the reorg then fails, Rollback must
// restore branch A's tip, kernel MMR and index, and decisions must follow branch A.
TEST_CASE("NrdReorgBackup restores last committed index after failed alternative branch")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("reorg-baseline"), false, true);

	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(190, 2) }))); // branch A, h1
	builder.Commit();
	const BlockHeaderPtr pTipA = builder.Tip();
	const Hash tipAHash = pTipA->GetHash();
	const uint64_t kernelCountA = builder.KernelCount();
	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.IndexEntries(190).size() == 1);

	// Alternative branch B, processed but not committed: rewind to genesis, then
	// append a different kernel and a different header at height 1.
	builder.RewindTo(builder.GenesisHeader());
	REQUIRE(builder.KernelCount() == kernelCountA - 1);
	builder.AppendWithoutBody(
		Kernels({ MakeNrdKernel(191, 2) }),
		kPostHf3HeaderVersion,
		false,
		1'000'000 /* distinct header hash */);
	REQUIRE(builder.Tip()->GetHash() != tipAHash);

	builder.RebuildPreservingBaseline();
	REQUIRE(builder.IsIndexComplete());
	// Intermediate branch B is visible now, but it must not be the backup.
	REQUIRE(builder.IndexEntries(191).size() == 1);

	// The next reorg step fails -> ChainState batch Rollback.
	builder.Rollback();
	builder.SetTip(pTipA);

	REQUIRE(builder.TxHashSetRef().GetFlushedBlockHeader()->GetHash() == tipAHash);
	REQUIRE(builder.KernelCount() == kernelCountA);
	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.IndexEntries(190).size() == 1);
	REQUIRE(builder.IndexEntries(191).empty());

	// Decisions must be branch-A decisions: 190 already exists at height 1.
	REQUIRE_FALSE(builder.ApplyBlock(Kernels({ MakeNrdKernel(190, 2) })));
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::INVALID_RELATIVE_HEIGHT);

	// 191 has no history on branch A: first occurrence is accepted.
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(191, 2) })));
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::ACCEPTED);
}

// Finding 2: a local exception during rebuild (header/kernel read failure) must
// not escape. ApplyBlockNrdAware classifies it as LocalNrdHistoryUnavailable, and
// ProcessBlockInternal converts that to UNKNOWN_ERROR — BlockPipe bans only on
// INVALID or an escaped exception, so no peer ban. Genuine invalid blocks keep
// InvalidBlock (proved in Test_NrdFalseBan.cpp).
TEST_CASE("NrdRebuildException is local-unavailable and never escapes")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("rebuild-throw"), false, false);
	builder.AppendWithoutBody(Kernels({})); // h1 header only
	REQUIRE_FALSE(builder.IsIndexComplete());

	const uint64_t kernelsBefore = builder.KernelCount();
	builder.BlockDB()->SetThrowOnGetBlockHeader(true);

	const FullBlock block = MakeNextBlock(builder, Kernels({ MakeNrdKernel(192, 2) }));
	BlockProcessor::EApplyNrdAwareResult result = BlockProcessor::EApplyNrdAwareResult::Applied;
	REQUIRE_NOTHROW(result = ApplyAware(builder, block));
	REQUIRE(result == BlockProcessor::EApplyNrdAwareResult::LocalNrdHistoryUnavailable);
	REQUIRE(result != BlockProcessor::EApplyNrdAwareResult::InvalidBlock);
	REQUIRE_FALSE(builder.IsIndexComplete());
	REQUIRE(builder.TxHashSetRef().DidLastNrdRebuildThrow());
	REQUIRE(builder.KernelCount() == kernelsBefore); // no partial apply

	// Recovery: once the local read works again the same path rebuilds and applies.
	builder.BlockDB()->SetThrowOnGetBlockHeader(false);
	REQUIRE(ApplyAware(builder, block) == BlockProcessor::EApplyNrdAwareResult::Applied);
	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.IndexEntries(192).size() == 1);
}
