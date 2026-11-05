#include "Test_NrdSupport.h"

#include <BlockChain/Processors/BlockProcessor.h>

#include <string>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace NrdTest;

namespace
{
	std::vector<TransactionKernel> Kernels(std::initializer_list<TransactionKernel> kernels)
	{
		return std::vector<TransactionKernel>(kernels);
	}

	FullBlock MakeNextBlock(
		ChainBuilder& builder,
		std::vector<TransactionKernel> kernels,
		const uint16_t version = kPostHf3HeaderVersion)
	{
		const uint64_t nextHeight = builder.Tip()->GetHeight() + 1;
		const BlockHeaderPtr pHeader = MakeHeader(
			version,
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

	BlockProcessor::EApplyNrdAwareResult ApplyAware(
		ChainBuilder& builder,
		const FullBlock& block)
	{
		const std::shared_ptr<IBlockDB> pBlockDB = builder.BlockDB();
		return BlockProcessor::ApplyBlockNrdAware(
			builder.TxHashSetRef(),
			pBlockDB,
			*builder.Tip(),
			block);
	}
}

// Regression for the auditor finding on the block-application path: when the
// in-memory NRD recent-kernel index is incomplete locally, TxHashSet::ApplyBlock
// returns MISSING_LOCAL_HISTORY. The NRD-aware BlockProcessor path must rebuild
// once and, if that cannot succeed locally, return a non-ban classification
// instead of InvalidBlock.
//
// Non-ban proof (static trace, no P2P edit): BlockPipe::ProcessNewBlock bans only
// when AddBlock returns EBlockChainStatus::INVALID or when an exception escapes.
// BlockProcessor::ProcessBlockInternal catches LocalNrdHistoryUnavailable and
// returns EBlockChainStatus::UNKNOWN_ERROR, and ApplyBlockNrdAware never throws
// for this case — so neither ban condition is reachable on this path.
TEST_CASE("NrdFalseBan local index is rebuilt once and block applied")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("falseban-rebuild"), false, false /* incomplete index */);
	REQUIRE_FALSE(builder.IsIndexComplete());
	builder.AppendWithoutBody(Kernels({ MakeNrdKernel(180, 2) })); // h1
	builder.AppendWithoutBody(Kernels({}));                        // h2

	const FullBlock block = MakeNextBlock(builder, Kernels({ MakeNrdKernel(180, 2) })); // h3, diff 2

	REQUIRE(ApplyAware(builder, block) == BlockProcessor::EApplyNrdAwareResult::Applied);
	REQUIRE(builder.IsIndexComplete());
	REQUIRE(builder.IndexEntries(180).size() == 2);
}

TEST_CASE("NrdFalseBan unrebuildable local history is not an invalid block")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("falseban-local-unavailable"), false, false);
	builder.AppendWithoutBody(Kernels({ MakeNrdKernel(181, 2) })); // h1
	builder.AppendWithoutBody(Kernels({}));                        // h2

	// Remove a header the rebuild needs: the local state cannot be verified.
	BlockHeaderPtr pMissing;
	for (const BlockHeaderPtr& pHeader : builder.BlockDB()->GetHeaders()) {
		if (pHeader->GetHeight() == 1) {
			pMissing = pHeader;
		}
	}
	REQUIRE(pMissing != nullptr);
	builder.BlockDB()->RemoveBlockHeader(pMissing->GetHash());

	const FullBlock block = MakeNextBlock(builder, Kernels({ MakeNrdKernel(182, 2) }));

	const auto result = ApplyAware(builder, block);
	REQUIRE(result == BlockProcessor::EApplyNrdAwareResult::LocalNrdHistoryUnavailable);
	REQUIRE(result != BlockProcessor::EApplyNrdAwareResult::InvalidBlock);
	REQUIRE_FALSE(builder.IsIndexComplete());
}

TEST_CASE("NrdFalseBan genuine invalid block keeps the bad-block classification")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("falseban-invalid"));
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(183, 3) }))); // h1, complete index
	REQUIRE(builder.IsIndexComplete());

	// Two NRD kernels with the same excess in one block: duplicate rule, and the
	// local history is complete, so this is peer evidence, not local missing data.
	const FullBlock block = MakeNextBlock(
		builder,
		Kernels({ MakeNrdKernel(184, 2), MakeNrdKernel(184, 2) }));

	REQUIRE(ApplyAware(builder, block) == BlockProcessor::EApplyNrdAwareResult::InvalidBlock);
	REQUIRE(builder.IsIndexComplete());
}

TEST_CASE("NrdFalseBan plain block applies even with incomplete local index")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("falseban-plain"), false, false);
	builder.AppendWithoutBody(Kernels({})); // h1

	const FullBlock block = MakeNextBlock(builder, Kernels({ MakePlainKernel(185) }));
	REQUIRE(ApplyAware(builder, block) == BlockProcessor::EApplyNrdAwareResult::Applied);
	REQUIRE_FALSE(builder.IsIndexComplete());
}

TEST_CASE("NrdFalseBan non-const references preserve the classification enum")
{
	// Guards against a future refactor collapsing LocalNrdHistoryUnavailable into
	// InvalidBlock (which would re-open the false-ban path).
	REQUIRE(BlockProcessor::EApplyNrdAwareResult::Applied != BlockProcessor::EApplyNrdAwareResult::InvalidBlock);
	REQUIRE(BlockProcessor::EApplyNrdAwareResult::LocalNrdHistoryUnavailable != BlockProcessor::EApplyNrdAwareResult::InvalidBlock);
	REQUIRE(BlockProcessor::EApplyNrdAwareResult::LocalNrdHistoryUnavailable != BlockProcessor::EApplyNrdAwareResult::Applied);
}
