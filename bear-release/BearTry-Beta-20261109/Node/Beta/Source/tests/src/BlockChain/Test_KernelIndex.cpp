#include <catch.hpp>

#include <TestServer.h>
#include <TestMiner.h>

#include <BlockChain/BlockChain.h>
#include <Database/BlockDb.h>
#include <PMMR/KernelIndex.h>
#include <Wallet/Keychain/KeyChain.h>

#include <set>
#include <vector>

namespace
{
	std::unique_ptr<OutputLocation> LookupKernel(
		const std::shared_ptr<Locked<IBlockDB>>& pBlockDB,
		const uint64_t generation,
		const Commitment& excess)
	{
		return pBlockDB->Read()->GetKernelPosition(generation, excess);
	}
}

TEST_CASE("KernelIndex is chain-bound and detects corrupted mappings")
{
	TestServer::Ptr pServer = TestServer::Create();
	TestMiner miner(pServer);
	KeyChain keyChain = KeyChain::FromRandom();
	const std::vector<MinedBlock> chain = miner.MineChain(keyChain, 8);
	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	auto pBlockDB = pServer->GetBlockDB();

	REQUIRE(pChain->EnsureKernelIndex());

	const BlockHeaderPtr pTip = pChain->GetTipBlockHeader(EChainType::CONFIRMED);
	REQUIRE(pTip != nullptr);
	const std::unique_ptr<KernelIndexState> pState = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(pState != nullptr);
	REQUIRE(pState->GetTipHash() == pTip->GetHash());
	REQUIRE(pState->GetTipHeight() == pTip->GetHeight());
	REQUIRE(pState->GetKernelRoot() == pTip->GetKernelRoot());
	REQUIRE(pState->GetKernelCount() == pTip->GetNumKernels());
	REQUIRE(pState->GetEntryCount() == pTip->GetNumKernels());

	const std::set<size_t> heights = { 1, 4, 7 };
	for (const size_t height : heights) {
		REQUIRE(height < chain.size());
		const TransactionKernel& kernel = chain[height].block.GetKernels().front();
		const std::unique_ptr<OutputLocation> pLocation = LookupKernel(
			pBlockDB, pState->GetGeneration(), kernel.GetExcessCommitment());
		REQUIRE(pLocation != nullptr);
		REQUIRE(pLocation->GetBlockHeight() == height);
		REQUIRE(pLocation->GetLeafIndex().Get() ==
			chain[height].block.GetHeader()->GetNumKernels() - chain[height].block.GetKernels().size());

		const std::unique_ptr<TransactionKernel> pFound =
			pChain->GetTxHashSet()->GetKernelByLeafIndex(pLocation->GetLeafIndex().Get());
		REQUIRE(pFound != nullptr);
		REQUIRE(pFound->GetExcessCommitment() == kernel.GetExcessCommitment());
	}

	// Missing excess is not mapped to an arbitrary kernel.
	const Commitment missing{CBigInteger<33>::ValueOf(1)};
	REQUIRE(LookupKernel(pBlockDB, pState->GetGeneration(), missing) == nullptr);

	// Corrupt the index with an extra primary mapping. Schema v2 keys entries by
	// position+excess, so this adds a 9th leaf instead of overwriting an existing
	// excess key; EnsureKernelIndex must detect the count/checksum mismatch and
	// rebuild a clean generation.
	const TransactionKernel& tipKernel = chain.back().block.GetKernels().front();
	{
		auto pWriter = pBlockDB->Write();
		pWriter->AddKernelPosition(
			pState->GetGeneration(),
			tipKernel.GetExcessCommitment(),
			OutputLocation(LeafIndex::At(1), 1));
	}
	REQUIRE(pBlockDB->Read()->GetKernelPositionCount(pState->GetGeneration()) ==
		pState->GetKernelCount() + 1);

	REQUIRE(pChain->EnsureKernelIndex());
	const std::unique_ptr<KernelIndexState> pRepaired = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(pRepaired != nullptr);
	REQUIRE(pRepaired->GetGeneration() != pState->GetGeneration());
	REQUIRE(pBlockDB->Read()->GetKernelPositionChecksum(pRepaired->GetGeneration()) ==
		pRepaired->GetChecksum());
	const std::unique_ptr<OutputLocation> pRepairedLocation =
		LookupKernel(pBlockDB, pRepaired->GetGeneration(), tipKernel.GetExcessCommitment());
	REQUIRE(pRepairedLocation != nullptr);
	REQUIRE(pRepairedLocation->GetBlockHeight() == chain.back().block.GetHeight());
}

TEST_CASE("KernelIndex ignores an interrupted unpublished rebuild")
{
	TestServer::Ptr pServer = TestServer::Create();
	TestMiner miner(pServer);
	KeyChain keyChain = KeyChain::FromRandom();
	const std::vector<MinedBlock> chain = miner.MineChain(keyChain, 5);
	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	auto pBlockDB = pServer->GetBlockDB();

	REQUIRE(pChain->EnsureKernelIndex());
	const std::unique_ptr<KernelIndexState> pState = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(pState != nullptr);

	// Simulate a crash after the active generation lost its entries and a new
	// generation was only partially written. The old state record still points at
	// the old generation, so no partial generation may become authoritative.
	const uint64_t interruptedGeneration = pState->GetGeneration() + 1;
	{
		auto pWriter = pBlockDB->Write();
		pWriter->ClearKernelPositions(pState->GetGeneration());
		pWriter->AddKernelPosition(
			interruptedGeneration,
			chain[1].block.GetKernels().front().GetExcessCommitment(),
			OutputLocation(LeafIndex::At(1), 1));
	}

	REQUIRE(pChain->EnsureKernelIndex());
	const std::unique_ptr<KernelIndexState> pRebuilt = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(pRebuilt != nullptr);
	// The rebuild reuses the next generation slot, but clears the partial entries
	// before writing the verified set.
	REQUIRE(pRebuilt->GetGeneration() == interruptedGeneration);
	REQUIRE(pRebuilt->GetKernelCount() == chain.back().block.GetHeader()->GetNumKernels());
	REQUIRE(pBlockDB->Read()->GetKernelPositionCount(pRebuilt->GetGeneration()) ==
		pRebuilt->GetKernelCount());
	REQUIRE(pBlockDB->Read()->GetKernelPositionChecksum(pRebuilt->GetGeneration()) ==
		pRebuilt->GetChecksum());
}

TEST_CASE("KernelIndex rebuilds when the state record is stale")
{
	TestServer::Ptr pServer = TestServer::Create();
	TestMiner miner(pServer);
	KeyChain keyChain = KeyChain::FromRandom();
	const std::vector<MinedBlock> chain = miner.MineChain(keyChain, 4);
	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	auto pBlockDB = pServer->GetBlockDB();

	REQUIRE(pChain->EnsureKernelIndex());
	const std::unique_ptr<KernelIndexState> pState = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(pState != nullptr);

	{
		auto pWriter = pBlockDB->Write();
		pWriter->SetKernelIndexState(KernelIndexState(
			pState->GetGeneration(),
			0,
			Hash(),
			Hash(),
			0,
			0,
			Hash()));
	}

	REQUIRE(pChain->EnsureKernelIndex());
	const std::unique_ptr<KernelIndexState> pRebuilt = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(pRebuilt != nullptr);
	REQUIRE(pRebuilt->GetTipHash() == pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash());
	REQUIRE(pRebuilt->GetKernelCount() == chain.back().block.GetHeader()->GetNumKernels());
}
