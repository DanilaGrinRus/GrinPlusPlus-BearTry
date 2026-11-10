#include <catch.hpp>

#include <TestServer.h>
#include <TestMiner.h>
#include <TxBuilder.h>

#include "../PMMR/Test_NrdSupport.h"

#include <BlockChain/BlockChain.h>
#include <BlockChain/ICoinView.h>
#include <Core/Global.h>
#include <Core/Models/BlockHeader.h>
#include <Core/Models/CompactBlock.h>
#include <Core/Models/FullBlock.h>
#include <Core/Models/SpentOutput.h>
#include <Core/Models/TransactionBody.h>
#include <Core/Models/TransactionInput.h>
#include <Core/Models/TransactionOutput.h>
#include <Core/Util/TransactionUtil.h>
#include <Crypto/Models/RangeProof.h>
#include <Database/BlockDb.h>
#include <P2P/Messages/BlockMessage.h>
#include <P2P/Connection.h>
#include <P2P/ConnectionManager.h>
#include <P2P/MessageProcessor.h>
#include <P2P/Pipeline/Pipeline.h>
#include <P2P/Seed/PeerManager.h>
#include <P2P/SyncStatus.h>
#include <P2P/Peer.h>
#include <P2P/Pipeline/BlockPipe.h>
#include <PMMR/Common/LeafIndex.h>
#include <Wallet/Keychain/KeyChain.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace NrdTest;

namespace
{
	class FakeCoinView final : public ICoinView
	{
	public:
		std::set<Commitment> Outputs;

		EOutputFeatures GetOutputType(const Commitment& commitment) const override
		{
			if (Outputs.find(commitment) == Outputs.end()) {
				throw std::runtime_error("GetOutputType: output not found");
			}
			return EOutputFeatures::DEFAULT;
		}
	};

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
		std::atomic<bool> SlowFirstBlock{ false };
		std::atomic<uint64_t> TipHeight{ 0 };
		std::atomic<uint64_t> CandidateHeight{ 0 };
		std::atomic<uint64_t> ConfirmedDifficulty{ 0 };
		bool ReorgAware{ false };
		std::mutex TipMutex;
		BlockHeaderPtr TipHeader;
		std::shared_ptr<FakeCoinView> CoinView;
		bool AddCoinViewCommitment{ false };
		Commitment CoinViewCommitment;
		std::mutex OrderMutex;
		std::vector<uint64_t> Order;
		std::string ThrowMessage{ "fault-injected local NRD rebuild read failure" };

		EBlockChainStatus AddBlock(const FullBlock& block) override
		{
			++AddCalls;
			if (SlowFirstBlock.load() && block.GetHeight() == 1) {
				std::this_thread::sleep_for(std::chrono::milliseconds(80));
			}
			if (block.GetHeight() == 1 && CoinView != nullptr) {
				if (AddCoinViewCommitment) {
					CoinView->Outputs.insert(CoinViewCommitment);
				}
				Global::SetCoinView(CoinView);
			}
			{
				std::lock_guard<std::mutex> lock(OrderMutex);
				Order.push_back(block.GetHeight());
			}
			if (ReorgAware) {
				if (block.GetHeight() != TipHeight.load() + 1 &&
					!(block.GetHeight() == TipHeight.load() &&
						block.GetTotalDifficulty() > ConfirmedDifficulty.load())) {
					return EBlockChainStatus::ORPHANED;
				}
			}
			{
				std::lock_guard<std::mutex> lock(TipMutex);
				TipHeader = block.GetHeader();
				TipHeight = block.GetHeight();
				ConfirmedDifficulty = block.GetTotalDifficulty();
				if (CandidateHeight.load() < block.GetHeight()) {
					CandidateHeight = block.GetHeight();
				}
			}
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
		uint64_t GetHeight(const EChainType chainType) const override { return chainType == EChainType::CANDIDATE ? CandidateHeight.load() : TipHeight.load(); }
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
		BlockHeaderPtr GetTipBlockHeader(const EChainType) const override { std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(TipMutex)); return TipHeader; }
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

