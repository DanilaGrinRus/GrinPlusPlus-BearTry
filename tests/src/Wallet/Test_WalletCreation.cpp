#include <catch.hpp>

#include <Core/Config.h>
#include <Core/Enums/Environment.h>
#include <Common/Util/FileUtil.h>
#include <filesystem.h>
#include <Crypto/CSPRNG.h>
#include <Wallet/WalletDB/Sqlite/SqliteDB.h>
#include <Wallet/WalletDB/Sqlite/SqliteStore.h>
#include <Wallet/WalletDB/WalletStoreException.h>
#include <uuid.h>

#include <string>
#include <vector>

namespace
{
	EncryptedSeed MakeTestSeed()
	{
		SecureVector iv = CSPRNG::GenerateRandomBytes(16);
		SecureVector salt = CSPRNG::GenerateRandomBytes(8);
		return EncryptedSeed(
			CBigInteger<16>(iv.data()),
			CBigInteger<8>(salt.data()),
			std::vector<uint8_t>(48, 0),
			ScryptParameters(1024, 8, 1)
		);
	}

	std::string UniqueUsername()
	{
		return "db-test-" + uuids::to_string(uuids::uuid_system_generator()());
	}
}

TEST_CASE("CreateWallet recycles an empty leftover directory")
{
	const ConfigPtr pConfig = Config::Default(Environment::AUTOMATED_TESTING);
	auto pStore = SqliteStore::Open(*pConfig);

	const std::string username = UniqueUsername();
	const fs::path userDir = pConfig->GetWalletPath() / username;

	// A previous failed create may leave an empty directory behind.
	REQUIRE(FileUtil::CreateDirectories(userDir));
	REQUIRE(fs::is_empty(userDir));

	REQUIRE_NOTHROW(pStore->CreateWallet(username, MakeTestSeed()));
	REQUIRE(FileUtil::Exists(userDir / "seed.json"));
	REQUIRE(FileUtil::Exists(userDir / "wallet.db"));

	pStore->CloseWallet(username);
	pStore->DeleteWallet(username);
}

TEST_CASE("CreateWallet cleans an interrupted seed.tmp and retries")
{
	const ConfigPtr pConfig = Config::Default(Environment::AUTOMATED_TESTING);
	auto pStore = SqliteStore::Open(*pConfig);

	const std::string username = UniqueUsername();
	const fs::path userDir = pConfig->GetWalletPath() / username;
	REQUIRE(FileUtil::CreateDirectories(userDir));

	// Simulate a crash between writing seed.json.tmp and its rename.
	FileUtil::WriteTextToFile(userDir / "seed.json.tmp", "partial");
	REQUIRE_FALSE(fs::is_empty(userDir));

	REQUIRE_NOTHROW(pStore->CreateWallet(username, MakeTestSeed()));
	REQUIRE(FileUtil::Exists(userDir / "seed.json"));
	REQUIRE_FALSE(FileUtil::Exists(userDir / "seed.json.tmp"));

	pStore->CloseWallet(username);
	pStore->DeleteWallet(username);
}

TEST_CASE("Created wallet keeps armored slatepack after stale-version reopen")
{
	const ConfigPtr pConfig = Config::Default(Environment::AUTOMATED_TESTING);
	auto pStore = SqliteStore::Open(*pConfig);

	const std::string username = UniqueUsername();
	const fs::path userDir = pConfig->GetWalletPath() / username;
	pStore->CreateWallet(username, MakeTestSeed());
	pStore->CloseWallet(username);

	const fs::path dbPath = userDir / "wallet.db";
	{
		auto pDatabase = SqliteDB::Open(dbPath, "test");
		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(TextParameter::New("integration-slate"));
		parameters.push_back(TextParameter::New("STANDARD"));
		parameters.push_back(BlobParameter::New(std::vector<uint8_t>(16, 0)));
		parameters.push_back(BlobParameter::New(std::vector<uint8_t>{ 7, 8, 9 }));
		parameters.push_back(TextParameter::New("armored-integration"));
		pDatabase->Update(
			"insert into slate(slate_id, stage, iv, slate, armored_slatepack) values(?, ?, ?, ?, ?)",
			parameters
		);

		// Simulate the N-08 state produced by older builds.
		pDatabase->Execute("update version set schema_version=2;");
	}

	{
		// Reopen through the real migration path.
		auto pWallet = pStore->OpenWallet(username, CSPRNG::GenerateRandomBytes(32));
		(void)pWallet;
		pStore->CloseWallet(username);
	}

	{
		auto pDatabase = SqliteDB::Open(dbPath, "test");
		auto pStatement = pDatabase->Query(
			"select armored_slatepack from slate where slate_id='integration-slate'"
		);
		REQUIRE(pStatement->Step());
		REQUIRE_FALSE(pStatement->IsColumnNull(0));
		REQUIRE(pStatement->GetColumnString(0) == "armored-integration");
	}

	pStore->DeleteWallet(username);
}

TEST_CASE("CreateWallet refuses a seed without a database and does not delete it")
{
	const ConfigPtr pConfig = Config::Default(Environment::AUTOMATED_TESTING);
	auto pStore = SqliteStore::Open(*pConfig);

	const std::string username = UniqueUsername();
	const fs::path userDir = pConfig->GetWalletPath() / username;
	REQUIRE(FileUtil::CreateDirectories(userDir));

	const fs::path seedFile = userDir / "seed.json";
	FileUtil::WriteTextToFile(seedFile, "{}");
	REQUIRE(FileUtil::Exists(seedFile));

	try
	{
		pStore->CreateWallet(username, MakeTestSeed());
		FAIL("Expected CreateWallet to refuse a seed without a database");
	}
	catch (const WalletStoreException& e)
	{
		REQUIRE(std::string(e.what()).find("without wallet database") != std::string::npos);
	}

	// The pre-existing seed must never be removed by a failed create.
	REQUIRE(FileUtil::Exists(seedFile));

	FileUtil::RemoveFile(userDir);
}
