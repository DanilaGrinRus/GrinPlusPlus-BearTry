#include <catch.hpp>

#include <TestServer.h>
#include <TestMiner.h>
#include <TxBuilder.h>

#include "../PMMR/Test_NrdSupport.h"

#include <BlockChain/BlockChain.h>
#include <Core/Global.h>
#include <Core/Models/CompactBlock.h>
#include <Core/Models/SpentOutput.h>
#include <Core/Util/TransactionUtil.h>
#include <Database/BlockDb.h>
#include <P2P/Peer.h>
#include <P2P/Pipeline/BlockPipe.h>
#include <PMMR/Common/LeafIndex.h>
#include <Wallet/Keychain/KeyChain.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace NrdTest;

namespace
{
	// Test double for IBlockChain. BlockPipe only uses AddBlock /
	// ProcessNextOrphanBlock / HasOrphan for this path; every other method is an
	// explicit inert stub so an unexpected call is visible in behaviour (not a
	// silent network/state effect).
	class FakeBlockChain final : public IBlockChain
	{
	public:
		std::atomic<int> AddCalls{ 0 };
		std::atomic<bool> SawNrdKernel{ false };
		std::atomic<int> NextStatus{ static_cast<int>(EBlockChainStatus::SUCCESS) };
		std::atomic<bool> ThrowOnAdd{ false };
		std::string ThrowMessage{ "fault-injected local NRD rebuild read failure" };

		EBlockChainStatus AddBlock(const FullBlock& block) override
		{
			++AddCalls;
			for (const TransactionKernel& kernel : block.GetKernels()) {
				if (kernel.GetFeatures() == EKernelFeatures::NO_RECENT_DUPLICATE) {
					SawNrdKernel = true;
				}
			}

			if (ThrowOnAdd) {
				throw std::runtime_error(ThrowMessage);
			}

			return static_cast<EBlockChainStatus>(NextStatus.load());
		}

		void ResyncChain() override { }
		void UpdateSyncStatus(SyncStatus&) const override { }
		uint64_t GetHeight(const EChainType) const override { return 0; }
		uint64_t GetTotalDifficulty(const EChainType) const override { return 0; }
		EBlockChainStatus AddCompactBlock(const CompactBlock&) override { return EBlockChainStatus::UNKNOWN_ERROR; }
		fs::path SnapshotTxHashSet(BlockHeaderPtr) override { return {}; }
		EBlockChainStatus ProcessTransactionHashSet(const Hash&, const fs::path&, SyncStatus&) override { return EBlockChainStatus::UNKNOWN_ERROR; }
		EBlockChainStatus ProcessExistingTxHashSet(const Hash&, SyncStatus&) override { return EBlockChainStatus::UNKNOWN_ERROR; }
		void CloseTxHashSet() override { }
		void OpenTxHashSet() override { }
		std::shared_ptr<ITxHashSet> GetTxHashSet() override { return nullptr; }
		bool EnsureKernelIndex() override { return false; }
		ApiStateSnapshot GetApiStateSnapshot() const override { return {}; }
		bool IsApiStateCurrent(const uint64_t) const override { return false; }
		EBlockChainStatus AddTransaction(TransactionPtr, const EPoolType) override { return EBlockChainStatus::UNKNOWN_ERROR; }
		TransactionPtr GetTransactionByKernelHash(const Hash&) const override { return nullptr; }
		EBlockChainStatus AddBlockHeader(BlockHeaderPtr) override { return EBlockChainStatus::UNKNOWN_ERROR; }
		EBlockChainStatus AddBlockHeaders(const std::vector<BlockHeaderPtr>&) override { return EBlockChainStatus::UNKNOWN_ERROR; }
		BlockHeaderPtr GetBlockHeaderByHeight(const uint64_t, const EChainType) const override { return nullptr; }
		BlockHeaderPtr GetBlockHeaderByHash(const Hash&) const override { return nullptr; }
		BlockHeaderPtr GetBlockHeaderByCommitment(const Commitment&) const override { return nullptr; }
		BlockHeaderPtr GetTipBlockHeader(const EChainType) const override { return nullptr; }
		std::vector<BlockHeaderPtr> GetBlockHeadersByHash(const std::vector<Hash>&) const override { return {}; }
		std::unique_ptr<CompactBlock> GetCompactBlockByHash(const Hash&) const override { return nullptr; }
		std::unique_ptr<FullBlock> GetBlockByHeight(const uint64_t) const override { return nullptr; }
		std::unique_ptr<FullBlock> GetBlockByHash(const Hash&) const override { return nullptr; }
		std::unique_ptr<FullBlock> GetBlockByCommitment(const Commitment&) const override { return nullptr; }
		bool HasBlock(const uint64_t, const Hash&) const override { return false; }
		std::vector<BlockWithOutputs> GetOutputsByHeight(const uint64_t, const uint64_t) const override { return {}; }
		std::vector<std::pair<uint64_t, Hash>> GetBlocksNeeded(const uint64_t) const override { return {}; }
		bool ProcessNextOrphanBlock() override { return false; }
		bool HasOrphan(const Hash&) const override { return false; }
		bool AreSpentPositionsComplete(const Hash&, const std::vector<Commitment>&, size_t& mappedCount) const override
		{
			mappedCount = 0;
			return false;
		}
	};

