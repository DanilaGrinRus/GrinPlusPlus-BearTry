#include <catch.hpp>

#include <TestFileUtil.h>
#include <Crypto/CSPRNG.h>
#include <Crypto/AES256.h>
#include <Crypto/Hasher.h>
#include <Wallet/WalletDB/Sqlite/SqliteDB.h>
#include <Wallet/WalletDB/Sqlite/SqliteStore.h>
#include <Wallet/WalletDB/WalletEncryptionUtil.h>
#include <Wallet/WalletDB/Sqlite/Schema.h>
#include <Wallet/WalletTx.h>
#include <chrono>
#include <Wallet/WalletDB/Sqlite/Tables/AccountsTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/MetadataTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/OutputsTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/SlateContextTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/SlateTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/TransactionsTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/VersionTable.h>
#include <Wallet/WalletDB/WalletStoreException.h>
#include <Wallet/WalletDB/Models/EncryptedSeed.h>
#include <Wallet/WalletDB/Sqlite/SqliteTransaction.h>
#include <Wallet/Keychain/KeyChain.h>
#include <Common/Util/FileUtil.h>
#include <Core/Config.h>
#include <Core/Enums/Environment.h>
#include <Crypto/Crypto.h>
#include <filesystem.h>

#include <algorithm>

namespace
{
	SqliteDB::Ptr OpenWalletDB(const TemporaryFile::Ptr& pTempFile)
	{
		return SqliteDB::Open(pTempFile->GetPath(), "test");
	}

	void CreateCurrentSchema(SqliteDB& database)
	{
		VersionTable::CreateTable(database);
		OutputsTable::CreateTable(database);
		TransactionsTable::CreateTable(database);
		MetadataTable::CreateTable(database);
		SlateContextTable::CreateTable(database);
		SlateTable::CreateTable(database);
		AccountsTable::CreateTable(database);
	}

	std::vector<uint8_t> LegacyFormat0Encrypt(
		const SecureVector& masterSeed,
		const std::string& dataType,
		const SecureVector& plaintext)
	{
		SecureVector seedWithNonce(masterSeed.cbegin(), masterSeed.cend());
		Serializer nonceSerializer;
		nonceSerializer.AppendVarStr(dataType);
		seedWithNonce.insert(seedWithNonce.end(), nonceSerializer.GetBytes().begin(), nonceSerializer.GetBytes().end());
		const SecretKey key = Hasher::Blake2b(seedWithNonce.data(), seedWithNonce.size());

		const SecureVector random = CSPRNG::GenerateRandomBytes(16);
		const CBigInteger<16> iv(random.data());
		const std::vector<uint8_t> encrypted = AES256::Encrypt(plaintext, key, iv);

		Serializer serializer;
		serializer.Append<uint8_t>(0);
		serializer.AppendBigInteger(iv);
		serializer.AppendByteVector(encrypted);
		return serializer.GetBytes();
	}

	std::vector<uint8_t> LegacySlateEncrypt(
		const SecureVector& masterSeed,
		const uuids::uuid& slateId,
		const CBigInteger<16>& iv,
		const SecureVector& plaintext)
	{
		Serializer keySerializer;
		keySerializer.AppendByteVector((const std::vector<unsigned char>&)masterSeed);
		keySerializer.AppendVarStr(uuids::to_string(slateId));
		keySerializer.AppendVarStr("slate");
		const SecretKey key = Hasher::Blake2b(keySerializer.GetBytes());
		return AES256::Encrypt(plaintext, key, iv);
	}

	void InsertSlate(SqliteDB& database, const std::string& slateId, const std::string& armoredSlatepack)
	{
		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(TextParameter::New(slateId));
		parameters.push_back(TextParameter::New("STANDARD"));
		parameters.push_back(BlobParameter::New(std::vector<uint8_t>(16, 0)));
		parameters.push_back(BlobParameter::New(std::vector<uint8_t>{ 1, 2, 3 }));
		parameters.push_back(TextParameter::New(armoredSlatepack));

		database.Update(
			"insert into slate(slate_id, stage, iv, slate, armored_slatepack) values(?, ?, ?, ?, ?)",
			parameters
		);
	}