	BlockHeaderPtr MakeHeaderWithTD(const uint16_t version, const uint64_t height, const Hash& previousHash, const uint64_t powTag, const uint64_t totalDifficulty)
{
	ProofOfWork proofOfWork(
		Consensus::DEFAULT_MIN_EDGE_BITS,
		std::vector<uint64_t>{ powTag + 1 },
		MakeHeaderHash(powTag));
	return std::make_shared<BlockHeader>(
		version,
		height,
		1'600'000'000 + static_cast<int64_t>(height),
		Hash(previousHash),
		Hash(HASH::ZERO),
		Hash(HASH::ZERO),
		Hash(HASH::ZERO),
		Hash(HASH::ZERO),
		BlindingFactor(),
		MmrSizeForLeaves(0),
		MmrSizeForLeaves(0),
		totalDifficulty,
		0,
		powTag,
		std::move(proofOfWork));
}

FullBlock MakeBlockWithTD(const uint64_t height, const Hash& previousHash, const uint64_t totalDifficulty, const uint64_t powTag)
{
	return FullBlock(
		MakeHeaderWithTD(4, height, previousHash, powTag, totalDifficulty),
		TransactionBody(std::vector<TransactionInput>{}, std::vector<TransactionOutput>{}, std::vector<TransactionKernel>{}));
}
FullBlock MakeCraftedNrdBlockWithPrev(const uint64_t height, const Hash& previousHash, const uint64_t powTag = 0)
{
	const BlockHeaderPtr pHeader = MakeHeader(4, height, previousHash, 1, powTag == 0 ? height + 90 : powTag);
	return FullBlock(
		pHeader,
		TransactionBody(
			std::vector<TransactionInput>{},
			std::vector<TransactionOutput>{},
			std::vector<TransactionKernel>({ MakeNrdKernel(static_cast<uint8_t>(height + 70), 2) })));
}

FullBlock MakeV3Parent(const uint64_t height, const Hash& previousHash, const Commitment& commitment)
{
	const BlockHeaderPtr pHeader = MakeHeader(4, height, previousHash, 0, height + 90, 1);
	return FullBlock(
		pHeader,
		TransactionBody(
			std::vector<TransactionInput>{},
			std::vector<TransactionOutput>({ TransactionOutput(EOutputFeatures::DEFAULT, commitment, RangeProof(std::vector<unsigned char>{})) }),
			std::vector<TransactionKernel>{}));
}

FullBlock MakeV3Child(const uint64_t height, const Hash& previousHash, const Commitment& commitment)
{
	const BlockHeaderPtr pHeader = MakeHeader(4, height, previousHash, 0, height + 90, 0);
	return FullBlock(
		pHeader,
		TransactionBody(
			std::vector<TransactionInput>({ TransactionInput(EOutputFeatures::DEFAULT, Commitment(commitment)) }),
			std::vector<TransactionOutput>{},
			std::vector<TransactionKernel>{}));
}
ByteBuffer SerializeBlockBody(const FullBlock& block)
{
	Serializer serializer(EProtocolVersion::V3);
	block.Serialize(serializer);
	return ByteBuffer(serializer.GetBytes(), EProtocolVersion::V3);
}
FullBlock MakeCraftedNrdBlockAt(const uint64_t height)
{
	const BlockHeaderPtr pHeader = MakeHeader(4, height, MakeHash(static_cast<uint8_t>(height + 40)), 1, height + 90);
	return FullBlock(
		pHeader,
		TransactionBody(
			std::vector<TransactionInput>{},
			std::vector<TransactionOutput>{},
			std::vector<TransactionKernel>({ MakeNrdKernel(static_cast<uint8_t>(height + 70), 2) })));
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

TEST_CASE("BlockPipe processes a batch in receive order")
{
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	pBlockChain->SlowFirstBlock = true;

	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();
	const FullBlock parent = MakeCraftedNrdBlockAt(1);
	const FullBlock child = MakeCraftedNrdBlockAt(2);

	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, parent));
	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, child));
	REQUIRE(WaitFor([&] {
		return pBlockChain->AddCalls.load() == 2 &&
			!pBlockPipe->IsProcessingBlock(parent.GetHash()) &&
			!pBlockPipe->IsProcessingBlock(child.GetHash());
	}));

	std::lock_guard<std::mutex> lock(pBlockChain->OrderMutex);
	REQUIRE(pBlockChain->Order == std::vector<uint64_t>{ 1, 2 });
}

