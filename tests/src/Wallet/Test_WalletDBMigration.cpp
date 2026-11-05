#include <catch.hpp>

#include <TestFileUtil.h>
#include <Crypto/CSPRNG.h>
#include <Wallet/WalletDB/Sqlite/SqliteDB.h>
#include <Wallet/WalletDB/Sqlite/Schema.h>
#include <Wallet/WalletDB/Sqlite/Tables/AccountsTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/MetadataTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/OutputsTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/SlateContextTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/SlateTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/TransactionsTable.h>
#include <Wallet/WalletDB/Sqlite/Tables/VersionTable.h>
#include <Wallet/WalletDB/WalletStoreException.h>

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
