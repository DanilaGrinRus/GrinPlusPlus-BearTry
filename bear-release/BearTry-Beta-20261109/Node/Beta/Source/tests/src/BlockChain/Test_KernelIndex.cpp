#include <catch.hpp>

#include <TestServer.h>
#include <TestMiner.h>
#include <TestChain.h>
#include <TxBuilder.h>

#include <BlockChain/BlockChain.h>
#include <API/Node/Handlers/GetKernelHandler.h>
#include <Database/BlockDb.h>
#include <PMMR/KernelIndex.h>
#include <Wallet/Keychain/KeyChain.h>

#include <json/json.h>
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

TEST_CASE("KernelIndex append and verified ancestor extension")
{
	TestServer::Ptr pServer = TestServer::Create();
	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	auto pBlockDB = pServer->GetBlockDB();
	REQUIRE(pChain->EnsureKernelIndex());
	const auto before = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(before != nullptr);
	const uint64_t generation = before->GetGeneration();
	const uint64_t beforeKernelCount = before->GetKernelCount();

	TestMiner miner(pServer);
	KeyChain keyChain = KeyChain::FromRandom();
	const auto chain = miner.MineChain(keyChain, 8);

	// Normal block processing must advance the index in the block transaction,
	// without waiting for the next startup Ensure().
	const auto afterMining = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(afterMining != nullptr);
	REQUIRE(afterMining->GetGeneration() == generation);
	REQUIRE(afterMining->GetTipHash() == chain.back().block.GetHeader()->GetHash());
	REQUIRE(afterMining->GetKernelCount() == chain.back().block.GetHeader()->GetNumKernels());
	REQUIRE(afterMining->GetEntryCount() == afterMining->GetKernelCount());
	REQUIRE(pBlockDB->Read()->GetKernelPositionDigest(generation) ==
		std::make_pair(afterMining->GetKernelCount(), afterMining->GetChecksum()));
	REQUIRE(pChain->EnsureKernelIndex());

	// Simulate a crash/startup that left the published state behind its complete
	// entries: remove the post-ancestor entries and restore the old state record.
	// Ensure() must extend the verified ancestor instead of rebuilding.
	{
		auto pWriter = pBlockDB->BatchWrite();
		const auto removed = pWriter->RemoveKernelPositionsFrom(generation, beforeKernelCount);
		REQUIRE(removed.first == afterMining->GetKernelCount() - beforeKernelCount);
		pWriter->SetKernelIndexState(*before);
		pWriter->Commit();
	}
	REQUIRE(pBlockDB->Read()->GetKernelIndexState()->GetTipHash() == before->GetTipHash());

	REQUIRE(pChain->EnsureKernelIndex());
	const auto after = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(after != nullptr);
	REQUIRE(after->GetGeneration() == generation);
	REQUIRE(after->GetTipHash() == chain.back().block.GetHeader()->GetHash());
	REQUIRE(after->GetKernelCount() == chain.back().block.GetHeader()->GetNumKernels());
	REQUIRE(pBlockDB->Read()->GetKernelPositionCount(generation) == after->GetKernelCount());
	REQUIRE(pBlockDB->Read()->GetKernelPositionChecksum(generation) == after->GetChecksum());
	for (size_t height = 1; height < chain.size(); ++height) {
		const auto location = LookupKernel(pBlockDB, generation, chain[height].block.GetKernels().front().GetExcessCommitment());
		REQUIRE(location != nullptr);
		REQUIRE(location->GetBlockHeight() == height);
	}
}

TEST_CASE("KernelIndex rebuilds rather than extending a corrupt ancestor")
{
	TestServer::Ptr pServer = TestServer::Create();
	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	auto pBlockDB = pServer->GetBlockDB();
	REQUIRE(pChain->EnsureKernelIndex());
	const auto before = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(before != nullptr);
	const uint64_t generation = before->GetGeneration();
	{
		auto writer = pBlockDB->Write();
		writer->ClearKernelPositions(generation);
	}
	TestMiner miner(pServer);
	KeyChain keyChain = KeyChain::FromRandom();
	const auto chain = miner.MineChain(keyChain, 5);
	REQUIRE(pChain->EnsureKernelIndex());
	const auto after = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(after != nullptr);
	REQUIRE(after->GetGeneration() != generation);
	REQUIRE(after->GetTipHash() == chain.back().block.GetHeader()->GetHash());
	REQUIRE(pBlockDB->Read()->GetKernelPositionChecksum(after->GetGeneration()) == after->GetChecksum());
}