	//
	// Legacy format 0 output rows (written before the authenticated-format
	// migration) carry no MAC, but their payload must survive the sweep byte for
	// byte. These helpers build a real output entity so the serialized blob is
	// the same shape the wallet DB stores.
	//
	OutputDataEntity MakeSpendableOutputEntity(const uint32_t walletTxId)
	{
		const CBigInteger<32> seed = CSPRNG::GenerateRandom32();
		const SecureVector seedBytes(seed.GetData().begin(), seed.GetData().end());
		const KeyChain keyChain = KeyChain::FromSeed(seedBytes);

		const KeyChainPath keyChainPath(std::vector<uint32_t>({ 1, 2, 3 }));
		const uint64_t amount = 7000000000ull;
		const SecretKey blindingFactor = keyChain.DerivePrivateKey(keyChainPath, amount);
		const Commitment commitment = Crypto::CommitBlinded(amount, BlindingFactor(blindingFactor.GetBytes()));
		const RangeProof rangeProof = keyChain.GenerateRangeProof(
			keyChainPath,
			amount,
			commitment,
			blindingFactor,
			EBulletproofType::ENHANCED
		);
		const TransactionOutput output(EOutputFeatures::DEFAULT, commitment, rangeProof);

		return OutputDataEntity(
			keyChainPath,
			blindingFactor,
			output,
			amount,
			EOutputStatus::SPENDABLE,
			std::make_optional<uint64_t>(11),
			std::make_optional<uint64_t>(500),
			std::make_optional<uint32_t>(walletTxId),
			std::vector<std::string>{}
		);
	}

	void InsertLegacyOutput(
		SqliteDB& database,
		const SecureVector& masterSeed,
		const OutputDataEntity& entity)
	{
		Serializer serializer;
		entity.Serialize(serializer);
		const std::vector<uint8_t> legacy = LegacyFormat0Encrypt(
			masterSeed, "OUTPUT", serializer.GetSecureBytes()
		);

		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(TextParameter::New(entity.GetOutput().GetCommitment().ToHex()));
		parameters.push_back(IntParameter::New((int)entity.GetStatus()));
		parameters.push_back(IntParameter::New((int)entity.GetWalletTxId().value()));
		parameters.push_back(BlobParameter::New(legacy));
		database.Update(
			"insert into outputs(commitment, status, transaction_id, encrypted) values(?, ?, ?, ?)",
			parameters
		);
	}

	void InsertLegacyTransaction(
		SqliteDB& database,
		const SecureVector& masterSeed,
		const uint32_t walletTxId)
	{
		WalletTx tx(
			walletTxId,
			EWalletTxType::RECEIVED,
			std::nullopt,
			std::nullopt,
			std::nullopt,
			std::chrono::system_clock::now(),
			std::nullopt,
			std::nullopt,
			2500000000ull,
			0,
			std::nullopt,
			std::nullopt,
			std::nullopt
		);
		Serializer serializer;
		tx.Serialize(serializer);
		const std::vector<uint8_t> legacy = LegacyFormat0Encrypt(
			masterSeed, "WALLET_TX", serializer.GetSecureBytes()
		);

		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(IntParameter::New((int)walletTxId));
		parameters.push_back(NullParameter::New());
		parameters.push_back(BlobParameter::New(legacy));
		database.Update(
			"insert into transactions(id, slate_id, encrypted) values(?, ?, ?)",
			parameters
		);
	}

	SlateContextEntity MakeSlateContextEntity()
	{
		return SlateContextEntity(
			SecretKey(CSPRNG::GenerateRandomBytes(32)),
			SecretKey(CSPRNG::GenerateRandomBytes(32)),
			std::vector<OutputDataEntity>{},
			std::vector<OutputDataEntity>{}
		);
	}

	// Legacy slate contexts used their own AES-CBC key derivation:
	// blake2b(master_seed | varstr(slate_id) | varstr("context")) with no MAC and
	// no format byte. enc_format defaults to 0 for such rows.
	void InsertLegacySlateContext(
		SqliteDB& database,
		const SecureVector& masterSeed,
		const uuids::uuid& slateId,
		const SlateContextEntity& context)
	{
		Serializer keySerializer;
		keySerializer.AppendByteVector((const std::vector<unsigned char>&)masterSeed);
		keySerializer.AppendVarStr(uuids::to_string(slateId));
		keySerializer.AppendVarStr("context");
		const SecretKey key = Hasher::Blake2b(keySerializer.GetBytes());

		const SecureVector ivBytes = CSPRNG::GenerateRandomBytes(16);
		const CBigInteger<16> iv(ivBytes.data());

		Serializer serializer;
		context.Serialize(serializer);
		const std::vector<uint8_t> encrypted = AES256::Encrypt(serializer.GetSecureBytes(), key, iv);

		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(TextParameter::New(uuids::to_string(slateId)));
		parameters.push_back(BlobParameter::New(iv.GetData()));
		parameters.push_back(BlobParameter::New(encrypted));
		database.Update(
			"insert into slate_contexts(slate_id, iv, enc_context) values(?, ?, ?)",
			parameters
		);
	}