	FullBlock MakeCraftedNrdBlock()
	{
		const BlockHeaderPtr pHeader = MakeHeader(4, 10, MakeHash(90), 1, 90);
		return FullBlock(
			pHeader,
			TransactionBody(
				std::vector<TransactionInput>{},
				std::vector<TransactionOutput>{},
				std::vector<TransactionKernel>({ MakeNrdKernel(77, 2) })));
	}

	bool WaitFor(const std::function<bool()>& predicate, const int timeoutMs = 5000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline) {
			if (predicate()) {
				return true;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		return predicate();
	}

	PeerPtr MakeLoopbackPeer()
	{
		return std::make_shared<Peer>(IPAddress::Parse("127.0.0.1"));
	}
}

// End-to-end BlockPipe classification (fault-injected, loopback peer object,
// no production P2P/BlockPipe edits):
//   LocalNrdHistoryUnavailable is mapped to EBlockChainStatus::UNKNOWN_ERROR by
//   BlockProcessor/BlockChainImpl; BlockPipe bans only INVALID.
//   This test drives the real BlockPipe worker threads with a crafted NRD block
//   and a controlled IBlockChain test double.
TEST_CASE("BlockPipe does not ban a peer for locally-unverifiable UNKNOWN_ERROR")
{
	const FullBlock block = MakeCraftedNrdBlock();
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	pBlockChain->NextStatus = static_cast<int>(EBlockChainStatus::UNKNOWN_ERROR);

	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();

	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, block));
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() == 1 && !pBlockPipe->IsProcessingBlock(block.GetHash()); }));

	REQUIRE(pBlockChain->SawNrdKernel.load());
	REQUIRE(pBlockChain->AddCalls.load() == 1);
	REQUIRE_FALSE(pPeer->IsBanned());
	REQUIRE(pPeer->GetBanReason() == EBanReason::None);
}

TEST_CASE("BlockPipe does not ban a peer when local rewind data is missing")
{
	const FullBlock block = MakeCraftedNrdBlock();
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	pBlockChain->NextStatus = static_cast<int>(EBlockChainStatus::LOCAL_REWIND_DATA_MISSING);

	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();

	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, block));
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() == 1 && !pBlockPipe->IsProcessingBlock(block.GetHash()); }));

	REQUIRE(pBlockChain->AddCalls.load() == 1);
	REQUIRE_FALSE(pPeer->IsBanned());
	REQUIRE(pPeer->GetBanReason() == EBanReason::None);
}

TEST_CASE("BlockPipe still bans a peer for an INVALID block")
{
	const FullBlock block = MakeCraftedNrdBlock();
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	pBlockChain->NextStatus = static_cast<int>(EBlockChainStatus::INVALID);

	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();

	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, block));
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() == 1 && !pBlockPipe->IsProcessingBlock(block.GetHash()); }));

	REQUIRE(pBlockChain->SawNrdKernel.load());
	REQUIRE(pPeer->IsBanned());
	REQUIRE(pPeer->GetBanReason() == EBanReason::BadBlock);
}

TEST_CASE("BlockPipe does not ban a peer when AddBlock throws")
{
	const FullBlock block = MakeCraftedNrdBlock();
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	pBlockChain->ThrowOnAdd = true;

	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();

	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, block));
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() == 1 && !pBlockPipe->IsProcessingBlock(block.GetHash()); }));

	REQUIRE(pBlockChain->SawNrdKernel.load());
	REQUIRE_FALSE(pPeer->IsBanned());
	REQUIRE(pPeer->GetBanReason() == EBanReason::None);
}
TEST_CASE("BlockPipe does not ban a peer when a local commit error escapes AddBlock")
{
	const FullBlock block = MakeCraftedNrdBlock();
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	pBlockChain->ThrowOnAdd = true;
	pBlockChain->ThrowMessage = "injected RocksDB index commit failure";

	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();

	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, block));
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() == 1 && !pBlockPipe->IsProcessingBlock(block.GetHash()); }));

	REQUIRE(pBlockChain->AddCalls.load() == 1);
	REQUIRE_FALSE(pPeer->IsBanned());
	REQUIRE(pPeer->GetBanReason() == EBanReason::None);
}

