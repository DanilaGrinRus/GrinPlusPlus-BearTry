#include "WalletEncryptionUtil.h"

#include <Crypto/AES256.h>
#include <Crypto/Hasher.h>
#include <Crypto/CSPRNG.h>

#include <stdexcept>

static const uint8_t ENCRYPTION_FORMAT = 0;
static const uint8_t AUTHENTICATED_ENCRYPTION_FORMAT = 1;
static const uint8_t ROW_BOUND_ENCRYPTION_FORMAT = 2;

static bool ConstantTimeEqual(const uint8_t* left, const uint8_t* right, const size_t size)
{
	uint8_t difference = 0;
	for (size_t i = 0; i < size; ++i) {
		difference |= static_cast<uint8_t>(left[i] ^ right[i]);
	}
	return difference == 0;
}

std::vector<uint8_t> WalletEncryptionUtil::Encrypt(
	const SecureVector& masterSeed,
	const std::string& dataType,
	const SecureVector& bytes)
{
	const SecureVector randomNumber = CSPRNG::GenerateRandomBytes(16);
	const CBigInteger<16> iv = CBigInteger<16>(randomNumber.data());
	const SecretKey key = WalletEncryptionUtil::CreateSecureKey(masterSeed, dataType);
	const SecretKey macKey = WalletEncryptionUtil::CreateMacKey(masterSeed, dataType);

	const std::vector<uint8_t> encryptedBytes = AES256::Encrypt(bytes, key, iv);

	// Encrypt-then-MAC: authenticate the format byte, IV and ciphertext.
	Serializer authenticated;
	authenticated.Append<uint8_t>(AUTHENTICATED_ENCRYPTION_FORMAT);
	authenticated.AppendBigInteger(iv);
	authenticated.AppendByteVector(encryptedBytes);
	const Hash mac = Hasher::HMAC_SHA256(macKey.GetVec(), authenticated.GetBytes());

	Serializer serializer;
	serializer.Append<uint8_t>(AUTHENTICATED_ENCRYPTION_FORMAT);
	serializer.AppendBigInteger(iv);
	serializer.AppendByteVector(encryptedBytes);
	serializer.AppendBigInteger<32>(mac);
	return serializer.GetBytes();
}

std::vector<uint8_t> WalletEncryptionUtil::Encrypt(
	const SecureVector& masterSeed,
	const std::string& dataType,
	const std::vector<uint8_t>& associatedData,
	const SecureVector& bytes)
{
	const SecureVector randomNumber = CSPRNG::GenerateRandomBytes(16);
	const CBigInteger<16> iv = CBigInteger<16>(randomNumber.data());
	const SecretKey key = WalletEncryptionUtil::CreateSecureKey(masterSeed, dataType);
	const SecretKey macKey = WalletEncryptionUtil::CreateMacKey(masterSeed, dataType);

	const std::vector<uint8_t> encryptedBytes = AES256::Encrypt(bytes, key, iv);

	Serializer authenticated;
	authenticated.Append<uint8_t>(ROW_BOUND_ENCRYPTION_FORMAT);
	authenticated.AppendBigInteger(iv);
	authenticated.AppendByteVector(encryptedBytes);
	authenticated.AppendByteVector(associatedData);
	const Hash mac = Hasher::HMAC_SHA256(macKey.GetVec(), authenticated.GetBytes());

	Serializer serializer;
	serializer.Append<uint8_t>(ROW_BOUND_ENCRYPTION_FORMAT);
	serializer.AppendBigInteger(iv);
	serializer.AppendByteVector(encryptedBytes);
	serializer.AppendBigInteger<32>(mac);
	return serializer.GetBytes();
}

SecureVector WalletEncryptionUtil::Decrypt(
	const SecureVector& masterSeed,
	const std::string& dataType,
	const std::vector<uint8_t>& associatedData,
	const std::vector<uint8_t>& encrypted)
{
	if (encrypted.empty()) {
		throw std::runtime_error("Wallet database row is empty");
	}

	// Legacy/authenticated rows are still readable. They do not bind the row
	// identity; the next write upgrades the row to format 2.
	if (encrypted[0] != ROW_BOUND_ENCRYPTION_FORMAT) {
		return WalletEncryptionUtil::Decrypt(masterSeed, dataType, encrypted);
	}

	ByteBuffer byteBuffer(encrypted);
	const uint8_t formatVersion = byteBuffer.ReadU8();
	(void)formatVersion;
	const CBigInteger<16> iv = byteBuffer.ReadBigInteger<16>();
	const size_t remaining = byteBuffer.GetRemainingSize();
	if (remaining < HASH_SIZE) {
		throw std::runtime_error("Wallet database row is truncated");
	}

	const std::vector<uint8_t> encryptedBytes = byteBuffer.ReadVector(remaining - HASH_SIZE);
	const Hash storedMac = byteBuffer.ReadBigInteger<HASH_SIZE>();
	const SecretKey macKey = WalletEncryptionUtil::CreateMacKey(masterSeed, dataType);

	Serializer authenticated;
	authenticated.Append<uint8_t>(ROW_BOUND_ENCRYPTION_FORMAT);
	authenticated.AppendBigInteger(iv);
	authenticated.AppendByteVector(encryptedBytes);
	authenticated.AppendByteVector(associatedData);
	const Hash expectedMac = Hasher::HMAC_SHA256(macKey.GetVec(), authenticated.GetBytes());

	if (!ConstantTimeEqual(storedMac.GetData().data(), expectedMac.GetData().data(), HASH_SIZE)) {
		throw std::runtime_error("Wallet database authentication failed");
	}

	const SecretKey key = WalletEncryptionUtil::CreateSecureKey(masterSeed, dataType);
	return AES256::Decrypt(encryptedBytes, key, iv);
}

