#pragma once

#include <Crypto/Models/SecretKey.h>
#include <Common/Secure.h>
#include <string>
#include <vector>

class WalletEncryptionUtil
{
public:
	static std::vector<uint8_t> Encrypt(
		const SecureVector& masterSeed,
		const std::string& dataType,
		const SecureVector& bytes
	);

	static SecureVector Decrypt(
		const SecureVector& masterSeed,
		const std::string& dataType,
		const std::vector<uint8_t>& encrypted
	);

	//
	// Format 2: authenticate the row identity as associated data. The MAC covers
	// format | iv | ciphertext | associatedData, so swapped rows of the same
	// type/key fail authentication. Readers still accept legacy format 0/1 rows.
	//
	static std::vector<uint8_t> Encrypt(
		const SecureVector& masterSeed,
		const std::string& dataType,
		const std::vector<uint8_t>& associatedData,
		const SecureVector& bytes
	);

	static SecureVector Decrypt(
		const SecureVector& masterSeed,
		const std::string& dataType,
		const std::vector<uint8_t>& associatedData,
		const std::vector<uint8_t>& encrypted
	);

private:
	static SecretKey CreateSecureKey(const SecureVector& masterSeed, const std::string& dataType);
	static SecretKey CreateMacKey(const SecureVector& masterSeed, const std::string& dataType);
};