TEST_CASE("KernelIndex extension batch rollback preserves the published state")
{
	TestServer::Ptr pServer = TestServer::Create();
	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	auto pBlockDB = pServer->GetBlockDB();
	REQUIRE(pChain->EnsureKernelIndex());
	const auto before = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(before != nullptr);
	const auto originalDigest = pBlockDB->Read()->GetKernelPositionDigest(before->GetGeneration());
	const Commitment additional{CBigInteger<33>::ValueOf(1)};
	const OutputLocation location(LeafIndex::At(before->GetKernelCount()), before->GetTipHeight() + 1);
	{
		auto writer = pBlockDB->BatchWrite();
		writer->AddKernelPosition(before->GetGeneration(), additional, location);
		writer->SetKernelIndexState(KernelIndexState(
			before->GetGeneration(), before->GetTipHeight() + 1, Hash::ValueOf(1), Hash::ValueOf(2),
			before->GetKernelCount() + 1, before->GetEntryCount() + 1,
			KernelIndexXorHash(before->GetChecksum(), additional, location)));
		// No commit: destructor rolls back both the entries and the state.
	}
	const auto after = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(after != nullptr);
	REQUIRE(after->GetTipHash() == before->GetTipHash());
	REQUIRE(after->GetKernelCount() == before->GetKernelCount());
	REQUIRE(pBlockDB->Read()->GetKernelPositionDigest(after->GetGeneration()) == originalDigest);
	REQUIRE(pBlockDB->Read()->GetKernelPosition(after->GetGeneration(), additional) == nullptr);
}

TEST_CASE("KernelIndex startup keeps the incrementally maintained index after restart")
{
	uint64_t generation = 0;
	Hash expectedTip;
	uint64_t expectedCount = 0;
	{
		TestServer::Ptr server = TestServer::Create();
		REQUIRE(server->GetBlockChain()->EnsureKernelIndex());
		const auto before = server->GetBlockDB()->Read()->GetKernelIndexState();
		REQUIRE(before != nullptr);
		generation = before->GetGeneration();
		TestMiner miner(server);
		KeyChain keyChain = KeyChain::FromRandom();
		const auto chain = miner.MineChain(keyChain, 8);
		expectedTip = chain.back().block.GetHeader()->GetHash();
		expectedCount = chain.back().block.GetHeader()->GetNumKernels();
		const auto afterMining = server->GetBlockDB()->Read()->GetKernelIndexState();
		REQUIRE(afterMining != nullptr);
		REQUIRE(afterMining->GetTipHash() == expectedTip);
		REQUIRE(afterMining->GetKernelCount() == expectedCount);
		server->SetPreserveDataOnDestruct(true);
	}
	TestServer::Ptr restarted = TestServer::OpenExisting();
	const auto after = restarted->GetBlockDB()->Read()->GetKernelIndexState();
	REQUIRE(after != nullptr);
	REQUIRE(after->GetGeneration() == generation);
	REQUIRE(after->GetTipHash() == expectedTip);
	REQUIRE(after->GetKernelCount() == expectedCount);
	const auto digest = restarted->GetBlockDB()->Read()->GetKernelPositionDigest(generation);
	REQUIRE(digest.first == expectedCount);
	REQUIRE(digest.second == after->GetChecksum());
}

TEST_CASE("KernelIndex rejects the old fork after a confirmed reorg")
{
	TestServer::Ptr server = TestServer::Create();
	const auto chain = server->GetBlockChain();
	const auto db = server->GetBlockDB();
	KeyChain keyChain = KeyChain::FromRandom();
	TxBuilder txBuilder(keyChain);
	TestChain builder(chain);
	const auto a = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 1 })) });
	const auto b = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 2 })) });
	const auto c = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 3 })) });
	builder.Rewind(2);
	const auto fork = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 1, 2 })) });
	REQUIRE(chain->AddBlock(a.block) == EBlockChainStatus::SUCCESS);
	REQUIRE(chain->AddBlockHeader(b.block.GetHeader()) == EBlockChainStatus::SUCCESS);
	REQUIRE(chain->AddBlockHeader(fork.block.GetHeader()) == EBlockChainStatus::SUCCESS);
	REQUIRE(chain->AddBlock(fork.block) == EBlockChainStatus::SUCCESS);
	REQUIRE(chain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() == fork.block.GetHash());
	REQUIRE(chain->EnsureKernelIndex());
	const auto before = db->Read()->GetKernelIndexState();
	REQUIRE(before != nullptr);
	const uint64_t oldGeneration = before->GetGeneration();
	const auto forkExcess = fork.block.GetKernels().front().GetExcessCommitment();
	REQUIRE(LookupKernel(db, oldGeneration, forkExcess) != nullptr);
	REQUIRE(chain->AddBlock(b.block) == EBlockChainStatus::SUCCESS);
	REQUIRE(chain->AddBlock(c.block) == EBlockChainStatus::SUCCESS);
	REQUIRE(chain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() == c.block.GetHash());
	REQUIRE(chain->EnsureKernelIndex());
	const auto after = db->Read()->GetKernelIndexState();
	REQUIRE(after != nullptr);
	REQUIRE(after->GetTipHash() == c.block.GetHash());
	// The reorg is handled incrementally: rewind to the common ancestor and re-append
	// the new branch in the same generation. A full rebuild is not required.
	REQUIRE(after->GetGeneration() == oldGeneration);
	REQUIRE(db->Read()->GetKernelPositionDigest(after->GetGeneration()).second == after->GetChecksum());
	REQUIRE(LookupKernel(db, after->GetGeneration(), forkExcess) == nullptr);
	for (const auto* block : { &a, &b, &c }) {
		const auto location = LookupKernel(db, after->GetGeneration(), block->block.GetKernels().front().GetExcessCommitment());
		REQUIRE(location != nullptr);
		REQUIRE(location->GetBlockHeight() == block->block.GetHeight());
	}
}