	// W-06 sweep backups are named wallet.db.pre-format2-<unix-time>.bak.
	std::vector<fs::path> FindMigrationBackups(const fs::path& walletDirectory)
	{
		std::vector<fs::path> backups;
		std::error_code error;
		for (const fs::directory_entry& entry : fs::directory_iterator(walletDirectory, error)) {
			if (error) {
				break;
			}

			const std::string name = entry.path().filename().string();
			if (name.find("wallet.db.pre-format2-") == 0) {
				backups.push_back(entry.path());
			}
		}

		std::sort(backups.begin(), backups.end());
		return backups;
	}

	EncryptedSeed MakeTestSeed()
	{
		const SecureVector iv = CSPRNG::GenerateRandomBytes(16);
		const SecureVector salt = CSPRNG::GenerateRandomBytes(8);
		return EncryptedSeed(
			CBigInteger<16>(iv.data()),
			CBigInteger<8>(salt.data()),
			std::vector<uint8_t>(48, 0),
			ScryptParameters(1024, 8, 1)
		);
	}

	std::string UniqueUsername()
	{
		return "w06-" + uuids::to_string(uuids::uuid_system_generator()());
	}
}

TEST_CASE("New wallet database is stamped with LATEST_SCHEMA_VERSION")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);

	// Before N-08 the version table claimed 2 while every table was created
	// with the v4 shape.
	REQUIRE(VersionTable::GetCurrentVersion(*pDatabase) == LATEST_SCHEMA_VERSION);
	REQUIRE_NOTHROW(VersionTable::VerifySchema(*pDatabase));
}

TEST_CASE("v2-stamped v4 database keeps armored_slatepack during migration")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);
	InsertSlate(*pDatabase, "slate-1", "armored-data");

	// Simulate the N-08 state: full v4 schema, stale version row.
	pDatabase->Execute("update version set schema_version=2;");

	REQUIRE_NOTHROW(SlateTable::UpdateSchema(*pDatabase, 2));

	auto pStatement = pDatabase->Query("select armored_slatepack from slate where slate_id='slate-1'");
	REQUIRE(pStatement->Step());
	REQUIRE_FALSE(pStatement->IsColumnNull(0));
	REQUIRE(pStatement->GetColumnString(0) == "armored-data");
	pStatement.reset();

	// Full migration on the same stale-version database must only fix the
	// version marker: the schema is already complete.
	VersionTable::Migrate(*pDatabase, CSPRNG::GenerateRandomBytes(32), 2);
	REQUIRE(VersionTable::GetCurrentVersion(*pDatabase) == LATEST_SCHEMA_VERSION);
	REQUIRE_NOTHROW(VersionTable::VerifySchema(*pDatabase));

	auto pAfterMigrate = pDatabase->Query("select armored_slatepack from slate where slate_id='slate-1'");
	REQUIRE(pAfterMigrate->Step());
	REQUIRE_FALSE(pAfterMigrate->IsColumnNull(0));
	REQUIRE(pAfterMigrate->GetColumnString(0) == "armored-data");
}

TEST_CASE("v4 wallet database migrates to v5 row-bound encryption schema")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);

	// v4-shaped schema (without enc_format columns).
	pDatabase->Execute(
		"create table version(schema_version INTEGER PRIMARY KEY); insert into version(schema_version) values(4);"
		"create table outputs(id INTEGER PRIMARY KEY, commitment TEXT UNIQUE NOT NULL, status INTEGER NOT NULL, transaction_id INTEGER, encrypted BLOB NOT NULL);"
		"create table transactions(id INTEGER PRIMARY KEY, slate_id TEXT, encrypted BLOB NOT NULL);"
		"create table metadata(id INTEGER PRIMARY KEY, next_tx_id INTEGER NOT NULL, refresh_block_height INTEGER NOT NULL, restore_leaf_index INTEGER NOT NULL);"
		"create table slate_contexts(slate_id TEXT PRIMARY KEY, iv BLOB NOT NULL, enc_context BLOB NOT NULL);"
		"create table slate(slate_id TEXT NOT NULL, stage TEXT NOT NULL, iv BLOB NOT NULL, slate BLOB NOT NULL, armored_slatepack TEXT);"
		"create table accounts(parent_path TEXT PRIMARY KEY, account_name TEXT NOT NULL, next_child_index INTEGER NOT NULL, current_address_index INTEGER DEFAULT 0 NOT NULL);"
	);
	InsertSlate(*pDatabase, "legacy-slate", "legacy-armored");

	VersionTable::Migrate(*pDatabase, CSPRNG::GenerateRandomBytes(32), 4);

	REQUIRE(VersionTable::GetCurrentVersion(*pDatabase) == LATEST_SCHEMA_VERSION);
	REQUIRE_NOTHROW(VersionTable::VerifySchema(*pDatabase));
	REQUIRE(pDatabase->TableHasColumn("slate", "enc_format"));
	REQUIRE(pDatabase->TableHasColumn("slate_contexts", "enc_format"));

	// Legacy rows are preserved and explicitly marked as format 0.
	auto pStatement = pDatabase->Query("select armored_slatepack, enc_format from slate where slate_id='legacy-slate'");
	REQUIRE(pStatement->Step());
	REQUIRE(pStatement->GetColumnString(0) == "legacy-armored");
	REQUIRE(pStatement->GetColumnInt(1) == 0);
}

