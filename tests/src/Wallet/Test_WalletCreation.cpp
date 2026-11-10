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

#include <cstdlib>
#include <fstream>
#include <optional>
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

	// task-63 low-disk hook: the driver mounts an own small VHD and points this
	// variable at a directory on it. Normal runs never mount such a volume and
	// skip the real-ENOSPC scenario.
	std::optional<std::string> GetLowDiskRoot()
	{
#ifdef _WIN32
		char* value = nullptr;
		size_t length = 0;
		if (_dupenv_s(&value, &length, "GRINPP_LOWDISK_ROOT") != 0 || value == nullptr) {
			return std::nullopt;
		}

		std::string root(value);
		std::free(value);
		if (root.empty()) {
			return std::nullopt;
		}

		return root;
#else
		const char* value = std::getenv("GRINPP_LOWDISK_ROOT");
		if (value == nullptr || *value == '\0') {
			return std::nullopt;
		}

		return std::string(value);
#endif
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

//
// task-63 low-disk: real ENOSPC on an own small VHD (mounted by the w63 PS
// driver). The test fills the volume itself, so it must refuse to run on a
// normal disk; without GRINPP_LOWDISK_ROOT it is skipped silently and the
// regular suite is unaffected.
//
TEST_CASE("CreateWallet fails closed on a full volume (task-63 low-disk)")
{
	const std::optional<std::string> lowDiskRoot = GetLowDiskRoot();
	if (!lowDiskRoot.has_value()) {
		return;
	}

	const fs::path root(*lowDiskRoot);
	std::error_code error;
	fs::create_directories(root, error);
	REQUIRE_FALSE(error);

	// Safety: only a deliberately small test volume may be filled. This prevents
	// an accidentally exported GRINPP_LOWDISK_ROOT=C:\... from filling a real disk.
	const fs::space_info capacityInfo = fs::space(root, error);
	REQUIRE_FALSE(error);
	REQUIRE(capacityInfo.capacity <= (256ull * 1024ull * 1024ull));

	const fs::path filler = root / "w63-enospc-filler.bin";
	const uint64_t maximumFill = (uint64_t)capacityInfo.capacity;
	{
		std::ofstream stream(filler, std::ios::binary | std::ios::trunc);
		REQUIRE(stream.good());
		std::vector<char> chunk(1024 * 1024, 0x5A);
		uint64_t written = 0;
		while (stream.good() && written < maximumFill) {
			stream.write(chunk.data(), (std::streamsize)chunk.size());
			stream.flush();
			written += chunk.size();
		}
	}
	REQUIRE(FileUtil::Exists(filler));

	const fs::space_info fullInfo = fs::space(root, error);
	REQUIRE_FALSE(error);
	REQUIRE(fullInfo.available < (4ull * 1024ull * 1024ull));

	const ConfigPtr pConfig = Config::Default(Environment::AUTOMATED_TESTING);
	const std::shared_ptr<SqliteStore> pStore = SqliteStore::Open(*pConfig);
	const std::string username = UniqueUsername();
	const fs::path userDir = pConfig->GetWalletPath() / username;

	// A full volume must fail closed (any store/file error) and must never leave
	// a wallet database that later looks successfully created.
	bool failedClosed = false;
	try {
		pStore->CreateWallet(username, MakeTestSeed());
	} catch (const std::exception&) {
		failedClosed = true;
	}
	REQUIRE(failedClosed);
	REQUIRE_FALSE(FileUtil::Exists(userDir / "wallet.db"));

	// Free the volume again: the failed attempt must not leave the profile in a
	// state that blocks a later successful create.
	fs::remove(filler, error);
	REQUIRE_FALSE(error);
	fs::remove_all(userDir, error);
	error.clear();

	REQUIRE_NOTHROW(pStore->CreateWallet(username, MakeTestSeed()));
	REQUIRE(FileUtil::Exists(userDir / "seed.json"));
	REQUIRE(FileUtil::Exists(userDir / "wallet.db"));
	pStore->CloseWallet(username);
	pStore->DeleteWallet(username);
}
