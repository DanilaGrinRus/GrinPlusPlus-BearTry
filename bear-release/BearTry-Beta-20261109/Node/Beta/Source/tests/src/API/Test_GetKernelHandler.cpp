#include <catch.hpp>

#include <TestMiner.h>
#include <TestServer.h>

#include <API/Node/Handlers/GetKernelHandler.h>
#include <BlockChain/BlockChain.h>
#include <Consensus.h>
#include <Core/Models/Fee.h>
#include <Core/Util/TransactionUtil.h>
#include <Crypto/CSPRNG.h>
#include <Crypto/Crypto.h>
#include <Database/Database.h>
#include <Database/BlockDb.h>
#include <Wallet/Keychain/KeyChain.h>

#include <memory>
#include <optional>
#include <string>

namespace
{
	RPC::Request MakeKernelRequest(
		const Commitment& excess,
		const std::optional<uint64_t>& minHeight = std::nullopt,
		const std::optional<uint64_t>& maxHeight = std::nullopt)
	{
		Json::Value params(Json::arrayValue);
		params.append(excess.ToHex());
		if (minHeight.has_value()) {
			params.append(Json::UInt64(minHeight.value()));
		} else {
			params.append(Json::nullValue);
		}
		if (maxHeight.has_value()) {
			params.append(Json::UInt64(maxHeight.value()));
		} else {
			params.append(Json::nullValue);
		}

		return RPC::Request::BuildRequest("get_kernel", params);
	}

	const Json::Value& RequireLocated(const RPC::Response& response)
	{
		REQUIRE(response.GetResult().has_value());
		const Json::Value& result = response.GetResult().value();
		REQUIRE(result.isMember("Ok"));
		return result["Ok"];
	}

	void RequireNotFound(const RPC::Response& response)
	{
		REQUIRE(response.GetError().has_value());
		REQUIRE(response.GetError()->GetMsg() == "Kernel not found");
	}

	// Builds a balanced spend that reuses a caller-provided kernel commitment.
	// Two such transactions with different coinbase inputs/outputs can share the
	// same kernel excess, giving a real duplicate-excess chain without violating
	// any consensus rule (plain kernel, fee 0).
	Transaction BuildSpendWithSharedKernel(
		KeyChain& keyChain,
		TxBuilder& txBuilder,
		const MinedBlock& coinbaseBlock,
		const KeyChainPath& outputPath,
		const BlindingFactor& kernelBlind,
		const TransactionKernel& kernel)
	{
		REQUIRE(coinbaseBlock.coinbasePath.has_value());
		const KeyChainPath& inputPath = coinbaseBlock.coinbasePath.value();
		const uint64_t amount = coinbaseBlock.coinbaseAmount;
		const TransactionOutput& coinbaseOutput = coinbaseBlock.block.GetOutputs().front();

		const Test::Input input({
			{ coinbaseOutput.GetFeatures(), coinbaseOutput.GetCommitment() },
			inputPath,
			amount
		});
		const auto builtOutput = txBuilder.BuildOutput(
			Test::Output{ outputPath, amount },
			EOutputFeatures::DEFAULT);

		const BlindingFactor inputBlind(keyChain.DerivePrivateKey(inputPath, amount).GetBytes());
		const BlindingFactor totalExcess =
			Crypto::AddBlindingFactors({ builtOutput.first }, { inputBlind });
		BlindingFactor offset = Crypto::AddBlindingFactors({ totalExcess }, { kernelBlind });

		TransactionBody body(
			std::vector<TransactionInput>({ input.input }),
			std::vector<TransactionOutput>({ builtOutput.second }),
			std::vector<TransactionKernel>({ kernel }));

		return Transaction(std::move(offset), std::move(body));
	}
}

// Regression for the direct-scan fallback dangling pointer: pKernel used to point
// at a block-local optional (GetKernelHandler.h). This exercises the handler-level
// JSON through the real TestServer DB/txhashset with the index disabled, then with
// the index enabled, and checks min/max height ranges and NOT_FOUND.
TEST_CASE("GetKernelHandler direct-scan path returns located kernel and ranges")
{
	TestServer::Ptr pServer = TestServer::Create();
	TestMiner miner(pServer);
	KeyChain keyChain = KeyChain::FromRandom();
	const std::vector<MinedBlock> chain = miner.MineChain(keyChain, 8);
	REQUIRE(chain.size() == 8);

	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	auto pManager = pServer->GetTxHashSetManagerShared();
	IDatabasePtr pDatabase = pServer->GetDatabase();
	GetKernelHandler handler(pChain, pManager, pDatabase);

	// Use the coinbase kernel of block 2 (height 2).
	REQUIRE(chain[2].block.GetKernels().size() == 1);
	const TransactionKernel& expectedKernel = chain[2].block.GetKernels().front();
	const Commitment excess = expectedKernel.GetExcessCommitment();

	// Force the direct-scan fallback: remove any persistent index state.
	{
		auto pWriter = pServer->GetBlockDB()->Write();
		pWriter->ClearAllKernelPositions();
		pWriter->Commit();
	}
	REQUIRE(pServer->GetBlockDB()->Read()->GetKernelIndexState() == nullptr);

	{
		const RPC::Response response = handler.Handle(MakeKernelRequest(excess));
		const Json::Value& located = RequireLocated(response);
		REQUIRE(located["height"].asUInt64() == 2);
		REQUIRE(located.isMember("mmr_index"));
		REQUIRE(located.isMember("tx_kernel"));
		const Json::Value& kernel = located["tx_kernel"];
		REQUIRE(kernel["excess"].asString() == excess.ToHex());
		REQUIRE(kernel.isMember("features"));
		REQUIRE(kernel.isMember("fee_shift"));
		REQUIRE(kernel.isMember("fee"));
		REQUIRE(kernel.isMember("lock_height"));
		REQUIRE(kernel.isMember("excess_sig"));
	}

	// Missing excess.
	const Commitment missing{CBigInteger<33>::ValueOf(static_cast<uint8_t>(0x7B))};
	RequireNotFound(handler.Handle(MakeKernelRequest(missing)));

	// Height range around the kernel's block.
	REQUIRE(RequireLocated(handler.Handle(MakeKernelRequest(excess, 2, 2)))["height"].asUInt64() == 2);
	RequireNotFound(handler.Handle(MakeKernelRequest(excess, 3, std::nullopt)));
	RequireNotFound(handler.Handle(MakeKernelRequest(excess, std::nullopt, 1)));

	// Index-enabled path must return the same JSON shape/result.
	REQUIRE(pChain->EnsureKernelIndex());
	REQUIRE(pServer->GetBlockDB()->Read()->GetKernelIndexState() != nullptr);
	{
		const RPC::Response response = handler.Handle(MakeKernelRequest(excess));
		const Json::Value& located = RequireLocated(response);
		REQUIRE(located["height"].asUInt64() == 2);
		REQUIRE(located["tx_kernel"]["excess"].asString() == excess.ToHex());
		REQUIRE(located.isMember("mmr_index"));
	}
}