TEST_CASE("W-06 legacy transaction rows are swept to format 2")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);

	WalletTx tx(
		7,
		EWalletTxType::SENT,
		std::nullopt,
		std::nullopt,
		std::nullopt,
		std::chrono::system_clock::now(),
		std::nullopt,
		std::nullopt,
		0,
		0,
		std::nullopt,
		std::nullopt,
		std::nullopt
	);
	Serializer serializer;
	tx.Serialize(serializer);
	const std::vector<uint8_t> legacy = WalletEncryptionUtil::Encrypt(masterSeed, "WALLET_TX", serializer.GetSecureBytes());

	std::vector<SqliteDB::IParameter::UPtr> parameters;
	parameters.push_back(IntParameter::New(7));
	parameters.push_back(NullParameter::New());
	parameters.push_back(BlobParameter::New(legacy));
	pDatabase->Update("insert into transactions(id, slate_id, encrypted) values(?, ?, ?)", parameters);

	REQUIRE(SqliteStore::HasLegacyEncryption(*pDatabase));
	REQUIRE(SqliteStore::MigrateLegacyEncryption(*pDatabase, masterSeed));
	REQUIRE_FALSE(SqliteStore::HasLegacyEncryption(*pDatabase));

	auto pStatement = pDatabase->Query("select encrypted from transactions where id=7");
	REQUIRE(pStatement->Step());
	const std::vector<uint8_t> migrated = pStatement->GetColumnBytes(0);
	REQUIRE_FALSE(migrated.empty());
	REQUIRE(migrated[0] == 2);

	const auto transactions = TransactionsTable::GetTransactions(*pDatabase, masterSeed);
	REQUIRE(transactions.size() == 1);
	REQUIRE(transactions[0].GetId() == 7);
}

TEST_CASE("W-06 legacy format 0 transaction rows are swept")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);

	WalletTx tx(
		8,
		EWalletTxType::RECEIVED,
		std::nullopt,
		std::nullopt,
		std::nullopt,
		std::chrono::system_clock::now(),
		std::nullopt,
		std::nullopt,
		100,
		0,
		std::nullopt,
		std::nullopt,
		std::nullopt
	);
	Serializer serializer;
	tx.Serialize(serializer);
	const std::vector<uint8_t> legacy = LegacyFormat0Encrypt(masterSeed, "WALLET_TX", serializer.GetSecureBytes());

	std::vector<SqliteDB::IParameter::UPtr> parameters;
	parameters.push_back(IntParameter::New(8));
	parameters.push_back(NullParameter::New());
	parameters.push_back(BlobParameter::New(legacy));
	pDatabase->Update("insert into transactions(id, slate_id, encrypted) values(?, ?, ?)", parameters);

	REQUIRE(SqliteStore::HasLegacyEncryption(*pDatabase));
	REQUIRE(SqliteStore::MigrateLegacyEncryption(*pDatabase, masterSeed));

	auto pStatement = pDatabase->Query("select encrypted from transactions where id=8");
	REQUIRE(pStatement->Step());
	const std::vector<uint8_t> migrated = pStatement->GetColumnBytes(0);
	REQUIRE_FALSE(migrated.empty());
	REQUIRE(migrated[0] == 2);

	const auto transactions = TransactionsTable::GetTransactions(*pDatabase, masterSeed);
	REQUIRE(transactions.size() == 1);
	REQUIRE(transactions[0].GetId() == 8);
}

