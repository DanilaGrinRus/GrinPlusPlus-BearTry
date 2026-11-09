#include "SlateTable.h"
#include "../../WalletEncryptionUtil.h"
#include "../SqliteDB.h"
#include "../Schema.h"

#include <Common/Util/StringUtil.h>
#include <Crypto/AES256.h>
#include <Crypto/CSPRNG.h>
#include <Crypto/Hasher.h>
#include <Common/Logger.h>
#include <Wallet/WalletDB/WalletStoreException.h>

void SlateTable::CreateTable(SqliteDB& database)
{
	std::string table_creation_cmd = "create table slate(slate_id TEXT NOT NULL, stage TEXT NOT NULL, iv BLOB NOT NULL, slate BLOB NOT NULL, armored_slatepack TEXT, enc_format INTEGER NOT NULL DEFAULT 0);";
	// TODO: Add indices
	database.Execute(table_creation_cmd);
}

void SlateTable::UpdateSchema(SqliteDB& database, const int previousVersion)
{
	if (previousVersion < 2) {
		// v0/v1 may already have a slate table without armored_slatepack. Never
		// issue a conflicting CREATE TABLE; add the missing column instead.
		if (database.TableExists("slate")) {
			if (!database.TableHasColumn("slate", "armored_slatepack")) {
				database.Execute("ALTER TABLE slate ADD armored_slatepack TEXT;");
			}
		} else {
			CreateTable(database);
		}
	} else if (previousVersion < 3) {
		// Some builds stamped a v4-shaped database as version 2 (N-08). In that
		// case the armored_slatepack column already exists and a copy through
		// "new_slate" would silently drop its values. Never lose the column.
		if (database.TableHasColumn("slate", "armored_slatepack")) {
			WALLET_INFO("Slate table already contains armored_slatepack; skipping v2->v3 migration");
		}
		else
		{

		WALLET_INFO("Migrating Slate Table to new schema");

		// Create "new_outputs" table
		std::string table_creation_cmd = "create table new_slate(slate_id TEXT NOT NULL, stage TEXT NOT NULL, iv BLOB NOT NULL, slate BLOB NOT NULL, armored_slatepack TEXT);";
		database.Execute(table_creation_cmd);

		// Load existing slates
		std::string get_slate_query = "SELECT slate_id, stage, iv, slate FROM slate";;
		auto pStatement = database.Query(get_slate_query);

		while (pStatement->Step()) {
			WALLET_DEBUG_F(
				"Migrating slate with id {} at stage {}",
				pStatement->GetColumnString(0),
				pStatement->GetColumnString(1)
			);

			std::string insert_slate_cmd = "insert into new_slate values(?, ?, ?, ?, ?)";

			std::vector<SqliteDB::IParameter::UPtr> parameters;
			parameters.push_back(TextParameter::New(pStatement->GetColumnString(0)));
			parameters.push_back(TextParameter::New(pStatement->GetColumnString(1)));
			parameters.push_back(BlobParameter::New(pStatement->GetColumnBytes(2)));
			parameters.push_back(BlobParameter::New(pStatement->GetColumnBytes(3)));
			parameters.push_back(NullParameter::New());

			database.Update(insert_slate_cmd, parameters);
		}

		// Finalize the read statement before DDL on the same table.
		pStatement->Finalize();

		// Delete existing table
		std::string drop_table_cmd = "DROP TABLE slate";
		database.Execute(drop_table_cmd);

		// Rename "new_slate" table to "slate"
		const std::string rename_table_cmd = "ALTER TABLE new_slate RENAME TO slate";
		database.Execute(rename_table_cmd);
		}
	}

	// v5 (W-06): row-bound authenticated slate records. Existing rows keep the
	// legacy AES-CBC blob (enc_format=0) and remain readable; new writes use
	// format 2 with AAD=slate_id+stage.
	if (previousVersion < 5 && database.TableExists("slate") && !database.TableHasColumn("slate", "enc_format")) {
		database.Execute("ALTER TABLE slate ADD COLUMN enc_format INTEGER NOT NULL DEFAULT 0;");
	}
}

void SlateTable::SaveSlate(
	SqliteDB& database,
	const SecureVector& masterSeed,
	const Slate& slate,
	const std::string& armored_slatepack)
{
	// TODO: Update if it already exists?
	std::string insert_slate_cmd = "insert into slate(slate_id, stage, iv, slate, armored_slatepack, enc_format) values(?, ?, ?, ?, ?, ?)";

	const std::string slateIdStr = uuids::to_string(slate.slateId);
	const std::string stageStr = slate.stage.ToString();
	const std::string dataType = "SLATE:" + slateIdStr + ":" + stageStr;
	const std::string associatedDataStr = slateIdStr + ":" + stageStr;
	const std::vector<uint8_t> associatedData(associatedDataStr.begin(), associatedDataStr.end());

	Serializer serializer;
	slate.Serialize(serializer);
	// W-06 format 2: slate data is bound to its slate id/stage.
	const std::vector<uint8_t> encrypted = WalletEncryptionUtil::Encrypt(
		masterSeed, dataType, associatedData, serializer.GetSecureBytes()
	);

	// The IV is carried inside the format-2 blob; keep a zero placeholder for the
	// legacy column so the schema stays compatible.
	CBigInteger<16> zeroIv;

	std::vector<SqliteDB::IParameter::UPtr> parameters;
	parameters.push_back(TextParameter::New(slateIdStr));
	parameters.push_back(TextParameter::New(stageStr));
	parameters.push_back(BlobParameter::New(zeroIv.GetData()));
	parameters.push_back(BlobParameter::New(encrypted));
	parameters.push_back(armored_slatepack.empty() ? NullParameter::New() : TextParameter::New(armored_slatepack));
	parameters.push_back(IntParameter::New(1));
	
	database.Update(insert_slate_cmd, parameters);
}

