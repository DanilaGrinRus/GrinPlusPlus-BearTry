#include "SlateContextTable.h"
#include "../../WalletEncryptionUtil.h"

#include <Common/Util/StringUtil.h>
#include <Common/Logger.h>
#include <Crypto/AES256.h>
#include <Crypto/Hasher.h>
#include <Wallet/WalletDB/WalletStoreException.h>

void SlateContextTable::CreateTable(SqliteDB& database)
{
	std::string table_creation_cmd = "create table slate_contexts(slate_id TEXT PRIMARY KEY, iv BLOB NOT NULL, enc_context BLOB NOT NULL, enc_format INTEGER NOT NULL DEFAULT 0);";
	// TODO: Add indices
	database.Execute(table_creation_cmd);
}

void SlateContextTable::UpdateSchema(SqliteDB& database, const int previousVersion)
{
	if (previousVersion < 2)
	{
		// Legacy <2 had an incompatible/absent slate_contexts table. Contexts are
		// transient negotiation state, not coins; recreating the table is
		// intentional, but the drop must be idempotent for v0 databases.
		database.Execute("DROP TABLE IF EXISTS slate_contexts;");
		CreateTable(database);
	}

	// v5 (W-06): row-bound authenticated slate contexts.
	if (previousVersion < 5 && database.TableExists("slate_contexts") && !database.TableHasColumn("slate_contexts", "enc_format")) {
		database.Execute("ALTER TABLE slate_contexts ADD COLUMN enc_format INTEGER NOT NULL DEFAULT 0;");
	}
}

void SlateContextTable::SaveSlateContext(
	SqliteDB& database,
	const SecureVector& masterSeed,
	const uuids::uuid& slateId,
	const SlateContextEntity& slateContext)
{
	std::string insert_context_cmd = "insert into slate_contexts(slate_id, iv, enc_context, enc_format) values(?, ?, ?, ?)";

	const std::string slateIdStr = uuids::to_string(slateId);
	const std::string dataType = "SLATE_CTX:" + slateIdStr;
	const std::vector<uint8_t> associatedData(slateIdStr.begin(), slateIdStr.end());

	Serializer serializer;
	slateContext.Serialize(serializer);
	const std::vector<uint8_t> encrypted = WalletEncryptionUtil::Encrypt(
		masterSeed, dataType, associatedData, serializer.GetSecureBytes()
	);
	CBigInteger<16> zeroIv;

	std::vector<SqliteDB::IParameter::UPtr> parameters;
	parameters.push_back(TextParameter::New(slateIdStr));
	parameters.push_back(BlobParameter::New(zeroIv.GetData()));
	parameters.push_back(BlobParameter::New(encrypted));
	parameters.push_back(IntParameter::New(1));

	database.Update(insert_context_cmd, parameters);
}

void SlateContextTable::MigrateLegacyRows(SqliteDB& database, const SecureVector& masterSeed)
{
	auto pStatement = database.Query("select slate_id from slate_contexts where enc_format=0");
	std::vector<std::string> legacyRows;
	while (pStatement->Step()) {
		legacyRows.push_back(pStatement->GetColumnString(0));
	}
	pStatement->Finalize();

	for (const std::string& slateIdStr : legacyRows) {
		const auto slateIdOpt = uuids::uuid::from_string(slateIdStr);
		if (!slateIdOpt.has_value()) {
			WALLET_WARNING_F("Skipping legacy slate context with invalid id: {}", slateIdStr);
			continue;
		}

		auto pContext = SlateContextTable::LoadSlateContext(database, masterSeed, slateIdOpt.value());
		if (pContext == nullptr) {
			continue;
		}

		Serializer serializer;
		pContext->Serialize(serializer);
		const std::string dataType = "SLATE_CTX:" + slateIdStr;
		const std::vector<uint8_t> associatedData(slateIdStr.begin(), slateIdStr.end());
		const std::vector<uint8_t> encrypted = WalletEncryptionUtil::Encrypt(
			masterSeed, dataType, associatedData, serializer.GetSecureBytes()
		);

		CBigInteger<16> zeroIv;
		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(BlobParameter::New(zeroIv.GetData()));
		parameters.push_back(BlobParameter::New(encrypted));
		parameters.push_back(TextParameter::New(slateIdStr));
		database.Update(
			"update slate_contexts set iv=?, enc_context=?, enc_format=1 where slate_id=?",
			parameters
		);
	}
}