TEST_CASE("W-06 legacy slate rows are swept")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);

	Slate slate;
	Serializer slateSerializer;
	slate.Serialize(slateSerializer);
	const SecureVector ivBytes = CSPRNG::GenerateRandomBytes(16);
	const CBigInteger<16> iv(ivBytes.data());
	const std::vector<uint8_t> legacy = LegacySlateEncrypt(masterSeed, slate.slateId, iv, slateSerializer.GetSecureBytes());

	std::vector<SqliteDB::IParameter::UPtr> parameters;
	parameters.push_back(TextParameter::New(uuids::to_string(slate.slateId)));
	parameters.push_back(TextParameter::New(slate.stage.ToString()));
	parameters.push_back(BlobParameter::New(iv.GetData()));
	parameters.push_back(BlobParameter::New(legacy));
	parameters.push_back(NullParameter::New());
	pDatabase->Update("insert into slate(slate_id, stage, iv, slate, armored_slatepack) values(?, ?, ?, ?, ?)", parameters);

	REQUIRE(SqliteStore::HasLegacyEncryption(*pDatabase));
	REQUIRE(SqliteStore::MigrateLegacyEncryption(*pDatabase, masterSeed));
	REQUIRE_FALSE(SqliteStore::HasLegacyEncryption(*pDatabase));

	auto pStatement = pDatabase->Query("select slate, enc_format from slate where slate_id='" + uuids::to_string(slate.slateId) + "'");
	REQUIRE(pStatement->Step());
	const std::vector<uint8_t> migrated = pStatement->GetColumnBytes(0);
	REQUIRE_FALSE(migrated.empty());
	REQUIRE(migrated[0] == 2);
	REQUIRE(pStatement->GetColumnInt(1) == 1);

	const auto loaded = SlateTable::LoadSlate(*pDatabase, masterSeed, slate.slateId, slate.stage);
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->slateId == slate.slateId);
}

TEST_CASE("W-06 slate rows are bound to their identity")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);

	Slate slate1;
	Slate slate2;
	REQUIRE(slate1.slateId != slate2.slateId);
	SlateTable::SaveSlate(*pDatabase, masterSeed, slate1, "armored-1");
	SlateTable::SaveSlate(*pDatabase, masterSeed, slate2, "armored-2");

	const auto loaded = SlateTable::LoadSlate(*pDatabase, masterSeed, slate1.slateId, slate1.stage);
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->slateId == slate1.slateId);

	const std::string id1 = uuids::to_string(slate1.slateId);
	const std::string id2 = uuids::to_string(slate2.slateId);
	pDatabase->Execute(
		"update slate set slate=(select slate from slate where slate_id='" + id2 + "') where slate_id='" + id1 + "'"
	);

	// The ciphertext is authenticated with AAD=slate_id:stage, so a row swapped
	// from another slate must fail instead of being silently accepted.
	REQUIRE_THROWS_AS(
		SlateTable::LoadSlate(*pDatabase, masterSeed, slate1.slateId, slate1.stage),
		WalletStoreException
	);
}

