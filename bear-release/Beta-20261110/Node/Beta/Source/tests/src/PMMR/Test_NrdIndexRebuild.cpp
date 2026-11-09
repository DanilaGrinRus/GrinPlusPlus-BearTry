#include "Test_NrdSupport.h"

#include <algorithm>

using namespace NrdTest;

namespace
{
	std::vector<TransactionKernel> Kernels(std::initializer_list<TransactionKernel> kernels)
	{
		return std::vector<TransactionKernel>(kernels);
	}

	BlockHeaderPtr FindHeader(const std::shared_ptr<FakeBlockDB>& pDB, const uint64_t height)
	{
		for (const BlockHeaderPtr& pHeader : pDB->GetHeaders()) {
			if (pHeader->GetHeight() == height) {
				return pHeader;
			}
		}

		return nullptr;
	}

	void ExpectEntry(
		const ChainBuilder& builder,
		const uint8_t excessTag,
		const uint64_t expectedPosition,
		const uint64_t expectedHeight)
	{
		const auto entries = builder.IndexEntries(excessTag);
		REQUIRE(entries.size() == 1);
		REQUIRE(entries[0].MmrPosition == expectedPosition);
		REQUIRE(entries[0].Height == expectedHeight);
	}
}

TEST_CASE("NrdRebuildPibdWithoutBodiesPublishesKernelMmrPlusHeaders")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("pibd-no-bodies"));

	// Block 1: no kernels. Block 2: NRD kernel rel=4. Block 3: plain kernel.
	builder.AppendWithoutBody(Kernels({}));
	builder.AppendWithoutBody(Kernels({ MakeNrdKernel(10, 4) }));
	builder.AppendWithoutBody(Kernels({ MakePlainKernel(44) }));
	REQUIRE(builder.BlockDB()->GetBlockCount() == 0);
	REQUIRE(builder.TipHeight() == 3);

	builder.Rebuild();

	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.IndexEntryCount() == 1);
	// genesis kernel is leaf 0, NRD kernel is leaf 1 -> node position 1.
	ExpectEntry(builder, 10, 1, 2);

	// A rebuilt index must be usable for applying new blocks at once.
	// relative_height=4: height 4 (diff 2) and height 5 (diff 3) are invalid,
	// height 6 (diff 4) is exactly on the accepted boundary.
	REQUIRE_FALSE(builder.ApplyBlock(Kernels({ MakeNrdKernel(10, 4) })));
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::INVALID_RELATIVE_HEIGHT);
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(12, 4) }))); // h4
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::ACCEPTED);
	REQUIRE_FALSE(builder.ApplyBlock(Kernels({ MakeNrdKernel(10, 4) }))); // h5
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::INVALID_RELATIVE_HEIGHT);
	REQUIRE(builder.ApplyBlock(Kernels({}))); // h5
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(10, 4) }))); // h6, diff 4
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::ACCEPTED);
}

TEST_CASE("NrdRebuildHonoursTwoWeekWindowCutoff")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("rebuild-cutoff"));

	// tip height = 2*WEEK_HEIGHT + 2, so cutoff = 2. The kernel in block 1 is
	// outside the window; the kernel in block 2 is on the cutoff and stays.
	builder.AppendWithoutBody(Kernels({ MakeNrdKernel(1, 2) }));
	builder.AppendWithoutBody(Kernels({ MakeNrdKernel(2, 2) }));
	for (uint64_t height = 3; height <= 2 * Consensus::WEEK_HEIGHT + 2; ++height) {
		builder.AppendWithoutBody(Kernels({}));
	}

	REQUIRE(builder.TipHeight() == 2 * Consensus::WEEK_HEIGHT + 2);
	REQUIRE(builder.BlockDB()->GetBlockCount() == 0);
	builder.Rebuild();

	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.IndexEntryCount() == 1);
	REQUIRE(builder.IndexEntries(1).empty());
	ExpectEntry(builder, 2, LeafIndex::At(2).GetPosition(), 2);
}