// Handler-level duplicate-excess ranges (the E2E gap): two REAL blocks share one
// plain kernel excess at heights 30 and 32. With the index disabled, a direct scan
// must return h32 for an unlimited/lower-bounded range, h30 when max_height=31
// (latest-in-range fallback), and NOT_FOUND for the narrow h31..h31 range.
TEST_CASE("GetKernelHandler duplicate excess returns latest in range")
{
	TestServer::Ptr pServer = TestServer::Create();
	TestMiner miner(pServer);
	KeyChain keyChain = KeyChain::FromRandom();
	TxBuilder txBuilder(keyChain);
	const std::vector<MinedBlock> chain = miner.MineChain(keyChain, 30);
	REQUIRE(chain.size() == 30);

	const BlindingFactor kernelBlind = CSPRNG::GenerateRandom32();
	const Commitment kernelCommit = Crypto::CommitBlinded(0, kernelBlind);
	const TransactionKernel sharedKernel = txBuilder.BuildKernel(
		EKernelFeatures::DEFAULT_KERNEL,
		Fee::From(0),
		kernelBlind,
		kernelCommit);

	const Transaction tx1 = BuildSpendWithSharedKernel(
		keyChain, txBuilder, chain[1], KeyChainPath({ 3, 1 }), kernelBlind, sharedKernel);
	const Transaction tx2 = BuildSpendWithSharedKernel(
		keyChain, txBuilder, chain[2], KeyChainPath({ 3, 2 }), kernelBlind, sharedKernel);
	REQUIRE(tx1.GetBody().GetKernels().front().GetExcessCommitment() ==
		tx2.GetBody().GetKernels().front().GetExcessCommitment());

	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	Test::Tx coinbase30 = txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 30 }));
	TransactionPtr pAggregate30 = TransactionUtil::Aggregate({
		coinbase30.pTransaction,
		std::make_shared<Transaction>(tx1)
	});
	const FullBlock block30 = miner.MineNextBlock(chain.back().block.GetHeader(), *pAggregate30);
	REQUIRE(pChain->AddBlock(block30) == EBlockChainStatus::SUCCESS);

	Test::Tx coinbase31 = txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 31 }));
	const FullBlock block31 = miner.MineNextBlock(block30.GetHeader(), *coinbase31.pTransaction);
	REQUIRE(pChain->AddBlock(block31) == EBlockChainStatus::SUCCESS);

	Test::Tx coinbase32 = txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, 32 }));
	TransactionPtr pAggregate32 = TransactionUtil::Aggregate({
		coinbase32.pTransaction,
		std::make_shared<Transaction>(tx2)
	});
	const FullBlock block32 = miner.MineNextBlock(block31.GetHeader(), *pAggregate32);
	REQUIRE(pChain->AddBlock(block32) == EBlockChainStatus::SUCCESS);
	REQUIRE(pChain->GetHeight(EChainType::CONFIRMED) == 32);

	{
		auto pWriter = pServer->GetBlockDB()->Write();
		pWriter->ClearAllKernelPositions();
		pWriter->Commit();
	}

	auto pManager = pServer->GetTxHashSetManagerShared();
	IDatabasePtr pDatabase = pServer->GetDatabase();
	GetKernelHandler handler(pChain, pManager, pDatabase);
	const Commitment excess = sharedKernel.GetExcessCommitment();

	REQUIRE(RequireLocated(handler.Handle(MakeKernelRequest(excess)))["height"].asUInt64() == 32);
	REQUIRE(RequireLocated(handler.Handle(MakeKernelRequest(excess, 32, std::nullopt)))["height"].asUInt64() == 32);
	REQUIRE(RequireLocated(handler.Handle(MakeKernelRequest(excess, std::nullopt, 31)))["height"].asUInt64() == 30);
	RequireNotFound(handler.Handle(MakeKernelRequest(excess, 31, 31)));
}
