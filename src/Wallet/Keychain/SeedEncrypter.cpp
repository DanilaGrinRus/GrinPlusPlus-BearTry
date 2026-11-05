#include "SeedEncrypter.h"

#include <Wallet/Exceptions/KeyChainException.h>
#include <Crypto/AES256.h>
#include <Crypto/KDF.h>
#include <Crypto/Hasher.h>
#include <Crypto/CSPRNG.h>
#include <Common/Logger.h>

namespace
{
	constexpr size_t SEED_HMAC_SIZE = 32;
	constexpr size_t MIN_SEED_SIZE = 16;
	constexpr size_t MAX_SEED_SIZE = 32;

	bool IsSupportedSeedLength(const size_t size)
	{
		return size >= MIN_SEED_SIZE
			&& size <= MAX_SEED_SIZE
			&& (size % 4) == 0;
	}
}

SecureVector SeedEncrypter::DecryptWalletSeed(const EncryptedSeed& encryptedSeed, const SecureString& password)
{
	try
	{
		WALLET_INFO("Decrypting wallet seed");

		// The scrypt parameters come from the on-disk seed file. Reject
		// attacker-controlled values before calling into crypto_scrypt.
		EncryptedSeed::ValidateScryptParameters(encryptedSeed.GetScryptParameters());

		SecretKey passwordHash = KDF::PBKDF(
			password,
			encryptedSeed.GetSalt().GetData(),
			encryptedSeed.GetScryptParameters()
		);

		const SecureVector decrypted = AES256::Decrypt(
			encryptedSeed.GetEncryptedSeedBytes(),
			passwordHash,
			encryptedSeed.GetIV()
		);

		if (decrypted.size() < MIN_SEED_SIZE + SEED_HMAC_SIZE) {
			WALLET_ERROR_F("Decrypted seed is too short: {} bytes", decrypted.size());
			throw KEYCHAIN_EXCEPTION_F(
				"Decrypted seed length is invalid: {}",
				decrypted.size()
			);
		}

		const size_t seedSize = decrypted.size() - SEED_HMAC_SIZE;
		if (!IsSupportedSeedLength(seedSize)) {
			WALLET_ERROR_F("Decrypted seed has unsupported length: {} bytes", seedSize);
			throw KEYCHAIN_EXCEPTION_F(
				"Decrypted seed length is invalid: {}",
				seedSize
			);
		}

		SecureVector walletSeed(decrypted.begin(), decrypted.begin() + seedSize);

		Hash hash256 = Hasher::HMAC_SHA256(
			walletSeed.data(),
			walletSeed.size(),
			passwordHash.data(),
			passwordHash.size()
		);
		Hash hash256Check(decrypted.data() + walletSeed.size());

		if (hash256 == hash256Check) {
			return walletSeed;
		}
	}
	catch (const KeyChainException&)
	{
		// Preserve precise diagnostics for malformed seed files.
		throw;
	}
	catch (std::exception& e)
	{
		WALLET_ERROR_F("Exception thrown: {}", e.what());
	}

	throw KEYCHAIN_EXCEPTION("Failed to decrypt seed.");
}

EncryptedSeed SeedEncrypter::EncryptWalletSeed(const SecureVector& walletSeed, const SecureString& password)
{
	WALLET_INFO("Encrypting wallet seed");

	if (!IsSupportedSeedLength(walletSeed.size())) {
		WALLET_ERROR_F("Refusing to encrypt seed with unsupported length: {} bytes", walletSeed.size());
		throw KEYCHAIN_EXCEPTION_F(
			"Invalid seed length: {}",
			walletSeed.size()
		);
	}

	CBigInteger<16> iv(CSPRNG::GenerateRandomBytes(16).data());
	CBigInteger<8> salt(CSPRNG::GenerateRandomBytes(8).data());

	ScryptParameters parameters(32768, 8, 1);
	SecretKey passwordHash = KDF::PBKDF(password, salt.GetData(), parameters);

	Hash hash256 = Hasher::HMAC_SHA256(
		walletSeed.data(),
		walletSeed.size(),
		passwordHash.data(),
		passwordHash.size()
	);

	SecureVector seedPlusHash;
	seedPlusHash.insert(seedPlusHash.begin(), walletSeed.cbegin(), walletSeed.cend());
	seedPlusHash.insert(seedPlusHash.end(), hash256.GetData().cbegin(), hash256.GetData().cend());

	std::vector<uint8_t> encrypted = AES256::Encrypt(seedPlusHash, passwordHash, iv);

	return EncryptedSeed(std::move(iv), std::move(salt), std::move(encrypted), std::move(parameters));
}