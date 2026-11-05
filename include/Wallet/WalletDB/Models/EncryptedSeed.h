#pragma once

#include <Crypto/Models/BigInteger.h>
#include <Crypto/Models/ScryptParameters.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Wallet/WalletDB/WalletStoreException.h>

static const uint8_t ENCRYPTED_SEED_FORMAT = 1;

class EncryptedSeed
{
public:
	EncryptedSeed(CBigInteger<16>&& iv, CBigInteger<8>&& salt, std::vector<uint8_t>&& encryptedSeedBytes, ScryptParameters&& scryptParameters)
		: m_iv(std::move(iv)), m_salt(std::move(salt)), m_encryptedSeedBytes(std::move(encryptedSeedBytes)), m_scryptParameters(std::move(scryptParameters))
	{
		ValidateScryptParameters(m_scryptParameters);
	}

	// scrypt parameters are read from an on-disk file. Bound them so a
	// malformed/tampered seed.json cannot request an unbounded allocation or
	// an unreasonable KDF workload.
	static void ValidateScryptParameters(const ScryptParameters& parameters)
	{
		const uint64_t N = parameters.N;
		const uint64_t r = parameters.r;
		const uint64_t p = parameters.p;

		if (N < 1024 || N > (1ull << 20) || (N & (N - 1)) != 0) {
			throw WALLET_STORE_EXCEPTION_F("Invalid scrypt parameter N: {}", N);
		}

		if (r < 1 || r > 32) {
			throw WALLET_STORE_EXCEPTION_F("Invalid scrypt parameter r: {}", r);
		}

		if (p < 1 || p > 16) {
			throw WALLET_STORE_EXCEPTION_F("Invalid scrypt parameter p: {}", p);
		}

		// 128 * N * r bytes is the scrypt working memory.
		constexpr uint64_t MAX_SCRYPT_MEMORY = 512ull * 1024 * 1024;
		if (N > MAX_SCRYPT_MEMORY / (128ull * r)) {
			throw WALLET_STORE_EXCEPTION_F(
				"scrypt parameters require too much memory: N={}, r={}, p={}",
				N,
				r,
				p
			);
		}
	}

	const CBigInteger<16>& GetIV() const { return m_iv; }
	const CBigInteger<8>& GetSalt() const { return m_salt; }
	const std::vector<unsigned char>& GetEncryptedSeedBytes() const { return m_encryptedSeedBytes; }
	const ScryptParameters& GetScryptParameters() const { return m_scryptParameters; }

	void Serialize(Serializer& serializer) const
	{
		serializer.Append<uint8_t>(ENCRYPTED_SEED_FORMAT);
		serializer.AppendBigInteger<16>(m_iv);
		serializer.AppendBigInteger<8>(m_salt);
		serializer.Append<uint32_t>(m_scryptParameters.N);
		serializer.Append<uint32_t>(m_scryptParameters.r);
		serializer.Append<uint32_t>(m_scryptParameters.p);
		serializer.AppendByteVector(m_encryptedSeedBytes);
	}

	Json::Value ToJSON() const
	{
		Json::Value json;
		json["version"] = ENCRYPTED_SEED_FORMAT;
		json["iv"] = m_iv.ToHex();
		json["salt"] = m_salt.ToHex();
		json["scrypt"] = m_scryptParameters.ToJSON();
		json["seed"] = HexUtil::ConvertToHex(m_encryptedSeedBytes);
		return json;
	}

	static EncryptedSeed Deserialize(ByteBuffer& byteBuffer)
	{
		const uint8_t version = byteBuffer.ReadU8();

		CBigInteger<16> iv = byteBuffer.ReadBigInteger<16>();
		CBigInteger<8> salt = byteBuffer.ReadBigInteger<8>();

		ScryptParameters parameters(131072, 16, 2);
		if (version >= 1)
		{
			const uint32_t N = byteBuffer.ReadU32();
			const uint32_t r = byteBuffer.ReadU32();
			const uint32_t p = byteBuffer.ReadU32();
			parameters = ScryptParameters(N, r, p);
		}

		std::vector<unsigned char> encryptedSeedBytes = byteBuffer.ReadVector(byteBuffer.GetRemainingSize());

		return EncryptedSeed(std::move(iv), std::move(salt), std::move(encryptedSeedBytes), std::move(parameters));
	}

	static EncryptedSeed FromJSON(const Json::Value& json)
	{
		CBigInteger<16> iv = CBigInteger<16>::FromHex(JsonUtil::GetRequiredField(json, "iv").asString());
		CBigInteger<8> salt = CBigInteger<8>::FromHex(JsonUtil::GetRequiredField(json, "salt").asString());

		Json::Value scryptJSON = JsonUtil::GetRequiredField(json, "scrypt");
		ScryptParameters parameters = ScryptParameters::FromJSON(scryptJSON);

		std::vector<unsigned char> encryptedSeedBytes = HexUtil::FromHex(JsonUtil::GetRequiredField(json, "seed").asString());

		return EncryptedSeed(std::move(iv), std::move(salt), std::move(encryptedSeedBytes), std::move(parameters));
	}

private:
	CBigInteger<16> m_iv;
	CBigInteger<8> m_salt;
	std::vector<unsigned char> m_encryptedSeedBytes;
	ScryptParameters m_scryptParameters;
};