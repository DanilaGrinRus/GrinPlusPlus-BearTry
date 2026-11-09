#include "Test_NrdSupport.h"

#include <BlockChain/Processors/TxHashSetProcessor.h>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace NrdTest;

namespace
{
	std::vector<TransactionKernel> Kernels(std::initializer_list<TransactionKernel> kernels)
	{
		return std::vector<TransactionKernel>(kernels);
	}

	TxHashSetProcessor::EFailureKind VerifyNrdPreCommit(ChainBuilder& builder)
	{
		const std::shared_ptr<const IBlockDB> pBlockDB = builder.BlockDB();
		return TxHashSetProcessor::VerifyNrdIndexBeforeCommit(
			builder.TxHashSetRef(),
			pBlockDB,
			*builder.Tip());
	}

	// Stable content hash of a txhashset directory: every file's relative path
	// plus bytes, sorted by path. Used to prove a refused staged state leaves a
	// previous working directory untouched.
	std::string HashDirectory(const fs::path& root)
	{
		std::vector<std::pair<std::string, std::string>> files;
		const std::string rootString = root.string();
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file()) {
				continue;
			}

			std::ifstream stream(entry.path(), std::ios::in | std::ios::binary);
			std::string contents(
				(std::istreambuf_iterator<char>(stream)),
				std::istreambuf_iterator<char>());
			std::string relative = entry.path().string();
			if (relative.size() > rootString.size() &&
				relative.compare(0, rootString.size(), rootString) == 0) {
				relative = relative.substr(rootString.size());
			}
			files.emplace_back(std::move(relative), std::move(contents));
		}

		std::sort(files.begin(), files.end());
		std::string combined;
		for (const auto& file : files) {
			combined += file.first;
			combined += '\0';
			combined += file.second;
			combined += '\0';
		}

		return combined;
	}
}

TEST_CASE("NrdPreCommitGateAcceptsCompleteIndexAndClassifiesRefusals")
{
	SECTION("complete valid history is accepted")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("precommit-valid"), false, false);
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(150, 2) })); // h1
		builder.AppendWithoutBody(Kernels({}));                        // h2
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(150, 2) })); // h3, diff 2

		REQUIRE(VerifyNrdPreCommit(builder) == TxHashSetProcessor::EFailureKind::None);
		REQUIRE(builder.IsIndexComplete());
		REQUIRE(builder.IndexEntries(150).size() == 2);
	}

	SECTION("historical relative-height violation is InvalidState")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("precommit-invalid"), false, false);
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(151, 3) })); // h1
		builder.AppendWithoutBody(Kernels({}));                        // h2
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(151, 3) })); // h3, diff 2 < 3

		REQUIRE(VerifyNrdPreCommit(builder) == TxHashSetProcessor::EFailureKind::InvalidState);
		REQUIRE_FALSE(builder.IsIndexComplete());
		REQUIRE(builder.IndexEntryCount() == 0);
	}

	SECTION("missing middle header is LocalUnverifiable, not peer evidence")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("precommit-missing-header"), false, false);
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(152, 2) })); // h1
		builder.AppendWithoutBody(Kernels({}));                        // h2
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(152, 2) })); // h3

		BlockHeaderPtr pMiddle;
		for (const BlockHeaderPtr& pHeader : builder.BlockDB()->GetHeaders()) {
			if (pHeader->GetHeight() == 2) {
				pMiddle = pHeader;
			}
		}
		REQUIRE(pMiddle != nullptr);
		builder.BlockDB()->RemoveBlockHeader(pMiddle->GetHash());

		REQUIRE(VerifyNrdPreCommit(builder) == TxHashSetProcessor::EFailureKind::LocalUnverifiable);
		REQUIRE_FALSE(builder.IsIndexComplete());
	}

	SECTION("kernel count mismatch is LocalUnverifiable")
	{
		ChainBuilder builder(ChainBuilder::MakeTempRoot("precommit-count-mismatch"), false, false);
		builder.AppendWithoutBody(Kernels({ MakeNrdKernel(153, 2) }));
		builder.AppendMismatchedHeader(builder.KernelCount() + 2);

		REQUIRE(VerifyNrdPreCommit(builder) == TxHashSetProcessor::EFailureKind::LocalUnverifiable);
		REQUIRE_FALSE(builder.IsIndexComplete());
	}
}

TEST_CASE("NrdPreCommitRefusalKeepsPreviousWorkingStateUntouched")
{
	// Models the ProcessTxHashSet ordering: the gate runs on the staged state
	// before the live directory is swapped, so a refusal must leave the live
	// txhashset byte-identical and still usable.
	ChainBuilder live(ChainBuilder::MakeTempRoot("precommit-live"));
	REQUIRE(live.ApplyBlock(Kernels({ MakeNrdKernel(160, 2) }))); // h1
	REQUIRE(live.ApplyBlock(Kernels({})));                        // h2
	live.Commit();
	REQUIRE(live.IsIndexComplete());

	const std::string liveBefore = HashDirectory(live.Root());
	REQUIRE_FALSE(liveBefore.empty());

	ChainBuilder staged(ChainBuilder::MakeTempRoot("precommit-staged"), false, false);
	staged.AppendWithoutBody(Kernels({ MakeNrdKernel(161, 4) })); // h1
	staged.AppendWithoutBody(Kernels({}));                        // h2
	staged.AppendWithoutBody(Kernels({ MakeNrdKernel(161, 4) })); // h3, diff 2 < 4

	REQUIRE(VerifyNrdPreCommit(staged) == TxHashSetProcessor::EFailureKind::InvalidState);
	REQUIRE_FALSE(staged.IsIndexComplete());

	// Refusing the staged state must not touch the previous working state.
	REQUIRE(HashDirectory(live.Root()) == liveBefore);
	REQUIRE(live.IsIndexComplete());
	REQUIRE(live.IndexEntries(160).size() == 1);
}