TEST_CASE("True v2 database migrates to v4 without losing slate or account rows")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);

	// A genuine v2 layout: version row 2, old slate without
	// armored_slatepack, old accounts without current_address_index.
	pDatabase->Execute(
		"create table version(schema_version INTEGER PRIMARY KEY);"
		"insert into version(schema_version) values(2);"
	);
	OutputsTable::CreateTable(*pDatabase);
	TransactionsTable::CreateTable(*pDatabase);
	MetadataTable::CreateTable(*pDatabase);
	SlateContextTable::CreateTable(*pDatabase);
	pDatabase->Execute(
		"create table slate(slate_id TEXT NOT NULL, stage TEXT NOT NULL, iv BLOB NOT NULL, slate BLOB NOT NULL);"
	);
	pDatabase->Execute(
		"create table accounts(parent_path TEXT PRIMARY KEY, account_name TEXT NOT NULL, next_child_index INTEGER NOT NULL);"
	);
	pDatabase->Execute("insert into accounts values('m/0/0', 'DEFAULT', 0);");
	std::vector<SqliteDB::IParameter::UPtr> oldSlateParameters;
	oldSlateParameters.push_back(TextParameter::New("old-slate"));
	oldSlateParameters.push_back(TextParameter::New("STANDARD"));
	oldSlateParameters.push_back(BlobParameter::New(std::vector<uint8_t>(16, 0)));
	oldSlateParameters.push_back(BlobParameter::New(std::vector<uint8_t>{ 4, 5, 6 }));
	pDatabase->Update(
		"insert into slate(slate_id, stage, iv, slate) values(?, ?, ?, ?)",
		oldSlateParameters
	);

	VersionTable::Migrate(*pDatabase, CSPRNG::GenerateRandomBytes(32), 2);

	REQUIRE(VersionTable::GetCurrentVersion(*pDatabase) == LATEST_SCHEMA_VERSION);
	REQUIRE(pDatabase->TableHasColumn("slate", "armored_slatepack"));
	REQUIRE(pDatabase->TableHasColumn("accounts", "current_address_index"));
	REQUIRE_NOTHROW(VersionTable::VerifySchema(*pDatabase));

	// Slate payload survived; armored slatepack is legitimately empty for a
	// row that was written before the column existed.
	auto pSlate = pDatabase->Query("select stage, slate, armored_slatepack from slate where slate_id='old-slate'");
	REQUIRE(pSlate->Step());
	REQUIRE(pSlate->GetColumnString(0) == "STANDARD");
	REQUIRE((pSlate->GetColumnBytes(1) == std::vector<uint8_t>{ 4, 5, 6 }));
	REQUIRE(pSlate->IsColumnNull(2));

	auto pAccount = pDatabase->Query(
		"select account_name, next_child_index, current_address_index from accounts where parent_path='m/0/0'"
	);
	REQUIRE(pAccount->Step());
	REQUIRE(pAccount->GetColumnString(0) == "DEFAULT");
	REQUIRE(pAccount->GetColumnInt(1) == 0);
	REQUIRE(pAccount->GetColumnInt(2) == 0);
}

TEST_CASE("Incomplete schema is never stamped as LATEST_SCHEMA_VERSION")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);
	pDatabase->Execute("update version set schema_version=2; drop table transactions;");

	REQUIRE_THROWS_AS(
		VersionTable::Migrate(*pDatabase, CSPRNG::GenerateRandomBytes(32), 2),
		WalletStoreException
	);

	// The version row must stay at 2: the database must not advertise v4.
	REQUIRE(VersionTable::GetCurrentVersion(*pDatabase) == 2);
}

//
// W-06 full AEAD migration: the remaining gaps of the original sweep coverage
// are legacy format 0 OUTPUT rows, legacy slate contexts, and the real
// SqliteStore::OpenWallet path (backup, idempotency, rollback, tamper).
//
TEST_CASE("W-06 legacy format 0 output rows are swept and preserve committed data")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);

	const OutputDataEntity original = MakeSpendableOutputEntity(21);
	InsertLegacyOutput(*pDatabase, masterSeed, original);

	REQUIRE(SqliteStore::HasLegacyEncryption(*pDatabase));
	REQUIRE(SqliteStore::MigrateLegacyEncryption(*pDatabase, masterSeed));
	REQUIRE_FALSE(SqliteStore::HasLegacyEncryption(*pDatabase));

	const std::string commitmentHex = original.GetOutput().GetCommitment().ToHex();
	{
		auto pStatement = pDatabase->Query(
			"select encrypted from outputs where commitment='" + commitmentHex + "'"
		);
		REQUIRE(pStatement->Step());
		const std::vector<uint8_t> migrated = pStatement->GetColumnBytes(0);
		REQUIRE_FALSE(migrated.empty());
		REQUIRE(migrated[0] == 2);
	}

	const std::vector<OutputDataEntity> outputs = OutputsTable::GetOutputs(*pDatabase, masterSeed);
	REQUIRE(outputs.size() == 1);
	REQUIRE(outputs[0].GetCommitment() == original.GetCommitment());
	REQUIRE(outputs[0].GetAmount() == original.GetAmount());
	REQUIRE(outputs[0].GetStatus() == EOutputStatus::SPENDABLE);
	REQUIRE(outputs[0].GetMMRIndex().value() == 11);
	REQUIRE(outputs[0].GetBlockHeight().value() == 500);
	REQUIRE(outputs[0].GetWalletTxId().value() == 21);
	REQUIRE(outputs[0].GetKeyChainPath().Format() == original.GetKeyChainPath().Format());

	// The migrated row is now authenticated: tampering with the MAC (last byte)
	// must fail instead of producing a silently wrong output.
	std::vector<uint8_t> tampered;
	{
		auto pStatement = pDatabase->Query(
			"select encrypted from outputs where commitment='" + commitmentHex + "'"
		);
		REQUIRE(pStatement->Step());
		tampered = pStatement->GetColumnBytes(0);
	}
	REQUIRE_FALSE(tampered.empty());
	tampered.back() ^= 0x01;
	{
		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(BlobParameter::New(tampered));
		parameters.push_back(TextParameter::New(commitmentHex));
		pDatabase->Update("update outputs set encrypted=? where commitment=?", parameters);
	}
	REQUIRE_THROWS_AS(OutputsTable::GetOutputs(*pDatabase, masterSeed), std::runtime_error);
}

