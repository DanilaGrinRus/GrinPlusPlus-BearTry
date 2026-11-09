#include <catch.hpp>

#include <json/json.h>
#include <Crypto/AES256.h>
#include <Crypto/CSPRNG.h>
#include <Crypto/KDF.h>
#include <Wallet/Exceptions/KeyChainException.h>
#include <Wallet/Keychain/SeedEncrypter.h>
#include <Wallet/WalletDB/Models/EncryptedSeed.h>
#include <Wallet/WalletDB/WalletStoreException.h>

#include <string>

TEST_CASE("SeedEncrypter round-trips a supported seed")
{
	const SecureVector seed = CSPRNG::GenerateRandomBytes(32);
	const SecureString password("Password1");

	const EncryptedSeed encrypted = SeedEncrypter::EncryptWalletSeed(seed, password);
	const SecureVector decrypted = SeedEncrypter::DecryptWalletSeed(encrypted, password);

	REQUIRE(decrypted == seed);
}

TEST_CASE("SeedEncrypter rejects a short decrypted seed without UB")
{
	const SecureString password("Password1");
	const ScryptParameters parameters(1024, 8, 1);
	SecureVector saltBytes = CSPRNG::GenerateRandomBytes(8);
	CBigInteger<8> salt(saltBytes.data());
	SecureVector ivBytes = CSPRNG::GenerateRandomBytes(16);
	CBigInteger<16> iv(ivBytes.data());

	const SecretKey key = KDF::PBKDF(password, salt.GetData(), parameters);
	const SecureVector shortPlaintext(15, 0x11);
	std::vector<uint8_t> encrypted = AES256::Encrypt(shortPlaintext, key, iv);

	EncryptedSeed shortSeed(
		CBigInteger<16>(ivBytes.data()),
		CBigInteger<8>(saltBytes.data()),
		std::move(encrypted),
		ScryptParameters(1024, 8, 1)
	);

	try
	{
		SeedEncrypter::DecryptWalletSeed(shortSeed, password);
		FAIL("Expected DecryptWalletSeed to reject a short decrypted seed");
	}
	catch (const KeyChainException& e)
	{
		// Before the fix the reversed iterator range either crashed or was
		// reported as a generic "Failed to decrypt seed." error.
		REQUIRE(std::string(e.what()).find("Decrypted seed length") != std::string::npos);
	}
}

TEST_CASE("EncryptedSeed rejects unbounded scrypt parameters")
{
	Json::Value json;
	json["version"] = 1;
	json["iv"] = std::string(32, '0');
	json["salt"] = std::string(16, '0');
	json["scrypt"]["N"] = 1u << 30; // > 2^20
	json["scrypt"]["r"] = 8;
	json["scrypt"]["p"] = 1;
	json["seed"] = std::string(96, '0');

	REQUIRE_THROWS_AS(EncryptedSeed::FromJSON(json), WalletStoreException);

	// A non-power-of-two N is rejected as well.
	json["scrypt"]["N"] = 30000;
	REQUIRE_THROWS_AS(EncryptedSeed::FromJSON(json), WalletStoreException);

	// A normal wallet seed file still parses.
	json["scrypt"]["N"] = 32768;
	REQUIRE_NOTHROW(EncryptedSeed::FromJSON(json));
}