TEST_CASE("BlockPipe refuses missing spent position as local rewind data missing")
{
	Hash tip30aHash;
	FullBlock block30b;
	const auto keyChain = KeyChain::FromRandom();
	{
		TestServer::Ptr pServer = TestServer::Create();
		pServer->SetPreserveDataOnDestruct(true);
		TestMiner miner(pServer);
		TxBuilder txBuilder(keyChain);
		IBlockChain::Ptr pChain = pServer->GetBlockChain();

		const std::vector<MinedBlock> minedChain = miner.MineChain(keyChain, 30);
		REQUIRE(minedChain.size() == 30);

		Test::Input input({
			{ minedChain[1].block.GetOutputs().front().GetFeatures(),
			  minedChain[1].block.GetOutputs().front().GetCommitment() },
			minedChain[1].coinbasePath.value(),
			minedChain[1].coinbaseAmount
		});
		TxBuilder::Criteria spendCriteria;
		spendCriteria.inputs = { input };
		spendCriteria.outputs = {
			Test::Output{ KeyChainPath({ 1, 0 }), 10'000'000 },
			Test::Output{ KeyChainPath({ 1, 1 }), minedChain[1].coinbaseAmount - 10'000'000 }
		};
		spendCriteria.include_change = false;
		spendCriteria.fee_base = 0;
		spendCriteria.num_kernels = 1;
		const Transaction spend = txBuilder.BuildTx(spendCriteria);

		Test::Tx coinbase30a = txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 30 }));
		TransactionPtr combined30a = TransactionUtil::Aggregate({
			coinbase30a.pTransaction, std::make_shared<Transaction>(spend)
		});
		FullBlock block30a = miner.MineNextBlock(minedChain.back().block.GetHeader(), *combined30a);
		REQUIRE(block30a.GetInputs().size() == 1);
		REQUIRE(pChain->AddBlock(block30a) == EBlockChainStatus::SUCCESS);
		tip30aHash = block30a.GetHash();

		// A conflicting height-30 branch is valid except for the local spent row.
		Test::Tx coinbase30b = txBuilder.BuildCoinbaseTx(KeyChainPath({ 1, 30 }));
		block30b = miner.MineNextBlock(minedChain.back().block.GetHeader(), *coinbase30b.pTransaction);

		// The confirmed block has an input; inject the spent-position record so
		// the test controls exactly one missing entry, then remove it.
		const std::vector<Commitment> blockInputs({ input.input.GetCommitment() });
		{
			auto pWriter = pServer->GetBlockDB()->Write();
			pWriter->AddSpentPositions(
				tip30aHash,
				std::vector<SpentOutput>({
					SpentOutput(input.input.GetCommitment(), OutputLocation(LeafIndex::At(0), 1))
				}));
			pWriter->Commit();
		}
		size_t mappedCount = 0;
		REQUIRE(pChain->AreSpentPositionsComplete(tip30aHash, blockInputs, mappedCount));
		REQUIRE(mappedCount == 1);
		{
			auto pWriter = pServer->GetBlockDB()->Write();
			pWriter->ClearSpentPositions();
			pWriter->Commit();
		}
		mappedCount = 0;
		REQUIRE_THROWS(pChain->AreSpentPositionsComplete(tip30aHash, blockInputs, mappedCount));

		const PeerPtr pPeer = MakeLoopbackPeer();
		{
			auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pChain);
			REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, block30b));
			REQUIRE(WaitFor([&] {
				return !pBlockPipe->IsProcessingBlock(block30b.GetHash());
			}, 10000));
		}

		REQUIRE_FALSE(pPeer->IsBanned());
		REQUIRE(pPeer->GetBanReason() == EBanReason::None);
		std::cout << "post_refusal_confirmed_height=" << pChain->GetHeight(EChainType::CONFIRMED)
			<< " post_refusal_candidate_height=" << pChain->GetHeight(EChainType::CANDIDATE)
			<< " post_refusal_header_mmr_leaves=" << pServer->GetHeaderMMR()->Read()->GetLeafCount()
			<< std::endl;
		REQUIRE(pChain->GetHeight(EChainType::CONFIRMED) == 30);
		REQUIRE(pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() == tip30aHash);
	}

	// Restart from the unchanged, body-present but spent-position-missing state.
	{
		TestServer::Ptr pServer = TestServer::OpenExisting();
		IBlockChain::Ptr pChain = pServer->GetBlockChain();
		REQUIRE(pChain->GetHeight(EChainType::CONFIRMED) == 30);
		REQUIRE(pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() == tip30aHash);
		REQUIRE(pServer->GetBlockDB()->Read()->GetBlock(tip30aHash) != nullptr);
	}
}
