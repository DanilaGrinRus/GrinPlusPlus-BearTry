#include <catch.hpp>

#include <TestServer.h>
#include <TestMiner.h>
#include <TxBuilder.h>

#include <BlockChain/BlockChain.h>
#include <Consensus.h>
#include <Core/Util/TransactionUtil.h>
#include <Database/BlockDb.h>
#include <PMMR/TxHashSetImpl.h>
#include <Wallet/Keychain/KeyChain.h>

#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace
{
	std::shared_ptr<TxHashSet> RequireCompleteNrdIndex(const IBlockChain::Ptr& pChain)
	{
		const std::shared_ptr<ITxHashSet> pBase = pChain->GetTxHashSet();
		auto pTxHashSet = std::dynamic_pointer_cast<TxHashSet>(pBase);
		REQUIRE(pTxHashSet != nullptr);
		REQUIRE(pTxHashSet->IsRecentNrdIndexComplete());
		return pTxHashSet;
	}

	Test::Input MakeCoinbaseInput(const MinedBlock& minedBlock, const KeyChainPath& path, const uint64_t amount)
	{
		const TransactionOutput& output = minedBlock.block.GetOutputs().front();
		return Test::Input({
			{ output.GetFeatures(), output.GetCommitment() },
			path,
			amount
		});
	}

	Test::Input MakeOutputInput(
		const TransactionOutput& output,
		const KeyChainPath& path,
		const uint64_t amount)
	{
		return Test::Input({
			{ output.GetFeatures(), output.GetCommitment() },
			path,
			amount
		});
	}
}

// Task-8(b): reconstructed bodyless state. This is NOT network PIBD: a real
// TestServer chain is mined, then all block BODIES are removed from the real
// BlockDB while headers / kernel MMR / txhashset remain. The chain is reopened on
// the same paths, startup rebuilds the NRD index from headers+kernel MMR, an NRD
// block is applied, the chain reorgs to an alternative branch, and the state is
// restarted again. No production code is changed.
// Test-only consensus configuration: this test exercises bodyless NRD index
// reconstruction on a real TestServer/BlockDB. Mining a real 552,960-block
// Floonet chain is not practical, so AUTOMATED_TESTING permits the NRD kernel at
// its normal v1 header for this explicit test only. Production and the default
// test configuration keep the HF3 rejection; the RAII guard clears the seam even
// when a REQUIRE throws.
namespace
{
	struct NrdBeforeHf3ForTesting
	{
		NrdBeforeHf3ForTesting()
		{
			Consensus::SetNrdBeforeHf3OverrideForTesting(true);
		}

		~NrdBeforeHf3ForTesting()
		{
			Consensus::SetNrdBeforeHf3OverrideForTesting(false);
		}
	};
}

