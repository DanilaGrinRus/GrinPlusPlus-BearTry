#include <catch.hpp>

#include <Crypto/CSPRNG.h>
#include <Wallet/WalletDB/WalletEncryptionUtil.h>

#include <stdexcept>

TEST_CASE("WalletEncryptionUtil authenticates database rows")
{
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);
	const SecureVector plaintext{ 1, 2, 3, 4, 5 };
	const std::string dataType = "test-row";

	const std::vector<uint8_t> encrypted = WalletEncryptionUtil::Encrypt(masterSeed, dataType, plaintext);
	const SecureVector decrypted = WalletEncryptionUtil::Decrypt(masterSeed, dataType, encrypted);
	REQUIRE(decrypted == plaintext);

	std::vector<uint8_t> tampered = encrypted;
	tampered.back() ^= 0x01;
	REQUIRE_THROWS_AS(
		WalletEncryptionUtil::Decrypt(masterSeed, dataType, tampered),
		std::runtime_error
	);
}

TEST_CASE("WalletEncryptionUtil binds row identity as associated data (W-06)")
{
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);
	const std::string dataType = "test-row";
	const SecureVector plaintext1{ 1, 2, 3, 4, 5 };
	const SecureVector plaintext2{ 6, 7, 8, 9, 10 };
	const std::vector<uint8_t> associatedData1{ 'r', 'o', 'w', '-', '1' };
	const std::vector<uint8_t> associatedData2{ 'r', 'o', 'w', '-', '2' };

	const std::vector<uint8_t> encrypted1 = WalletEncryptionUtil::Encrypt(masterSeed, dataType, associatedData1, plaintext1);
	const std::vector<uint8_t> encrypted2 = WalletEncryptionUtil::Encrypt(masterSeed, dataType, associatedData2, plaintext2);

	REQUIRE(WalletEncryptionUtil::Decrypt(masterSeed, dataType, associatedData1, encrypted1) == plaintext1);
	REQUIRE(WalletEncryptionUtil::Decrypt(masterSeed, dataType, associatedData2, encrypted2) == plaintext2);

	// A row swapped with another row of the same type/key must fail authentication.
	REQUIRE_THROWS_AS(
		WalletEncryptionUtil::Decrypt(masterSeed, dataType, associatedData1, encrypted2),
		std::runtime_error
	);
	REQUIRE_THROWS_AS(
		WalletEncryptionUtil::Decrypt(masterSeed, dataType, associatedData2, encrypted1),
		std::runtime_error
	);

	// Tampering with a bound row is detected.
	std::vector<uint8_t> tampered = encrypted1;
	tampered.back() ^= 0x01;
	REQUIRE_THROWS_AS(
		WalletEncryptionUtil::Decrypt(masterSeed, dataType, associatedData1, tampered),
		std::runtime_error
	);

	// Legacy format 1 rows remain readable (dual-read) and are upgraded on write.
	const std::vector<uint8_t> legacy = WalletEncryptionUtil::Encrypt(masterSeed, dataType, plaintext1);
	REQUIRE(WalletEncryptionUtil::Decrypt(masterSeed, dataType, associatedData1, legacy) == plaintext1);
}