TEST_CASE("BlockPipe defers a serialized child until its parent is committed")
{
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();
	const FullBlock parent = MakeCraftedNrdBlockWithPrev(1, MakeHash(40));
	ByteBuffer parentBodyBuffer = SerializeBlockBody(parent);
	const BlockMessage decodedParent = BlockMessage::Deserialize(parentBodyBuffer);
	const FullBlock child = MakeCraftedNrdBlockWithPrev(2, decodedParent.GetBlock().GetHash());

	// Child arrives first; raw bytes must be held until height 1 is committed.
	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(child)));
	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(parent)));
	REQUIRE(WaitFor([&] {
		return pBlockChain->AddCalls.load() == 2 &&
			!pBlockPipe->IsProcessingBlock(parent.GetHash()) &&
			!pBlockPipe->IsProcessingBlock(child.GetHash());
	}));

	std::lock_guard<std::mutex> lock(pBlockChain->OrderMutex);
	REQUIRE(pBlockChain->Order == std::vector<uint64_t>{ 1, 2 });
}

TEST_CASE("V3 serialized child fails until parent output is in CoinView; raw pipe defers it")
{
	const Commitment commitment(CBigInteger<33>::ValueOf(7));
	const FullBlock parent = MakeV3Parent(1, MakeHash(40), commitment);
	ByteBuffer parentBodyBuffer = SerializeBlockBody(parent);
	const BlockMessage decodedParent = BlockMessage::Deserialize(parentBodyBuffer);
	const FullBlock child = MakeV3Child(2, decodedParent.GetBlock().GetHash(), commitment);
	const ByteBuffer childBody = SerializeBlockBody(child);
	const ICoinView::CPtr previousCoinView = Global::GetCoinView();
	auto pCoinView = std::make_shared<FakeCoinView>();

	Global::SetCoinView(pCoinView);
	ByteBuffer badBuffer(childBody);
	REQUIRE_THROWS(BlockMessage::Deserialize(badBuffer));

	pCoinView->Outputs.insert(commitment);
	Global::SetCoinView(pCoinView);
	ByteBuffer goodBuffer(childBody);
	const BlockMessage decodedChild = BlockMessage::Deserialize(goodBuffer);
	REQUIRE(decodedChild.GetBlock().GetHeight() == 2);

	// Start the pipe with an EMPTY CoinView: the child is deferred, the parent
	// is applied and adds its output, then the child is deserialized/applied.
	pCoinView->Outputs.clear();
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	pBlockChain->CoinView = pCoinView;
	pBlockChain->AddCoinViewCommitment = true;
	pBlockChain->CoinViewCommitment = commitment;
	// CANDIDATE headers are far ahead; bodies must use CONFIRMED+1.
	pBlockChain->CandidateHeight = 100;
	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();

	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(child)));
	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(parent)));
	REQUIRE(WaitFor([&] {
		return pBlockChain->AddCalls.load() == 2 &&
			!pBlockPipe->IsProcessingBlock(parent.GetHash()) &&
			!pBlockPipe->IsProcessingBlock(child.GetHash());
	}));

	std::lock_guard<std::mutex> lock(pBlockChain->OrderMutex);
	REQUIRE(pBlockChain->Order == std::vector<uint64_t>{ 1, 2 });
	Global::SetCoinView(previousCoinView);
}

TEST_CASE("BlockPipe bounded deferred storage and shutdown cleanup")
{
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	const PeerPtr pPeer = MakeLoopbackPeer();
	{
		auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
		size_t accepted = 0;
		for (uint64_t height = 100; height < 100 + 513; ++height) {
			const FullBlock block = MakeCraftedNrdBlockWithPrev(height, MakeHash(static_cast<uint8_t>(height)));
			if (pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(block))) {
				++accepted;
			}
		}
		REQUIRE(accepted == 512);
		REQUIRE_FALSE(pPeer->IsBanned());
	}
	REQUIRE(pBlockChain->AddCalls.load() == 0);
	REQUIRE_FALSE(pPeer->IsBanned());
}

