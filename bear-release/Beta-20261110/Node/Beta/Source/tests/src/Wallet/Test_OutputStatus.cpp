#include <catch.hpp>

#include <Wallet/Enums/OutputStatus.h>
#include <Wallet/WalletDB/Models/OutputDataEntity.h>
#include <Wallet/Keychain/KeyChain.h>
#include <Crypto/Crypto.h>
#include <Crypto/CSPRNG.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Serialization/ByteBuffer.h>

#include <optional>
#include <vector>

TEST_CASE("OutputStatus: REVERTED string round-trip")
{
	REQUIRE(OutputStatus::FromString("Reverted") == EOutputStatus::REVERTED);
	REQUIRE(OutputStatus::ToString(EOutputStatus::REVERTED) == "Reverted");

	for (int value = 0; value <= 6; ++value)
	{
		const EOutputStatus status = (EOutputStatus)value;
		REQUIRE(OutputStatus::FromString(OutputStatus::ToString(status)) == status);
	}
}

TEST_CASE("OutputDataEntity: REVERTED status survives serialization (save/load representation)")
{
	// Build a real output so the serialized blob matches what the wallet DB stores.
	const CBigInteger<32> masterSeed = CSPRNG::GenerateRandom32();
	const SecureVector masterSeedBytes(masterSeed.GetData().begin(), masterSeed.GetData().end());
	const KeyChain keyChain = KeyChain::FromSeed(masterSeedBytes);

	const KeyChainPath keyChainPath(std::vector<uint32_t>({ 1, 2, 3 }));
	const uint64_t amount = 123456789;
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

	const OutputDataEntity original(
		keyChainPath,
		blindingFactor,
		output,
		amount,
		EOutputStatus::REVERTED,
		std::make_optional<uint64_t>(42),
		std::make_optional<uint64_t>(700000),
		std::make_optional<uint32_t>(7),
		std::vector<std::string>{}
	);

	Serializer serializer;
	original.Serialize(serializer);
	std::vector<uint8_t> serialized = serializer.GetBytes();
	ByteBuffer byteBuffer(std::move(serialized));
	const OutputDataEntity restored = OutputDataEntity::Deserialize(byteBuffer);

	REQUIRE(restored.GetStatus() == EOutputStatus::REVERTED);
	REQUIRE(restored.GetAmount() == amount);
	REQUIRE(restored.GetCommitment() == commitment);
	REQUIRE(restored.GetBlockHeight().value() == 700000);
	REQUIRE(restored.GetMMRIndex().value() == 42);
	REQUIRE(restored.GetWalletTxId().value() == 7);

	// REVERTED must not be treated as spendable by callers that query the status.
	REQUIRE(restored.GetStatus() != EOutputStatus::SPENDABLE);
}