// Regression for the orphan path: ProcessNextOrphanBlock used to call
// BlockProcessor directly and ignore KernelIndexNeedsRebuild. If AppendBlock is
// refused (or RewindToAncestor fails), the orphan block is committed but the
// index stays stale until the next restart. The test forces a refused append by
// replacing the state with an unrelated empty generation, processes the orphan,
// and requires the verified fallback to leave the index at the new tip.
TEST_CASE("KernelIndex orphan processing repairs a refused append", "[orphan][.]")
{
	TestServer::Ptr pServer = TestServer::Create();
	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	auto pBlockDB = pServer->GetBlockDB();
	KeyChain keyChain = KeyChain::FromRandom();
	TxBuilder txBuilder(keyChain);
	TestChain builder(pChain);

	const auto block_a = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 1 })) });
	const auto block_b = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 2 })) });
	const auto block_c = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 3 })) });
	builder.Rewind(2);
	const auto block_b_fork = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 1, 2 })) });

	REQUIRE(pChain->AddBlock(block_a.block) == EBlockChainStatus::SUCCESS);
	REQUIRE(pChain->AddBlockHeader(block_b.block.GetHeader()) == EBlockChainStatus::SUCCESS);
	REQUIRE(pChain->AddBlockHeader(block_c.block.GetHeader()) == EBlockChainStatus::SUCCESS);
	REQUIRE(pChain->AddBlock(block_b_fork.block) == EBlockChainStatus::SUCCESS);
	REQUIRE(pChain->AddBlock(block_c.block) == EBlockChainStatus::ORPHANED);
	REQUIRE(pChain->AddBlock(block_b.block) == EBlockChainStatus::SUCCESS);

	GetKernelHandler handler(pChain, pServer->GetTxHashSetManagerShared(), pServer->GetDatabase());
	const Commitment excess = block_c.block.GetKernels().front().GetExcessCommitment();
	const Commitment confirmedExcess = block_b_fork.block.GetKernels().front().GetExcessCommitment();
	const auto lookupHeight = [&](const Commitment& commitment) -> uint64_t {
		Json::Value params(Json::arrayValue);
		params.append(commitment.ToHex());
		const Json::Value response = handler.Handle(
			RPC::Request::BuildRequest("get_kernel", params)).ToJSON();
		return response["result"]["Ok"]["height"].asUInt64();
	};
	REQUIRE(lookupHeight(confirmedExcess) == block_b_fork.block.GetHeight());

	// Force AppendBlock to refuse the orphan: an unrelated empty generation is
	// not the parent of the orphan block. The old code committed the block and
	// left this state in place; the fixed path must run the verified Ensure.
	const auto before = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(before != nullptr);
	{
		auto pWriter = pBlockDB->BatchWrite();
		pWriter->SetKernelIndexState(KernelIndexState(
			before->GetGeneration() + 100, 0, Hash(), Hash(), 0, 0, Hash()));
		pWriter->Commit();
	}
	REQUIRE(lookupHeight(confirmedExcess) == block_b_fork.block.GetHeight());

	REQUIRE(pChain->ProcessNextOrphanBlock());
	REQUIRE(pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() == block_c.block.GetHash());

	const auto after = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(after != nullptr);
	REQUIRE(after->GetTipHash() == block_c.block.GetHash());
	REQUIRE(after->GetTipHeight() == block_c.block.GetHeight());
	REQUIRE(after->GetKernelCount() == block_c.block.GetHeader()->GetNumKernels());
	REQUIRE(pBlockDB->Read()->GetKernelPositionDigest(after->GetGeneration()) ==
		std::make_pair(after->GetKernelCount(), after->GetChecksum()));

	// get_kernel must work both through the direct-scan fallback (before the
	// repair) and through the repaired persistent index (after it).
	REQUIRE(lookupHeight(excess) == block_c.block.GetHeight());
}