void SlateTable::MigrateLegacyRows(SqliteDB& database, const SecureVector& masterSeed)
{
	auto pStatement = database.Query("select slate_id, stage from slate where enc_format=0");
	std::vector<std::pair<std::string, std::string>> legacyRows;
	while (pStatement->Step()) {
		legacyRows.emplace_back(pStatement->GetColumnString(0), pStatement->GetColumnString(1));
	}
	pStatement->Finalize();

	for (const auto& row : legacyRows) {
		const auto slateIdOpt = uuids::uuid::from_string(row.first);
		if (!slateIdOpt.has_value()) {
			WALLET_WARNING_F("Skipping legacy slate with invalid id: {}", row.first);
			continue;
		}

		const uuids::uuid slateId = slateIdOpt.value();
		const SlateStage stage = SlateStage::FromString(row.second);
		auto pSlate = SlateTable::LoadSlate(database, masterSeed, slateId, stage);
		if (pSlate == nullptr) {
			continue;
		}

		Serializer serializer;
		pSlate->Serialize(serializer);
		const std::string dataType = "SLATE:" + row.first + ":" + row.second;
		const std::string associatedDataStr = row.first + ":" + row.second;
		const std::vector<uint8_t> associatedData(associatedDataStr.begin(), associatedDataStr.end());
		const std::vector<uint8_t> encrypted = WalletEncryptionUtil::Encrypt(
			masterSeed, dataType, associatedData, serializer.GetSecureBytes()
		);

		CBigInteger<16> zeroIv;
		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(BlobParameter::New(zeroIv.GetData()));
		parameters.push_back(BlobParameter::New(encrypted));
		parameters.push_back(TextParameter::New(row.first));
		parameters.push_back(TextParameter::New(row.second));
		database.Update(
			"update slate set iv=?, slate=?, enc_format=1 where slate_id=? and stage=?",
			parameters
		);
	}
}

std::unique_ptr<Slate> SlateTable::LoadSlate(
	SqliteDB& database,
	const SecureVector& masterSeed,
	const uuids::uuid& slateId,
	const SlateStage& stage)
{
	std::string get_slate_query = StringUtil::Format("SELECT iv, slate, enc_format FROM slate WHERE slate_id='{}' and stage='{}'", uuids::to_string(slateId), stage.ToString());
	auto pStatement = database.Query(get_slate_query);

	if (!pStatement->Step()) {
		WALLET_INFO_F("Slate not found for id {}", uuids::to_string(slateId));
		return nullptr;
	}

	WALLET_TRACE_F("Slate found for id {}", uuids::to_string(slateId));

	std::vector<uint8_t> iv_bytes = pStatement->GetColumnBytes(0);
	if (iv_bytes.size() != 16) {
		WALLET_ERROR_F("Slate corrupted: {}", database.GetError());
		throw WALLET_STORE_EXCEPTION("Slate corrupted.");
	}

	CBigInteger<16> iv(std::move(iv_bytes));
	const int encFormat = pStatement->GetColumnInt(2);

	try
	{
		std::vector<uint8_t> encrypted = pStatement->GetColumnBytes(1);
		if (encFormat == 1) {
			const std::string slateIdStr = uuids::to_string(slateId);
			const std::string stageStr = stage.ToString();
			const std::string dataType = "SLATE:" + slateIdStr + ":" + stageStr;
			const std::string associatedDataStr = slateIdStr + ":" + stageStr;
			const std::vector<uint8_t> associatedData(associatedDataStr.begin(), associatedDataStr.end());
			const SecureVector decrypted = WalletEncryptionUtil::Decrypt(masterSeed, dataType, associatedData, encrypted);
			ByteBuffer byteBuffer(std::vector<uint8_t>{ decrypted.begin(), decrypted.end() });
			return std::make_unique<Slate>(Slate::Deserialize(byteBuffer));
		}

		return std::make_unique<Slate>(Decrypt(masterSeed, slateId, iv, encrypted));
	}
	catch (std::exception& e)
	{
		WALLET_ERROR_F("Failed to load slate {}: {}", uuids::to_string(slateId), e);
		throw WALLET_STORE_EXCEPTION("Failed to load slate");
	}
}