TEST_CASE("NrdRebuildRejectsHistoricalRelativeHeightViolation")
{
	SECTION("violation across blocks")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("rebuild-history-violation"));
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(20, 3) })); // h1
		builder.AppendWithoutBody(Kernels({}));                      // h2
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(20, 3) })); // h3, diff 2 < 3

		builder.Rebuild();

		REQUIRE_FALSE(builder.IsIndexComplete());
		REQUIRE(builder.IndexEntryCount() == 0);
		// Fail closed: a later NRD block must not be accepted from a rebuild
		// that Rust 5.5.2 verify_kernel_pos_index would have rejected.
		REQUIRE_FALSE(builder.ApplyBlock(Kernels({ MakeNrdKernel(21, 2) })));
		REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::MISSING_LOCAL_HISTORY);
	}

	SECTION("duplicate excess inside one historical block")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("rebuild-history-block-duplicate"));
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(22, 2), MakeNrdKernel(22, 2) }));

		builder.Rebuild();

		REQUIRE_FALSE(builder.IsIndexComplete());
		REQUIRE(builder.IndexEntryCount() == 0);
	}

	SECTION("valid history at the exact boundary publishes")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("rebuild-history-boundary"));
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(23, 3) })); // h1
		builder.AppendWithoutBody(Kernels({}));
		builder.AppendWithoutBody(Kernels({}));
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(23, 3) })); // h4, diff 3

		builder.Rebuild();

		REQUIRE(builder.IsIndexComplete());
		REQUIRE(builder.IndexEntryCount() == 2);
	}
}

TEST_CASE("NrdRebuildNeverPublishesPartialIndex")
{
	SECTION("missing header")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("partial-missing-header"));
		builder.AppendWithoutBody(Kernels({}));                      // h1
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(30, 2) })); // h2
		builder.AppendWithoutBody(Kernels({}));                      // h3
		builder.AppendWithoutBody(Kernels({}));                      // h4

		const BlockHeaderPtr pMissing = FindHeader(builder.BlockDB(), 2);
		REQUIRE(pMissing != nullptr);
		builder.BlockDB()->RemoveBlockHeader(pMissing->GetHash());

		builder.Rebuild();

		REQUIRE_FALSE(builder.IsIndexComplete());
		REQUIRE(builder.IndexEntryCount() == 0);
		REQUIRE_FALSE(builder.ApplyBlock(Kernels({ MakeNrdKernel(31, 2) })));
		REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::MISSING_LOCAL_HISTORY);
	}

	SECTION("kernel MMR shorter than the claimed tip header")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("partial-short-kernel-mmr"));
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(32, 2) }));
		// Claim two more kernels than the kernel MMR contains.
		builder.AppendMismatchedHeader(builder.KernelCount() + 2);

		builder.Rebuild();

		REQUIRE_FALSE(builder.IsIndexComplete());
		REQUIRE(builder.IndexEntryCount() == 0);
	}

	SECTION("a complete rebuild is replaced, never merged with an old tip")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("partial-old-tip"));
		BlockHeaderPtr pOlderTip;
		for (uint8_t tag = 40; tag <= 45; ++tag) {
			REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(tag, 2) })));
			if (tag == 41) {
				pOlderTip = builder.Tip();
			}
		}
		builder.Commit();

		builder.Rebuild();
		REQUIRE(builder.IsIndexComplete());
		REQUIRE(builder.IndexEntryCount() == 6);

		// Rewind the real MMR + header chain to the older tip, then rebuild. The
		// rebuild starts by clearing the index, so entries of the newer tip must
		// be gone and never merged in.
		builder.RewindTo(pOlderTip);
		builder.Rebuild();

		REQUIRE(builder.IsIndexComplete());
		REQUIRE(builder.IndexEntryCount() == 2);
		REQUIRE(builder.IndexEntries(40).size() == 1);
		REQUIRE(builder.IndexEntries(41).size() == 1);
		REQUIRE(builder.IndexEntries(42).empty());
		REQUIRE(builder.IndexEntries(45).empty());
	}
}

TEST_CASE("NrdIndexMissingIsNotAcceptedAndRebuildFixesIt")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("missing-index"), false, false);
	REQUIRE_FALSE(builder.IsIndexComplete());

	REQUIRE_FALSE(builder.ApplyBlock(Kernels({ MakeNrdKernel(50, 2) })));
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::MISSING_LOCAL_HISTORY);

	builder.Rebuild();
	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(50, 2) })));
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::ACCEPTED);
}