TEST_CASE("BlockPipe raw queue is safe under concurrent producers")
{
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();
	const FullBlock child = MakeCraftedNrdBlockWithPrev(300, MakeHash(9));
	const ByteBuffer body = SerializeBlockBody(child);

	std::vector<std::thread> producers;
	std::atomic<int> accepted{ 0 };
	for (int i = 0; i < 8; ++i) {
		producers.emplace_back([&] {
			if (pBlockPipe->AddBlockToProcess(pPeer, ByteBuffer(body))) {
				++accepted;
			}
		});
	}
	for (auto& producer : producers) {
		producer.join();
	}

	REQUIRE(accepted.load() >= 1);
	REQUIRE_FALSE(pPeer->IsBanned());
}

TEST_CASE("BlockPipe total deferred byte limit rejects before block-count limit")
{
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	const PeerPtr pPeer = MakeLoopbackPeer();
	{
		auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
		size_t accepted = 0;
		for (uint64_t height = 200; height < 200 + 70; ++height) {
			const FullBlock block = MakeCraftedNrdBlockWithPrev(height, MakeHash(static_cast<uint8_t>(height)));
			std::vector<uint8_t> bytes = SerializeBlockBody(block).ReadRemainingBytes();
			bytes.resize(1024 * 1024, 0); // header remains parseable; body is deferred
			if (pBlockPipe->AddBlockToProcess(pPeer, ByteBuffer(std::move(bytes), EProtocolVersion::V3))) {
				++accepted;
			}
		}
		REQUIRE(accepted >= 1);
		REQUIRE(accepted < 70);
		REQUIRE_FALSE(pPeer->IsBanned());
	}
}

TEST_CASE("BlockPipe holds a same-height fork branch and applies the canonical one")
{
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();
	const FullBlock parent = MakeCraftedNrdBlockWithPrev(1, MakeHash(40));
	ByteBuffer parentBodyBuffer = SerializeBlockBody(parent);
	const BlockMessage decodedParent = BlockMessage::Deserialize(parentBodyBuffer);
	const FullBlock canonical = MakeCraftedNrdBlockWithPrev(2, decodedParent.GetBlock().GetHash());
	const FullBlock fork = MakeCraftedNrdBlockWithPrev(2, MakeHash(99), 999);

	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(fork)));
	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(parent)));
	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(canonical)));
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() == 2; }));

	std::lock_guard<std::mutex> lock(pBlockChain->OrderMutex);
	REQUIRE(pBlockChain->Order == std::vector<uint64_t>{ 1, 2 });
	REQUIRE_FALSE(pPeer->IsBanned());
}

TEST_CASE("BlockPipe releases total-byte budget after applying deferred chain and retries")
{
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();

	// Keep the worker waiting at confirmed 99 until the buffer is full.
	const FullBlock parent = MakeCraftedNrdBlockWithPrev(99, MakeHash(7), 5000);
	ByteBuffer parentBody = SerializeBlockBody(parent);
	const BlockMessage decodedParent = BlockMessage::Deserialize(parentBody);
	Hash previous = decodedParent.GetBlock().GetHash();


	std::vector<ByteBuffer> bodies;
	for (uint64_t height = 100; height < 164; ++height) {
		const FullBlock block = MakeCraftedNrdBlockWithPrev(height, previous, height + 5000);
		ByteBuffer body = SerializeBlockBody(block);
		std::vector<uint8_t> bytes = body.ReadRemainingBytes();
		bytes.resize(1048000, 0);
		ByteBuffer padded(std::move(bytes), EProtocolVersion::V3);
		previous = block.GetHash();
		bodies.push_back(std::move(padded));
	}
	for (const ByteBuffer& body : bodies) {
		REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, body));
	}

	// 64 x ~1 MB fills the 64 MiB total budget; the next block must be rejected.
	const FullBlock extra = MakeCraftedNrdBlockWithPrev(164, previous, 9999);
	ByteBuffer extraBody = SerializeBlockBody(extra);
	std::vector<uint8_t> extraBytes = extraBody.ReadRemainingBytes();
	extraBytes.resize(1048000, 0);
	ByteBuffer extraPadded(std::move(extraBytes), EProtocolVersion::V3);
	REQUIRE_FALSE(pBlockPipe->AddBlockToProcess(pPeer, extraPadded));
	{
		std::lock_guard<std::mutex> lock(pBlockChain->TipMutex);
		pBlockChain->TipHeader = decodedParent.GetBlock().GetHeader();
		pBlockChain->TipHeight = 99;
	}

	// Point the confirmed tip at the held parent so the worker applies 100..163.
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() >= 1; }));
	REQUIRE_FALSE(pPeer->IsBanned());
}

