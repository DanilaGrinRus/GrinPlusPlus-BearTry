#include <catch.hpp>

#include <Wallet/WalletRefresher.h>
#include <Wallet/CancelTx.h>
#include <Wallet/Keychain/KeyChain.h>
#include <API/Wallet/Owner/Models/TxLifecycle.h>
#include <Wallet/WalletDB/Models/OutputDataEntity.h>
#include <Core/Config.h>
#include <Core/Enums/Environment.h>
#include <Core/Models/Transaction.h>
#include <Core/Models/TransactionBody.h>
#include <Core/Models/TransactionKernel.h>
#include <Core/Models/TransactionOutput.h>
#include <Crypto/Crypto.h>
#include <Crypto/CSPRNG.h>

#include "Helpers/FakeNodeClient.h"
#include "Helpers/FakeWalletDB.h"

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace
{
	struct OwnOutput
	{
		Commitment commitment;
		OutputDataEntity entity;
	};

	OwnOutput MakeSpendableOutput(
		const KeyChain& keyChain,
		const std::vector<uint32_t>& pathIndices,
		const uint64_t amount,
		const std::optional<uint64_t>& height,
		const uint32_t walletTxId)
	{
		const KeyChainPath keyChainPath{ std::vector<uint32_t>(pathIndices) };
		const SecretKey blindingFactor = keyChain.DerivePrivateKey(keyChainPath, amount);
		const Commitment commitment = Crypto::CommitBlinded(amount, BlindingFactor(blindingFactor.GetBytes()));
		const RangeProof rangeProof = keyChain.GenerateRangeProof(
			keyChainPath,
			amount,
			commitment,
			blindingFactor,
			EBulletproofType::ENHANCED
		);

		const TransactionOutput output(EOutputFeatures::DEFAULT, commitment, rangeProof);
		OutputDataEntity entity(
			keyChainPath,
			blindingFactor,
			output,
			amount,
			EOutputStatus::SPENDABLE,
			std::make_optional<uint64_t>(1),
			height,
			std::make_optional<uint32_t>(walletTxId),
			std::vector<std::string>{}
		);

		return OwnOutput{ commitment, entity };
	}

	WalletTx MakeWalletTx(
		const uint32_t id,
		const uint64_t amount,
		const std::optional<uint64_t>& confirmationHeight,
		const Commitment& kernelExcess)
	{
		const TransactionKernel kernel(
			EKernelFeatures::DEFAULT_KERNEL,
			Fee(),
			confirmationHeight.value_or(0),
			kernelExcess,
			Signature()
		);

		TransactionBody body(
			std::vector<TransactionInput>{},
			std::vector<TransactionOutput>{},
			std::vector<TransactionKernel>{ kernel }
		);
		Transaction transaction(BlindingFactor(), std::move(body));

		return WalletTx(
			id,
			EWalletTxType::RECEIVED,
			std::nullopt,
			std::nullopt,
			std::nullopt,
			std::chrono::system_clock::now(),
			std::nullopt,
			confirmationHeight,
			amount,
			0,
			std::nullopt,
			std::make_optional(transaction),
			std::nullopt
		);
	}

	struct Fixture
	{
		ConfigPtr pConfig = Config::Default(Environment::AUTOMATED_TESTING);
		CBigInteger<32> masterSeed = CSPRNG::GenerateRandom32();
		SecureVector masterSeedBytes;
		KeyChain keyChain;
		std::shared_ptr<FakeWalletDB> pDB;
		std::shared_ptr<FakeNodeClient> pNode;

		Fixture()
			: masterSeedBytes(masterSeed.GetData().begin(), masterSeed.GetData().end()),
			keyChain(KeyChain::FromSeed(masterSeedBytes)),
			pDB(std::make_shared<FakeWalletDB>()),
			pNode(std::make_shared<FakeNodeClient>())
		{
			pNode->tip.height = 10;
			pNode->tip.hash = Hash::FromHex(std::string(64, 'c'));
			pNode->mmrSizeAtHeight = 0;
		}

		std::vector<OutputDataEntity> Refresh(const bool fromGenesis = false)
		{
			WalletRefresher refresher(*pConfig, pNode);
			Locked<IWalletDB> locked(pDB);
			return refresher.Refresh(masterSeedBytes, locked, fromGenesis);
		}
	};
}