SecureVector WalletEncryptionUtil::Decrypt(
	const SecureVector& masterSeed,
	const std::string& dataType,
	const std::vector<uint8_t>& encrypted)
{
	ByteBuffer byteBuffer(encrypted);

	const uint8_t formatVersion = byteBuffer.ReadU8();
	if (formatVersion == ENCRYPTION_FORMAT) {
		// Legacy rows written before authenticated encryption. Read-only
		// compatibility: they remain decryptable and are re-encrypted as
		// format 1 the next time a row is written.
		const CBigInteger<16> iv = byteBuffer.ReadBigInteger<16>();
		const std::vector<uint8_t> encryptedBytes =
			byteBuffer.ReadVector(byteBuffer.GetRemainingSize());
		const SecretKey key = WalletEncryptionUtil::CreateSecureKey(masterSeed, dataType);
		return AES256::Decrypt(encryptedBytes, key, iv);
	}

	if (formatVersion != AUTHENTICATED_ENCRYPTION_FORMAT) {
		throw std::runtime_error(
			"Unsupported wallet database encryption format: " + std::to_string(formatVersion)
		);
	}

	const CBigInteger<16> iv = byteBuffer.ReadBigInteger<16>();
	const size_t remaining = byteBuffer.GetRemainingSize();
	if (remaining < HASH_SIZE) {
		throw std::runtime_error("Wallet database row is truncated");
	}

	const std::vector<uint8_t> encryptedBytes = byteBuffer.ReadVector(remaining - HASH_SIZE);
	const Hash storedMac = byteBuffer.ReadBigInteger<HASH_SIZE>();
	const SecretKey macKey = WalletEncryptionUtil::CreateMacKey(masterSeed, dataType);

	Serializer authenticated;
	authenticated.Append<uint8_t>(AUTHENTICATED_ENCRYPTION_FORMAT);
	authenticated.AppendBigInteger(iv);
	authenticated.AppendByteVector(encryptedBytes);
	const Hash expectedMac = Hasher::HMAC_SHA256(macKey.GetVec(), authenticated.GetBytes());

	if (!ConstantTimeEqual(storedMac.GetData().data(), expectedMac.GetData().data(), HASH_SIZE)) {
		throw std::runtime_error("Wallet database authentication failed");
	}

	const SecretKey key = WalletEncryptionUtil::CreateSecureKey(masterSeed, dataType);
	return AES256::Decrypt(encryptedBytes, key, iv);
}

SecretKey WalletEncryptionUtil::CreateSecureKey(
	const SecureVector& masterSeed,
	const std::string& dataType)
{
	SecureVector seedWithNonce(masterSeed.cbegin(), masterSeed.cend());

	Serializer nonceSerializer;
	nonceSerializer.AppendVarStr(dataType);
	seedWithNonce.insert(
		seedWithNonce.end(),
		nonceSerializer.GetBytes().begin(),
		nonceSerializer.GetBytes().end()
	);

	return Hasher::Blake2b(seedWithNonce.data(), seedWithNonce.size());
}

SecretKey WalletEncryptionUtil::CreateMacKey(
	const SecureVector& masterSeed,
	const std::string& dataType)
{
	SecureVector seedWithContext(masterSeed.cbegin(), masterSeed.cend());

	Serializer contextSerializer;
	contextSerializer.AppendVarStr("wallet-db-mac-v1");
	contextSerializer.AppendVarStr(dataType);
	seedWithContext.insert(
		seedWithContext.end(),
		contextSerializer.GetBytes().begin(),
		contextSerializer.GetBytes().end()
	);

	return Hasher::Blake2b(seedWithContext.data(), seedWithContext.size());
}