std::pair<std::unique_ptr<Slate>, std::string> SlateTable::LoadLatestSlate(
	SqliteDB& database,
	const SecureVector& masterSeed,
	const uuids::uuid& slateId)
{
	std::string get_slate_query = StringUtil::Format("SELECT stage, iv, slate, armored_slatepack, enc_format FROM slate WHERE slate_id='{}'", uuids::to_string(slateId));
	auto pStatement = database.Query(get_slate_query);

	SlateStage highest_stage = ESlateStage::NONE;
	std::pair<std::unique_ptr<Slate>, std::string> latest = { nullptr, "" };
	while (pStatement->Step()) {
		WALLET_TRACE_F(
			"Slate found for id {} with stage {}",
			uuids::to_string(slateId),
			pStatement->GetColumnString(0)
		);

		SlateStage stage = SlateStage::FromString(pStatement->GetColumnString(0));
		if (stage < highest_stage) {
			continue;
		}

		std::vector<uint8_t> iv_bytes = pStatement->GetColumnBytes(1);
		if (iv_bytes.size() != 16) {
			WALLET_ERROR_F("Slate corrupted: {}", database.GetError());
			continue;
		}

		try {
			CBigInteger<16> iv(std::move(iv_bytes));
			std::vector<uint8_t> encrypted = pStatement->GetColumnBytes(2);
			std::string armored_slatepack;
			if (!pStatement->IsColumnNull(3)) {
				armored_slatepack = pStatement->GetColumnString(3);
			}

			const int encFormat = pStatement->GetColumnInt(4);
			if (encFormat == 1) {
				const std::string slateIdStr = uuids::to_string(slateId);
				const std::string stageStr = stage.ToString();
				const std::string dataType = "SLATE:" + slateIdStr + ":" + stageStr;
				const std::string associatedDataStr = slateIdStr + ":" + stageStr;
				const std::vector<uint8_t> associatedData(associatedDataStr.begin(), associatedDataStr.end());
				const SecureVector decrypted = WalletEncryptionUtil::Decrypt(masterSeed, dataType, associatedData, encrypted);
				ByteBuffer byteBuffer(std::vector<uint8_t>{ decrypted.begin(), decrypted.end() });
				latest.first = std::make_unique<Slate>(Slate::Deserialize(byteBuffer));
			} else {
				latest.first = std::make_unique<Slate>(Decrypt(masterSeed, slateId, iv, encrypted));
			}
			latest.second = armored_slatepack;

			highest_stage = stage;
		} catch (std::exception& e) {
			WALLET_ERROR_F("Failed to load slate {}: {}", uuids::to_string(slateId), e);
		}
	}

	return latest;
}

std::string SlateTable::LoadArmoredSlatepack(
	SqliteDB& database,
	const uuids::uuid& slateId)
{
	std::string get_slate_query = StringUtil::Format("SELECT stage, armored_slatepack FROM slate WHERE slate_id='{}' and armored_slatepack IS NOT NULL", uuids::to_string(slateId));
	auto pStatement = database.Query(get_slate_query);

	SlateStage highest_stage = ESlateStage::NONE;
	std::string armored_slatepack;
	while (pStatement->Step()) {
		WALLET_DEBUG_F(
			"Slatepack found for id {} with stage {}",
			uuids::to_string(slateId),
			pStatement->GetColumnString(0)
		);

		SlateStage stage = SlateStage::FromString(pStatement->GetColumnString(0));
		if (stage >= highest_stage) {
			armored_slatepack = pStatement->GetColumnString(1);
			highest_stage = stage;
		}
	}

	return armored_slatepack;
}

// Binary serializes a slate, encrypts it with AES256 using a random 16-byte IV, and returns it as: `IV | encrypted_slate`
std::vector<uint8_t> SlateTable::Encrypt(
	const SecureVector& masterSeed,
	const CBigInteger<16>& iv,
	const Slate& slate)
{
	// Encrypt slate
	Serializer serializer;
	slate.Serialize(serializer);
	SecretKey aes_key = DeriveAESKey(masterSeed, slate.slateId);

	return AES256::Encrypt(serializer.GetSecureBytes(), aes_key, iv);
}

Slate SlateTable::Decrypt(
	const SecureVector& masterSeed,
	const uuids::uuid& slateId,
	const CBigInteger<16>& iv,
	const std::vector<uint8_t>& encrypted)
{
	SecretKey aesKey = DeriveAESKey(masterSeed, slateId);
	SecureVector decrypted = AES256::Decrypt(encrypted, aesKey, iv);

	ByteBuffer byteBuffer(std::vector<uint8_t>{ decrypted.data(), decrypted.data() + decrypted.size() });
	return Slate::Deserialize(byteBuffer);
}

Hash SlateTable::DeriveAESKey(const SecureVector& masterSeed, const uuids::uuid& slateId)
{
	Serializer serializer;
	serializer.AppendByteVector((const std::vector<unsigned char>&)masterSeed);
	serializer.AppendVarStr(uuids::to_string(slateId));
	serializer.AppendVarStr("slate");

	return Hasher::Blake2b(serializer.GetBytes());
}