TEST_CASE("W-09 status policy: kernel evidence and chain progress")
{
	const auto outputHeight = std::make_optional<uint64_t>(5);

	// Kernel found on chain: the output existed and was spent later.
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::SPENDABLE, outputHeight, 10, 9, EKernelLookupStatus::FOUND
		) == EOutputStatus::SPENT
	);

	// Kernel gone: the confirming block was reorged out.
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::SPENDABLE, outputHeight, 10, 9, EKernelLookupStatus::NOT_FOUND
		) == EOutputStatus::REVERTED
	);

	// No kernel data, but the chain advanced: a spend is possible.
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::SPENDABLE, outputHeight, 10, 9, EKernelLookupStatus::UNAVAILABLE
		) == EOutputStatus::SPENT
	);

	// No kernel data and the chain did not advance: a partial/same-height answer
	// must never produce a sticky SPENT.
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::SPENDABLE, outputHeight, 10, 10, EKernelLookupStatus::UNAVAILABLE
		) == EOutputStatus::REVERTED
	);

	// The output cannot have been spent before the chain moved past its height.
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::SPENDABLE, outputHeight, 5, 5, EKernelLookupStatus::FOUND
		) == EOutputStatus::REVERTED
	);
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::LOCKED, outputHeight, 5, 5, EKernelLookupStatus::FOUND
		) == EOutputStatus::REVERTED
	);

	// A reverted output cannot be spent in the canonical UTXO set; it stays
	// reverted until it reappears (unless a kernel proves the tx is on chain).
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::REVERTED, outputHeight, 20, 10, EKernelLookupStatus::UNAVAILABLE
		) == EOutputStatus::REVERTED
	);
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::REVERTED, outputHeight, 20, 10, EKernelLookupStatus::FOUND
		) == EOutputStatus::SPENT
	);

	// Expected-by-design states are unchanged.
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::NO_CONFIRMATIONS, std::nullopt, 20, 10, EKernelLookupStatus::UNAVAILABLE
		) == EOutputStatus::NO_CONFIRMATIONS
	);
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::SPENT, outputHeight, 20, 10, EKernelLookupStatus::FOUND
		) == EOutputStatus::SPENT
	);
	REQUIRE(
		WalletRefresher::DetermineMissingOutputStatus(
			EOutputStatus::CANCELED, outputHeight, 20, 10, EKernelLookupStatus::FOUND
		) == EOutputStatus::CANCELED
	);
}

TEST_CASE("W-09 reorg-out becomes REVERTED and recovers to SPENDABLE")
{
	Fixture f;
	const Commitment kernelExcess = Commitment::FromHex(std::string("08") + std::string(64, '1'));
	const OwnOutput own = MakeSpendableOutput(f.keyChain, { 1, 2, 3 }, 500, 5, 1);

	f.pDB->outputs.push_back(own.entity);
	f.pDB->transactions.push_back(MakeWalletTx(1, 500, 5, kernelExcess));
	f.pDB->refreshHeight = 9;
	f.pDB->restoreLeafIndex = 50;

	// The output is no longer in the node's UTXO set and its kernel is gone:
	// the confirming block was reorged out.
	f.pNode->kernels[kernelExcess] = KernelLookup{ EKernelLookupStatus::NOT_FOUND, 0 };

	const auto refreshed = f.Refresh();
	REQUIRE(refreshed.size() == 1);
	REQUIRE(f.pDB->GetOutputs(f.masterSeedBytes).front().GetStatus() == EOutputStatus::REVERTED);
	REQUIRE(f.pDB->GetTransactions(f.masterSeedBytes).front().GetType() == EWalletTxType::RECEIVED);

	// The refresh committed the new tip and rewound the restore cursor so the
	// reorg window is rescanned.
	REQUIRE(f.pDB->refreshHeight == 10);
	REQUIRE(f.pDB->restoreLeafIndex == 0);

	// The output reappears in the canonical UTXO set.
	f.pNode->unspentOutputs.insert_or_assign(own.commitment, OutputLocation(LeafIndex::At(0), 5));

	f.Refresh();
	REQUIRE(f.pDB->GetOutputs(f.masterSeedBytes).front().GetStatus() == EOutputStatus::SPENDABLE);
}