TEST_CASE("W-06 legacy slate context rows are swept and preserve negotiation state")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);

	const uuids::uuid slateId = uuids::uuid_system_generator()();
	SlateContextEntity original = MakeSlateContextEntity();
	InsertLegacySlateContext(*pDatabase, masterSeed, slateId, original);

	REQUIRE(SqliteStore::HasLegacyEncryption(*pDatabase));
	REQUIRE(SqliteStore::MigrateLegacyEncryption(*pDatabase, masterSeed));
	REQUIRE_FALSE(SqliteStore::HasLegacyEncryption(*pDatabase));

	const std::string slateIdStr = uuids::to_string(slateId);
	{
		auto pStatement = pDatabase->Query(
			"select enc_context, enc_format from slate_contexts where slate_id='" + slateIdStr + "'"
		);
		REQUIRE(pStatement->Step());
		const std::vector<uint8_t> migrated = pStatement->GetColumnBytes(0);
		REQUIRE_FALSE(migrated.empty());
		REQUIRE(migrated[0] == 2);
		REQUIRE(pStatement->GetColumnInt(1) == 1);
	}

	const std::unique_ptr<SlateContextEntity> loaded =
		SlateContextTable::LoadSlateContext(*pDatabase, masterSeed, slateId);
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->GetSecretKey() == original.GetSecretKey());
	REQUIRE(loaded->GetSecretNonce() == original.GetSecretNonce());
}

TEST_CASE("W-06 failed sweep rolls back and keeps legacy rows intact")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = OpenWalletDB(pTempFile);
	CreateCurrentSchema(*pDatabase);
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);

	InsertLegacyTransaction(*pDatabase, masterSeed, 31);

	// A corrupt legacy slate-context row makes the sweep fail after the
	// transaction table was already rewritten. The outer transaction must roll
	// back as a unit: no partially upgraded database.
	std::vector<SqliteDB::IParameter::UPtr> parameters;
	parameters.push_back(TextParameter::New(uuids::to_string(uuids::uuid_system_generator()())));
	parameters.push_back(BlobParameter::New(std::vector<uint8_t>(16, 0)));
	parameters.push_back(BlobParameter::New(std::vector<uint8_t>{ 1, 2, 3 }));
	pDatabase->Update(
		"insert into slate_contexts(slate_id, iv, enc_context) values(?, ?, ?)",
		parameters
	);

	REQUIRE(SqliteStore::HasLegacyEncryption(*pDatabase));

	{
		SqliteTransaction transaction(pDatabase);
		transaction.Begin();
		REQUIRE_THROWS(SqliteStore::MigrateLegacyEncryption(*pDatabase, masterSeed));
	}

	// Still legacy, and the transaction blob is still format 0.
	REQUIRE(SqliteStore::HasLegacyEncryption(*pDatabase));
	{
		auto pStatement = pDatabase->Query("select encrypted from transactions where id=31");
		REQUIRE(pStatement->Step());
		const std::vector<uint8_t> encrypted = pStatement->GetColumnBytes(0);
		REQUIRE_FALSE(encrypted.empty());
		REQUIRE(encrypted[0] == 0);
	}
}