std::unique_ptr<SlateContextEntity> SlateContextTable::LoadSlateContext(
	SqliteDB& database,
	const SecureVector& masterSeed,
	const uuids::uuid& slateId)
{
	std::string get_context_query = StringUtil::Format("SELECT iv, enc_context, enc_format FROM slate_contexts WHERE slate_id='{}'", uuids::to_string(slateId));
	auto pStatement = database.Query(get_context_query);

	if (!pStatement->Step()) {
		WALLET_INFO_F("Slate context not found for id {}", uuids::to_string(slateId));
		return nullptr;
	}

	WALLET_DEBUG_F("Slate context found for id {}", uuids::to_string(slateId));

	std::vector<uint8_t> iv_bytes = pStatement->GetColumnBytes(0);
	if (iv_bytes.size() != 16) {
		WALLET_ERROR_F("Slate context corrupted: {}", database.GetError());
		throw WALLET_STORE_EXCEPTION("Slate context corrupted.");
	}

	CBigInteger<16> iv(std::move(iv_bytes));
	std::vector<uint8_t> encrypted = pStatement->GetColumnBytes(1);
	const int encFormat = pStatement->GetColumnInt(2);

	if (encFormat == 1) {
		const std::string slateIdStr = uuids::to_string(slateId);
		const std::string dataType = "SLATE_CTX:" + slateIdStr;
		const std::vector<uint8_t> associatedData(slateIdStr.begin(), slateIdStr.end());
		const SecureVector decrypted = WalletEncryptionUtil::Decrypt(masterSeed, dataType, associatedData, encrypted);
		ByteBuffer byteBuffer(std::vector<uint8_t>{ decrypted.begin(), decrypted.end() });
		return std::make_unique<SlateContextEntity>(SlateContextEntity::Deserialize(byteBuffer));
	}

	return std::make_unique<SlateContextEntity>(Decrypt(masterSeed, slateId, iv, encrypted)); // TODO: Pass in keypath and pubkey?
}

std::vector<unsigned char> SlateContextTable::Encrypt(
	const SecureVector& masterSeed,
	const uuids::uuid& slateId,
	const CBigInteger<16>& iv,
	const SlateContextEntity& slateContext)
{
	Serializer serializer;
	slateContext.Serialize(serializer);
	const SecretKey aesKey = DeriveAESKey(masterSeed, slateId);
	std::vector<uint8_t> encrypted = AES256::Encrypt(serializer.GetSecureBytes(), aesKey, iv);

	return encrypted;
}

SlateContextEntity SlateContextTable::Decrypt(
	const SecureVector& masterSeed,
	const uuids::uuid& slateId,
	const CBigInteger<16>& iv,
	const std::vector<unsigned char>& encrypted)
{
	const SecretKey aesKey = DeriveAESKey(masterSeed, slateId);
	SecureVector decrypted = AES256::Decrypt(encrypted, aesKey, iv);

	ByteBuffer byteBuffer(std::vector<unsigned char>{ decrypted.begin(), decrypted.end() });
	return SlateContextEntity::Deserialize(byteBuffer);
}

Hash SlateContextTable::DeriveAESKey(const SecureVector& masterSeed, const uuids::uuid& slateId)
{
	Serializer serializer;
	serializer.AppendByteVector((const std::vector<unsigned char>&)masterSeed);
	serializer.AppendVarStr(uuids::to_string(slateId));
	serializer.AppendVarStr("context");

	return Hasher::Blake2b(serializer.GetBytes());
}