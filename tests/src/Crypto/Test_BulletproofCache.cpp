#include <catch.hpp>

#include <Crypto/BulletProofsCache.h>
#include <Crypto/Bulletproofs.h>
#include <Crypto/Crypto.h>
#include <Crypto/CSPRNG.h>
#include <Wallet/Keychain/KeyChain.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/RangeProof.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>

TEST_CASE("Crypto::BulletproofsCacheBindsProofBytes")
{
	std::vector<unsigned char> commitmentBytes(33, 7);
	const Commitment commitment(CBigInteger<33>(std::move(commitmentBytes)));
	const RangeProof proofA(std::vector<unsigned char>(675, 1));
	const RangeProof proofB(std::vector<unsigned char>(675, 2));

	BulletProofsCache cache;
	REQUIRE_FALSE(cache.WasAlreadyVerified(commitment, proofA));

	cache.AddToCache(commitment, proofA);
	REQUIRE(cache.WasAlreadyVerified(commitment, proofA));

	// Same commitment, different proof: must not hit the cache.
	REQUIRE_FALSE(cache.WasAlreadyVerified(commitment, proofB));

	cache.AddToCache(commitment, proofB);
	REQUIRE(cache.WasAlreadyVerified(commitment, proofA));
	REQUIRE(cache.WasAlreadyVerified(commitment, proofB));
}

TEST_CASE("Crypto::BulletproofsRejectsEmptyBatchItem")
{
	std::vector<unsigned char> commitmentBytes(33, 9);
	const Commitment commitment(CBigInteger<33>(std::move(commitmentBytes)));

	REQUIRE(Bulletproofs::GetInstance().VerifyBulletproofs({}));

	std::vector<std::pair<Commitment, RangeProof>> invalid;
	invalid.emplace_back(commitment, RangeProof(std::vector<unsigned char>()));
	REQUIRE_FALSE(Bulletproofs::GetInstance().VerifyBulletproofs(invalid));
}
TEST_CASE("Crypto::BulletproofsWarmCacheRejectsMutatedProof")
{
	const CBigInteger<32> masterSeed = CSPRNG::GenerateRandom32();
	const SecureVector seedBytes(masterSeed.GetData().begin(), masterSeed.GetData().end());
	const uint64_t amount = 45;
	const KeyChainPath keyId(std::vector<uint32_t>({ 1, 2, 3 }));
	KeyChain keyChain = KeyChain::FromSeed(seedBytes);
	const SecretKey blindingFactor = keyChain.DerivePrivateKey(keyId, amount);
	const Commitment commitment =
		Crypto::CommitBlinded(amount, BlindingFactor(blindingFactor.GetBytes()));
	const RangeProof proof =
		keyChain.GenerateRangeProof(keyId, amount, commitment, blindingFactor, EBulletproofType::ORIGINAL);

	std::vector<std::pair<Commitment, RangeProof>> valid;
	valid.emplace_back(commitment, proof);
	REQUIRE(Bulletproofs::GetInstance().VerifyBulletproofs(valid));
	// Warm cache hit; the same exact proof remains valid.
	REQUIRE(Bulletproofs::GetInstance().VerifyBulletproofs(valid));

	std::vector<unsigned char> mutatedBytes = proof.GetProofBytes();
	REQUIRE(mutatedBytes.size() > 10);
	mutatedBytes[10] ^= 0x01;
	std::vector<std::pair<Commitment, RangeProof>> mutated;
	mutated.emplace_back(commitment, RangeProof(std::move(mutatedBytes)));
	REQUIRE_FALSE(Bulletproofs::GetInstance().VerifyBulletproofs(mutated));
}
TEST_CASE("Crypto::RangeProofSizeBoundaries")
{
	for (size_t length = 0; length <= (size_t)MAX_PROOF_SIZE + 1; ++length) {
		Serializer serializer;
		serializer.Append<uint64_t>(length);
		serializer.AppendByteVector(std::vector<uint8_t>(length, 0xA5));
		ByteBuffer buffer(serializer.GetBytes(), EProtocolVersion::V1);
		if (length <= (size_t)MAX_PROOF_SIZE) {
			REQUIRE_NOTHROW(RangeProof::Deserialize(buffer));
			REQUIRE(buffer.GetRemainingSize() == 0);
		} else {
			REQUIRE_THROWS_AS(RangeProof::Deserialize(buffer), DeserializationException);
		}
	}
}