TEST_CASE("BlockPipe applies a stronger same-height alternative branch (reorg)")
{
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	pBlockChain->ReorgAware = true;
	auto pBlockPipe = BlockPipe::Create(Global::GetConfig(), pBlockChain);
	const PeerPtr pPeer = MakeLoopbackPeer();

	const FullBlock canonical1 = MakeCraftedNrdBlockWithPrev(1, MakeHash(40), 7001);
	ByteBuffer body1 = SerializeBlockBody(canonical1);
	const BlockMessage decoded1 = BlockMessage::Deserialize(body1);
	const Hash canonical1Hash = decoded1.GetBlock().GetHash();
	const FullBlock canonical2 = MakeCraftedNrdBlockWithPrev(2, canonical1Hash, 7002);
	const FullBlock alternative2 = MakeBlockWithTD(2, canonical1Hash, 10, 7003);
	ByteBuffer altBody = SerializeBlockBody(alternative2);
	const BlockMessage decodedAlt = BlockMessage::Deserialize(altBody);

	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(canonical1)));
	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(canonical2)));
	REQUIRE(pBlockPipe->AddBlockToProcess(pPeer, SerializeBlockBody(alternative2)));
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() == 3; }));

	{
		std::lock_guard<std::mutex> lock(pBlockChain->OrderMutex);
		REQUIRE(pBlockChain->Order == std::vector<uint64_t>{ 1, 2, 2 });
	}
	std::lock_guard<std::mutex> tipLock(pBlockChain->TipMutex);
	REQUIRE(pBlockChain->TipHeader->GetHash() == decodedAlt.GetBlock().GetHash());
	REQUIRE_FALSE(pPeer->IsBanned());
}

TEST_CASE("MessageProcessor is stable and applies V3 child-first through BlockPipe")
{
	auto savedContext = Global::GetContext();
	const Commitment commitment(CBigInteger<33>::ValueOf(11));
	const FullBlock parent = MakeV3Parent(1, MakeHash(41), commitment);
	ByteBuffer parentBody = SerializeBlockBody(parent);
	const BlockMessage decodedParent = BlockMessage::Deserialize(parentBody);
	const FullBlock child = MakeV3Child(2, decodedParent.GetBlock().GetHash(), commitment);

	auto pBlockChain = std::make_shared<FakeBlockChain>();
	auto pCoinView = std::make_shared<FakeCoinView>();
	pBlockChain->CoinView = pCoinView;
	pBlockChain->AddCoinViewCommitment = true;
	pBlockChain->CoinViewCommitment = commitment;
	pBlockChain->CandidateHeight = 100;
	Global::SetCoinView(pCoinView);

	auto pConnectionManager = ConnectionManager::Create();
	auto pSyncStatus = std::make_shared<SyncStatus>();
	pSyncStatus->UpdateStatus(ESyncStatus::SYNCING_BLOCKS);
	auto pPipeline = Pipeline::Create(Global::GetConfig(), pConnectionManager, pBlockChain, pSyncStatus);
	auto pPeerManager = std::make_shared<Locked<PeerManager>>(nullptr);
	auto pProcessor = std::make_unique<MessageProcessor>(*pConnectionManager, *pPeerManager, pBlockChain, pPipeline, pSyncStatus);

	const IPAddress ip = IPAddress::Parse("127.0.0.1");
	PeerPtr pPeer = std::make_shared<Peer>(ip);
	pPeer->UpdateVersion(1000);
	ConnectedPeer connectedPeer(pPeer, EDirection::OUTBOUND, 3414);
	Connection* rawConnection = new Connection(nullptr, 1, *pConnectionManager, connectedPeer, pSyncStatus, {});
	ConnectionPtr pConnection(rawConnection, [](Connection*) { });

	auto send = [&](const FullBlock& block) {
		std::vector<unsigned char> payload = SerializeBlockBody(block).ReadRemainingBytes();
		RawMessage message(MessageHeader(Global::GetMagicBytes(), MessageTypes::Block, payload.size()), std::move(payload));
		pProcessor->ProcessMessage(pConnection, message);
	};

	send(child);   // deferred: parent not committed
	send(child);   // duplicate redelivery must not fail/ban
	send(parent);
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() == 2; }));
	send(child);   // redelivery after apply must not fail/ban
	REQUIRE_FALSE(pPeer->IsBanned());
	{
		std::lock_guard<std::mutex> lock(pBlockChain->OrderMutex);
		REQUIRE(pBlockChain->Order == std::vector<uint64_t>{ 1, 2 });
	}

	// Stop the real ConnectionManager/pipe threads before their destructors:
	// ThreadPing exits only when Global::IsRunning() is false.
	Global::Shutdown();
	pProcessor.reset();
	pPipeline.reset();
	pConnectionManager.reset();
	Global::Init(savedContext);
}

