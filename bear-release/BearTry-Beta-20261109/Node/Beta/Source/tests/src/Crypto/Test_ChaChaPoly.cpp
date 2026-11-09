#include <catch.hpp>
#include <Crypto/Age.h>

#include <Crypto/ChaChaPoly.h>
#include <Crypto/CSPRNG.h>

TEST_CASE("ChaChaPoly - Stream Encryption")
{
	const SecretKey encryption_key = CSPRNG::GenerateRandom32();

	SecureVector random_data = CSPRNG::GenerateRandomBytes(500);
	std::vector<uint8_t> vec(random_data.cbegin(), random_data.cend());
	
	std::vector<uint8_t> encrypted = ChaChaPoly::Init(encryption_key).StreamEncrypt(vec);
	std::vector<uint8_t> decrypted = ChaChaPoly::Init(encryption_key).StreamDecrypt(encrypted);

	REQUIRE(vec == decrypted);
}
TEST_CASE("Age rejects modified header MAC with intact encrypted payload")
{
    const auto pair = X25519::GenerateEphemeralKeypair();
    const std::vector<uint8_t> payload{1, 2, 3};
    auto encoded = Age::Encrypt({pair.pubkey}, payload);
    REQUIRE(Age::Decrypt(pair, encoded) == payload);
    const std::string marker = "--- ";
    auto pos = std::search(encoded.begin(), encoded.end(), marker.begin(), marker.end());
    REQUIRE(pos != encoded.end());
    auto& first = *(pos + marker.size());
    first = first == 'A' ? 'B' : 'A';
    REQUIRE_THROWS(Age::Decrypt(pair, encoded));
}