TEST_CASE("W-06 wallet open sweeps legacy rows, backs up first, and is idempotent")
{
	const ConfigPtr pConfig = Config::Default(Environment::AUTOMATED_TESTING);
	const std::shared_ptr<SqliteStore> pStore = SqliteStore::Open(*pConfig);

	const std::string username = UniqueUsername();
	const fs::path walletDirectory = pConfig->GetWalletPath() / username;
	const fs::path dbPath = walletDirectory / "wallet.db";
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);

	pStore->CreateWallet(username, MakeTestSeed());
	pStore->CloseWallet(username);
	REQUIRE(FileUtil::Exists(dbPath));

	{
		auto pDatabase = SqliteDB::Open(dbPath, username);
		InsertLegacyTransaction(*pDatabase, masterSeed, 41);
		InsertLegacySlateContext(
			*pDatabase,
			masterSeed,
			uuids::uuid_system_generator()(),
			MakeSlateContextEntity()
		);
	}
	REQUIRE(FindMigrationBackups(walletDirectory).empty());

	{
		(void)pStore->OpenWallet(username, masterSeed);
		REQUIRE(FindMigrationBackups(walletDirectory).size() == 1);
		pStore->CloseWallet(username);
	}

	{
		auto pDatabase = SqliteDB::Open(dbPath, username);
		REQUIRE_FALSE(SqliteStore::HasLegacyEncryption(*pDatabase));

		auto pTransaction = pDatabase->Query("select encrypted from transactions where id=41");
		REQUIRE(pTransaction->Step());
		const std::vector<uint8_t> migratedTransaction = pTransaction->GetColumnBytes(0);
		REQUIRE_FALSE(migratedTransaction.empty());
		REQUIRE(migratedTransaction[0] == 2);

		auto pContext = pDatabase->Query("select enc_format from slate_contexts");
		REQUIRE(pContext->Step());
		REQUIRE(pContext->GetColumnInt(0) == 1);
	}

	// The backup is the pre-sweep database: the transaction row there is still
	// in legacy format 0.
	const std::vector<fs::path> backups = FindMigrationBackups(walletDirectory);
	REQUIRE(backups.size() == 1);
	{
		auto pBackup = SqliteDB::Open(backups[0], "backup");
		auto pTransaction = pBackup->Query("select encrypted from transactions where id=41");
		REQUIRE(pTransaction->Step());
		const std::vector<uint8_t> legacyTransaction = pTransaction->GetColumnBytes(0);
		REQUIRE_FALSE(legacyTransaction.empty());
		REQUIRE(legacyTransaction[0] == 0);

		// Rollback path: the backup is a complete, still-readable legacy wallet.
		const std::vector<WalletTx> backupTransactions =
			TransactionsTable::GetTransactions(*pBackup, masterSeed);
		REQUIRE(backupTransactions.size() == 1);
		REQUIRE(backupTransactions[0].GetId() == 41);
	}

	// Second open must not sweep again and must not accumulate backups.
	(void)pStore->OpenWallet(username, masterSeed);
	pStore->CloseWallet(username);
	REQUIRE(FindMigrationBackups(walletDirectory).size() == 1);

	pStore->DeleteWallet(username);
}

TEST_CASE("W-06 wallet open keeps legacy rows when the sweep fails")
{
	const ConfigPtr pConfig = Config::Default(Environment::AUTOMATED_TESTING);
	const std::shared_ptr<SqliteStore> pStore = SqliteStore::Open(*pConfig);

	const std::string username = UniqueUsername();
	const fs::path walletDirectory = pConfig->GetWalletPath() / username;
	const fs::path dbPath = walletDirectory / "wallet.db";
	const SecureVector masterSeed = CSPRNG::GenerateRandomBytes(32);

	pStore->CreateWallet(username, MakeTestSeed());
	pStore->CloseWallet(username);

	{
		auto pDatabase = SqliteDB::Open(dbPath, username);
		InsertLegacyTransaction(*pDatabase, masterSeed, 51);

		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(TextParameter::New(uuids::to_string(uuids::uuid_system_generator()())));
		parameters.push_back(BlobParameter::New(std::vector<uint8_t>(16, 0)));
		parameters.push_back(BlobParameter::New(std::vector<uint8_t>{ 4, 5, 6 }));
		pDatabase->Update(
			"insert into slate_contexts(slate_id, iv, enc_context) values(?, ?, ?)",
			parameters
		);
	}

	// A failed sweep must not abort the open (legacy rows stay readable and are
	// retried on the next open), and it must leave a pre-sweep backup.
	{
		auto pWallet = pStore->OpenWallet(username, masterSeed);
		(void)pWallet;
	}
	pStore->CloseWallet(username);
	REQUIRE(FindMigrationBackups(walletDirectory).size() == 1);

	{
		auto pDatabase = SqliteDB::Open(dbPath, username);
		REQUIRE(SqliteStore::HasLegacyEncryption(*pDatabase));

		auto pTransaction = pDatabase->Query("select encrypted from transactions where id=51");
		REQUIRE(pTransaction->Step());
		const std::vector<uint8_t> encrypted = pTransaction->GetColumnBytes(0);
		REQUIRE_FALSE(encrypted.empty());
		REQUIRE(encrypted[0] == 0);

		auto pContext = pDatabase->Query("select enc_format from slate_contexts");
		REQUIRE(pContext->Step());
		REQUIRE(pContext->GetColumnInt(0) == 0);
	}

	pStore->DeleteWallet(username);
}