TEST_CASE("MessageProcessor network path applies a stronger alternative branch")
{
	auto savedContext = Global::GetContext();
	auto pBlockChain = std::make_shared<FakeBlockChain>();
	pBlockChain->ReorgAware = true;
	auto pConnectionManager = ConnectionManager::Create();
	auto pSyncStatus = std::make_shared<SyncStatus>();
	pSyncStatus->UpdateStatus(ESyncStatus::SYNCING_BLOCKS);
	auto pPipeline = Pipeline::Create(Global::GetConfig(), pConnectionManager, pBlockChain, pSyncStatus);
	auto pPeerManager = std::make_shared<Locked<PeerManager>>(nullptr);
	auto pProcessor = std::make_unique<MessageProcessor>(*pConnectionManager, *pPeerManager, pBlockChain, pPipeline, pSyncStatus);
	const IPAddress ip = IPAddress::Parse("127.0.0.1");
	PeerPtr pPeer = std::make_shared<Peer>(ip);
	pPeer->UpdateVersion(1000);
	ConnectedPeer connectedPeer(pPeer, EDirection::OUTBOUND, 3414);
	Connection* rawConnection = new Connection(nullptr, 1, *pConnectionManager, connectedPeer, pSyncStatus, {});
	ConnectionPtr pConnection(rawConnection, [](Connection*) { });

	const FullBlock canonical1 = MakeCraftedNrdBlockWithPrev(1, MakeHash(40), 8001);
	ByteBuffer body1 = SerializeBlockBody(canonical1);
	const BlockMessage decoded1 = BlockMessage::Deserialize(body1);
	const Hash hash1 = decoded1.GetBlock().GetHash();
	const FullBlock canonical2 = MakeCraftedNrdBlockWithPrev(2, hash1, 8002);
	const FullBlock alternative2 = MakeBlockWithTD(2, hash1, 20, 8003);
	ByteBuffer altBody = SerializeBlockBody(alternative2);
	const BlockMessage decodedAlt = BlockMessage::Deserialize(altBody);

	auto send = [&](const FullBlock& block) {
		std::vector<unsigned char> payload = SerializeBlockBody(block).ReadRemainingBytes();
		RawMessage message(MessageHeader(Global::GetMagicBytes(), MessageTypes::Block, payload.size()), std::move(payload));
		pProcessor->ProcessMessage(pConnection, message);
	};

	send(canonical1);
	send(canonical2);
	send(alternative2);
	REQUIRE(WaitFor([&] { return pBlockChain->AddCalls.load() == 3; }));
	{
		std::lock_guard<std::mutex> lock(pBlockChain->OrderMutex);
		REQUIRE(pBlockChain->Order == std::vector<uint64_t>{ 1, 2, 2 });
	}
	REQUIRE_FALSE(pPeer->IsBanned());

	Global::Shutdown();
	pProcessor.reset();
	pPipeline.reset();
	pConnectionManager.reset();
	Global::Init(savedContext);
}
