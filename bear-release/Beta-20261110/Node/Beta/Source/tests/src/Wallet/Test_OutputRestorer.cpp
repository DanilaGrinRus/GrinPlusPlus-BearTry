#include <catch.hpp>

#include <Wallet/OutputRestorer.h>
#include <Wallet/Keychain/KeyChain.h>
#include <Crypto/Crypto.h>
#include <Crypto/CSPRNG.h>
#include <Server/Node/NodeClients/DefaultNodeClient.h>
#include "Helpers/FakeNodeClient.h"
#include "Helpers/FakeWalletDB.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace
{
	constexpr uint64_t TEST_HEIGHT = 1300000; // header version 5: enhanced bulletproofs only

	struct OwnOutput
	{
		OutputDTO dto;
		Commitment commitment;
	};

	OwnOutput MakeOwnOutput(
		const KeyChain& keyChain,
		const std::vector<uint32_t>& pathIndices,
		const uint64_t amount,
		const uint64_t leafOrdinal)
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

		return OwnOutput{
			OutputDTO(
				false,
				OutputIdentifier(EOutputFeatures::DEFAULT, Commitment(commitment)),
				OutputLocation(LeafIndex::At(leafOrdinal), TEST_HEIGHT),
				rangeProof
			),
			commitment
		};
	}

	struct Fixture
	{
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
			pNode->tip.height = 2000000;
			pNode->tip.hash = Hash::FromHex(std::string(64, 'b'));
		}
	};
}

TEST_CASE("OutputRestorer: empty page after collected outputs does not discard them (W-05)")
{
	Fixture f;
	const OwnOutput own = MakeOwnOutput(f.keyChain, { 1, 2, 3 }, 111, 1);

	// First page reports last_retrieved_index == 0 (the historical "no outputs"
	// sentinel) but already contains an output we own; the follow-up page is empty
	// and reaches the end of the scan window.
	f.pNode->pages.push_back(OutputRange(10, 0, std::vector<OutputDTO>{ own.dto }));
	f.pNode->pages.push_back(OutputRange(10, 10, std::vector<OutputDTO>{}));

	OutputRestorer restorer(f.pNode, f.keyChain);
	const auto outputs = restorer.FindAndRewindOutputs(f.pDB, false);

	REQUIRE(outputs.size() == 1);
	REQUIRE(outputs.front().GetCommitment() == own.commitment);
	REQUIRE(f.pDB->restoreLeafIndex == 10);
}

TEST_CASE("OutputRestorer: resume from a PMMR-position cursor")
{
	Fixture f;
	const OwnOutput own = MakeOwnOutput(f.keyChain, { 1, 2, 4 }, 222, 3);
	f.pDB->restoreLeafIndex = 6; // 0-based next position

	f.pNode->pages.push_back(OutputRange(10, 10, std::vector<OutputDTO>{ own.dto }));

	OutputRestorer restorer(f.pNode, f.keyChain);
	const auto outputs = restorer.FindAndRewindOutputs(f.pDB, false);

	REQUIRE(f.pNode->lastScanStart == 7); // 1-based start position = cursor + 1
	REQUIRE(outputs.size() == 1);
	REQUIRE(f.pDB->restoreLeafIndex == 10);
}

TEST_CASE("OutputRestorer: fromGenesis ignores the persisted cursor")
{
	Fixture f;
	f.pDB->restoreLeafIndex = 50;

	OutputRestorer restorer(f.pNode, f.keyChain);
	const auto outputs = restorer.FindAndRewindOutputs(f.pDB, true);

	REQUIRE(f.pNode->lastScanStart == 0);
	REQUIRE(outputs.empty());
	REQUIRE(f.pDB->restoreLeafIndex == 0);
}

TEST_CASE("OutputRestorer: nullptr range is 'not ready', not an empty scan (W-05)")
{
	Fixture f;
	f.pNode->returnNullPage = true;

	OutputRestorer restorer(f.pNode, f.keyChain);
	REQUIRE_THROWS_AS(
		restorer.FindAndRewindOutputs(f.pDB, false),
		NodeUnavailableException
	);
	REQUIRE(f.pDB->restoreLeafIndex == 0);
}

TEST_CASE("OutputRestorer: a non-advancing page stops the scan (progress protection)")
{
	Fixture f;
	f.pNode->pages.push_back(OutputRange(10, 3, std::vector<OutputDTO>{}));
	f.pNode->pages.push_back(OutputRange(10, 3, std::vector<OutputDTO>{}));

	OutputRestorer restorer(f.pNode, f.keyChain);
	REQUIRE_THROWS_AS(
		restorer.FindAndRewindOutputs(f.pDB, false),
		NodeException
	);

	// The cursor must not be moved by a failed scan; the batch rollback is the
	// caller's responsibility, but the restorer itself must not expose progress.
	REQUIRE(f.pDB->restoreLeafIndex == 0);
}

TEST_CASE("OutputRestorer: backend cursor units map deterministically (local <-> remote)")
{
	// Local leaf ordinal -> 0-based PMMR node position.
	REQUIRE(DefaultNodeClient::LeafPositionForOrdinal(0) == 0);
	REQUIRE(DefaultNodeClient::LeafPositionForOrdinal(1) == 1);
	REQUIRE(DefaultNodeClient::LeafPositionForOrdinal(2) == 3);
	REQUIRE(DefaultNodeClient::LeafPositionForOrdinal(3) == 4);

	// ... and back, for a leaf position.
	REQUIRE(DefaultNodeClient::LeafOrdinalForPosition(0) == 0);
	REQUIRE(DefaultNodeClient::LeafOrdinalForPosition(1) == 1);
	REQUIRE(DefaultNodeClient::LeafOrdinalForPosition(3) == 2);
	REQUIRE(DefaultNodeClient::LeafOrdinalForPosition(4) == 3);

	// A position that points at an internal node is rounded up to the next leaf;
	// this is what makes a persisted PMMR-position cursor safe across backends.
	REQUIRE(DefaultNodeClient::NextLeafPosition(0) == 0);
	REQUIRE(DefaultNodeClient::NextLeafPosition(2) == 3);
	REQUIRE(DefaultNodeClient::NextLeafPosition(5) == 7);
	REQUIRE(DefaultNodeClient::NextLeafPosition(6) == 7);
}