// Bodyless NRD/reorg scenario. Full reorg is not claimed: this reconstructed
// state intentionally has no bodies, so the node must refuse the alternative
// branch safely before mutating chain/TxHashSet/MMR state. The test then verifies
// that the original tip and NRD index survive a real restart unchanged.
TEST_CASE("Bodyless NRD refuses reorg safely and restarts on the original tip")
{
	const NrdBeforeHf3ForTesting nrdSeam;
	BlockHeaderPtr pTip29;
	BlockHeaderPtr pTip30;
	Hash tip30Hash;
	uint64_t kernelCount30 = 0;
	Commitment nrdExcess;
	std::optional<FullBlock> block30bCandidate;
	KeyChain keyChain = KeyChain::FromRandom();

	// ---- phase 1: real chain + one NRD block, then remove all block bodies ----
	{
		TestServer::Ptr pServer = TestServer::Create();
		pServer->SetPreserveDataOnDestruct(true);

		TestMiner miner(pServer);
		TxBuilder txBuilder(keyChain);
		IBlockChain::Ptr pChain = pServer->GetBlockChain();

		const std::vector<MinedBlock> minedChain = miner.MineChain(keyChain, 30);
		REQUIRE(minedChain.size() == 30);
		pTip29 = minedChain.back().block.GetHeader();

		// Spend the coinbase of block 1 through an NRD kernel.
		TxBuilder::Criteria nrdCriteria;
		nrdCriteria.inputs = {
			MakeCoinbaseInput(minedChain[1], minedChain[1].coinbasePath.value(), minedChain[1].coinbaseAmount)
		};
		nrdCriteria.outputs = {
			Test::Output{ KeyChainPath({ 1, 100 }), 9'000'000 },
			Test::Output{ KeyChainPath({ 1, 101 }), minedChain[1].coinbaseAmount - 9'000'000 }
		};
		nrdCriteria.include_change = false;
		nrdCriteria.fee_base = 0;
		nrdCriteria.num_kernels = 1;
		nrdCriteria.kernel_features = EKernelFeatures::NO_RECENT_DUPLICATE;
		nrdCriteria.relative_height = 2;
		const Transaction nrdTx = txBuilder.BuildTx(nrdCriteria);
		REQUIRE(!nrdTx.GetBody().GetKernels().empty());
		REQUIRE(nrdTx.GetBody().GetKernels().front().GetFeatures() == EKernelFeatures::NO_RECENT_DUPLICATE);
		REQUIRE(!nrdTx.GetBody().GetKernels().front().IsCoinbase());
		nrdExcess = nrdTx.GetBody().GetKernels().front().GetExcessCommitment();

		Test::Tx coinbase30 = txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 30 }));
		REQUIRE(coinbase30.pTransaction->GetBody().GetKernels().front().IsCoinbase());
		TransactionPtr pCombinedTx = TransactionUtil::Aggregate({
			coinbase30.pTransaction,
			std::make_shared<Transaction>(nrdTx)
		});
		FullBlock block30 = miner.MineNextBlock(minedChain.back().block.GetHeader(), *pCombinedTx, {});
		REQUIRE(pChain->AddBlock(block30) == EBlockChainStatus::SUCCESS);
		REQUIRE(pChain->GetHeight(EChainType::CONFIRMED) == 30);

		pTip30 = block30.GetHeader();
		tip30Hash = pTip30->GetHash();
		kernelCount30 = pTip30->GetNumKernels();
		{
			auto pTxHashSet = RequireCompleteNrdIndex(pChain);
			REQUIRE(pTxHashSet->GetRecentNrdEntries(nrdExcess).size() == 1);
			REQUIRE(pTxHashSet->GetRecentNrdEntries(nrdExcess).front().Height == 30);
		}

		// Build a conflicting height-30 block BEFORE removing bodies. Later it is
		// submitted against the bodyless state; the node must refuse it without
		// mutating chain/TxHashSet/MMR state.
		Test::Tx coinbase30b = txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 130 }));
		block30bCandidate = miner.MineNextBlock(
			minedChain.back().block.GetHeader(), *coinbase30b.pTransaction, {});
		REQUIRE(block30bCandidate->GetHeight() == 30);
		REQUIRE(block30bCandidate->GetPreviousHash() == minedChain.back().block.GetHeader()->GetHash());
		REQUIRE(block30bCandidate->GetHash() != tip30Hash);

		// Reconstructed PIBD-like state: bodies gone, headers/kernel MMR remain.
		auto pBlockDB = pServer->GetBlockDB();
		{
			auto pWriter = pBlockDB->Write();
			pWriter->ClearBlocks();
			pWriter->Commit();
		}
		REQUIRE(pBlockDB->Read()->GetBlock(tip30Hash) == nullptr);
		REQUIRE(pBlockDB->Read()->GetBlockHeader(tip30Hash) != nullptr);
	}

	// ---- phase 2: reopen same paths; startup rebuild from bodyless state ----
	{
		TestServer::Ptr pServer = TestServer::OpenExisting();
		pServer->SetPreserveDataOnDestruct(true);
		IBlockChain::Ptr pChain = pServer->GetBlockChain();
		REQUIRE(pChain->GetHeight(EChainType::CONFIRMED) == 30);
		REQUIRE(pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() == tip30Hash);

		auto pTxHashSet = RequireCompleteNrdIndex(pChain);
		REQUIRE(pTxHashSet->GetNumKernels() == kernelCount30);
		REQUIRE(pTxHashSet->GetRecentNrdEntries(nrdExcess).size() == 1);
		REQUIRE(pTxHashSet->GetRecentNrdEntries(nrdExcess).front().Height == 30);
		REQUIRE(pServer->GetBlockDB()->Read()->GetBlock(tip30Hash) == nullptr);

		// The pre-built conflicting height-30 block is submitted against the
		// bodyless state. The node must detect the reorg and refuse it because
		// the confirmed tip body is absent, without mutating state.
		REQUIRE(block30bCandidate.has_value());
		REQUIRE(block30bCandidate->GetHeight() == 30);
		REQUIRE(block30bCandidate->GetPreviousHash() == pTip29->GetHash());
		REQUIRE(block30bCandidate->GetHash() != tip30Hash);

		REQUIRE(pChain->AddBlock(*block30bCandidate) == EBlockChainStatus::LOCAL_REWIND_DATA_MISSING);
		REQUIRE(pChain->GetHeight(EChainType::CONFIRMED) == 30);
		REQUIRE(pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() == tip30Hash);
		REQUIRE(RequireCompleteNrdIndex(pChain)->GetRecentNrdEntries(nrdExcess).size() == 1);
	}
	// ---- phase 3: restart after the safe refusal; state must be unchanged ----
	{
		TestServer::Ptr pServer = TestServer::OpenExisting();
		pServer->SetPreserveDataOnDestruct(true);
		IBlockChain::Ptr pChain = pServer->GetBlockChain();
		REQUIRE(pChain->GetHeight(EChainType::CONFIRMED) == 30);
		REQUIRE(pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() == tip30Hash);
		auto pIndex = RequireCompleteNrdIndex(pChain);
		REQUIRE(pIndex->GetRecentNrdEntries(nrdExcess).size() == 1);
		REQUIRE(pChain->GetBlockByHeight(30) == nullptr);
		REQUIRE(pChain->GetBlockHeaderByHeight(30, EChainType::CONFIRMED) != nullptr);
	}
}