TEST_CASE("NrdRollbackRestoresIndexSoReappliedBlockIsAccepted")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("rollback-index"));

	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(60, 2) }))); // h1
	builder.Commit();
	const BlockHeaderPtr pCommittedTip = builder.Tip();

	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(61, 2) }))); // h2
	REQUIRE(builder.IndexEntries(61).size() == 1);

	builder.Rollback();
	builder.SetTip(pCommittedTip);

	// The rolled-back block must not leave an NRD entry behind: applying the
	// same block again has to be accepted, not rejected as a duplicate.
	REQUIRE(builder.IndexEntries(61).empty());
	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(61, 2) })));
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::ACCEPTED);
}

TEST_CASE("NrdRewindRemovesEntriesOfRewoundBlocks")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("rewind-index"));

	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(70, 2) }))); // h1
	const BlockHeaderPtr pRewindTarget = builder.Tip();
	REQUIRE(builder.ApplyBlock(Kernels({})));                      // h2
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(71, 2) }))); // h3
	builder.Commit();
	REQUIRE(builder.IndexEntries(71).size() == 1);

	builder.RewindTo(pRewindTarget);

	REQUIRE(builder.IndexEntries(71).empty());
	REQUIRE(builder.IndexEntries(70).size() == 1);
	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.ApplyBlock(Kernels({}))); // h2
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(72, 2) }))); // h3 alternative
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::ACCEPTED);
}

TEST_CASE("NrdMultiKernelBlocksKeepEveryPosition")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("multi-kernel"));

	// h1: plain kernel (leaf 1, pos 1) + NRD kernel (leaf 2, pos 3).
	builder.AppendWithoutBody(Kernels({ MakePlainKernel(80), MakeNrdKernel(81, 2) }));
	// h2: NRD kernel (leaf 3, pos 4) + NRD kernel (leaf 4, pos 7).
	builder.AppendWithoutBody(Kernels({ MakeNrdKernel(82, 2), MakeNrdKernel(83, 5) }));

	builder.Rebuild();

	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.IndexEntryCount() == 3);
	ExpectEntry(builder, 81, 3, 1);
	ExpectEntry(builder, 82, 4, 2);
	ExpectEntry(builder, 83, 7, 2);
	REQUIRE(builder.IndexEntries(80).empty()); // plain kernels are never indexed

	// A plain kernel excess does not satisfy (or block) the NRD rule: a later
	// NRD kernel with the same excess is indexed and accepted.
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(80, 1) }))); // h3
	REQUIRE(builder.LastNrdResult() == TxHashSet::ENrdValidationResult::ACCEPTED);
	REQUIRE(builder.IndexEntries(80).size() == 1);
}

TEST_CASE("NrdIndexSurvivesRestartFromKernelMmrAndHeaders")
{
	const fs::path root = ChainBuilder::MakeTempRoot("restart");
	std::vector<TxHashSet::RecentKernelPosition> beforeRestart;
	std::vector<BlockHeaderPtr> headers;

	{
		ChainBuilder builder(root);
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(90, 2) }));
		builder.AppendWithoutBody(Kernels({ MakePlainKernel(91) }));
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(92, 2) }));
		builder.Rebuild();
		REQUIRE(builder.IsIndexComplete());
		builder.Commit(); // flush the kernel MMR before reopening (restart)

		beforeRestart = builder.IndexEntries(92);
		REQUIRE(beforeRestart.size() == 1);
		headers = builder.BlockDB()->GetHeaders();
		builder.SetCleanup(false); // keep the txhashset dir for the restart
	}

	ChainBuilder restarted(root, true /*reuseExisting*/, false /*initializeNrdIndex*/);
	for (const BlockHeaderPtr& pHeader : headers) {
		restarted.AddHeader(pHeader);
	}
	const BlockHeaderPtr pTip = FindHeader(restarted.BlockDB(), 3);
	REQUIRE(pTip != nullptr);
	restarted.SetTip(pTip);
	REQUIRE_FALSE(restarted.IsIndexComplete());

	restarted.Rebuild();

	REQUIRE(restarted.IsIndexComplete());
	const auto afterRestart = restarted.IndexEntries(92);
	REQUIRE(afterRestart.size() == beforeRestart.size());
	REQUIRE(afterRestart[0].MmrPosition == beforeRestart[0].MmrPosition);
	REQUIRE(afterRestart[0].Height == beforeRestart[0].Height);
	REQUIRE(restarted.IndexEntryCount() == 2);
}