TEST_CASE("W-09 partial response at the same height is not treated as SPENT")
{
	Fixture f;
	const Commitment kernelExcess = Commitment::FromHex(std::string("08") + std::string(64, '2'));
	const OwnOutput own = MakeSpendableOutput(f.keyChain, { 1, 2, 4 }, 600, 5, 2);

	f.pDB->outputs.push_back(own.entity);
	f.pDB->transactions.push_back(MakeWalletTx(2, 600, 5, kernelExcess));
	// Same height as the node tip: no canonical spend can have happened since the
	// last successful refresh, so an omission is a partial answer or a reorg.
	f.pDB->refreshHeight = 10;
	f.pDB->restoreLeafIndex = 50;
	f.pNode->kernels[kernelExcess] = KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };

	f.Refresh();

	const auto outputs = f.pDB->GetOutputs(f.masterSeedBytes);
	REQUIRE(outputs.size() == 1);
	REQUIRE(outputs.front().GetStatus() == EOutputStatus::REVERTED);
	REQUIRE(outputs.front().GetStatus() != EOutputStatus::SPENT);
}

TEST_CASE("W-09 refresh aborts without commit when the tip changes mid-refresh")
{
	Fixture f;
	const Commitment kernelExcess = Commitment::FromHex(std::string("08") + std::string(64, '3'));
	const OwnOutput own = MakeSpendableOutput(f.keyChain, { 1, 2, 5 }, 700, 5, 3);

	f.pDB->outputs.push_back(own.entity);
	f.pDB->transactions.push_back(MakeWalletTx(3, 700, 5, kernelExcess));
	f.pDB->refreshHeight = 9;
	f.pDB->restoreLeafIndex = 50;

	// If the refresh were allowed to commit, this output would become SPENT.
	f.pNode->kernels[kernelExcess] = KernelLookup{ EKernelLookupStatus::FOUND, 5 };

	NodeTip tipA;
	tipA.height = 10;
	tipA.hash = Hash::FromHex(std::string(64, '1'));
	NodeTip tipB;
	tipB.height = 10;
	tipB.hash = Hash::FromHex(std::string(64, '2'));

	int tipCalls = 0;
	f.pNode->tipProvider = [&tipCalls, tipA, tipB]()
	{
		++tipCalls;
		return tipCalls == 1 ? tipA : tipB;
	};

	const auto refreshed = f.Refresh();
	REQUIRE(tipCalls == 2);
	REQUIRE(refreshed.size() == 1);

	// Nothing was committed: no status change, no height advance, no cursor move.
	const auto outputs = f.pDB->GetOutputs(f.masterSeedBytes);
	REQUIRE(outputs.front().GetStatus() == EOutputStatus::SPENDABLE);
	REQUIRE(f.pDB->refreshHeight == 9);
	REQUIRE(f.pDB->restoreLeafIndex == 50);
}

TEST_CASE("CancelTx refuses a REVERTED output with a typed error")
{
	Fixture f;
	const Commitment kernelExcess = Commitment::FromHex(std::string("08") + std::string(64, '5'));
	OwnOutput own = MakeSpendableOutput(f.keyChain, { 1, 2, 7 }, 900, std::nullopt, 5);
	own.entity.SetStatus(EOutputStatus::REVERTED);
	f.pDB->outputs.push_back(own.entity);

	WalletTx walletTx = MakeWalletTx(5, 900, std::nullopt, kernelExcess);
	walletTx.SetType(EWalletTxType::SENDING_FINALIZED);

	REQUIRE_THROWS_AS(
		CancelTx::CancelWalletTx(f.masterSeedBytes, f.pDB, walletTx),
		TxNotCancelableException
	);
}

TEST_CASE("W-09 lagging node: refresh does not mutate statuses and returns the wallet outputs")
{
	Fixture f;
	const Commitment kernelExcess = Commitment::FromHex(std::string("08") + std::string(64, '4'));
	const OwnOutput own = MakeSpendableOutput(f.keyChain, { 1, 2, 6 }, 800, 5, 4);

	f.pDB->outputs.push_back(own.entity);
	f.pDB->transactions.push_back(MakeWalletTx(4, 800, 5, kernelExcess));
	f.pDB->refreshHeight = 9;
	f.pDB->restoreLeafIndex = 50;
	f.pNode->tip.height = 3; // node is behind the wallet

	const auto refreshed = f.Refresh();
	REQUIRE(refreshed.size() == 1);
	REQUIRE(refreshed.front().GetStatus() == EOutputStatus::SPENDABLE);

	REQUIRE(f.pDB->GetOutputs(f.masterSeedBytes).front().GetStatus() == EOutputStatus::SPENDABLE);
	REQUIRE(f.pDB->refreshHeight == 9);
	// The cursor is rewound so the fork window is rescanned after the node catches up.
	REQUIRE(f.pDB->restoreLeafIndex == 0);
}
