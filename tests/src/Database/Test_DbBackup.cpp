#include <catch.hpp>

#include <Database/DbBackup.h>
#include <Database/Database.h>
#include <Core/Config.h>
#include <Core/Enums/Environment.h>
#include <json/json.h>
#include <filesystem.h>

namespace
{
	fs::path TempRoot(const std::string& tag)
	{
		static uint32_t counter = 0;
		const fs::path root = fs::temp_directory_path() /
			("grinpp-dbcheckpoint-" + tag + "-" + std::to_string(++counter));
		std::error_code ec;
		fs::remove_all(root, ec);
		fs::create_directories(root, ec);
		return root;
	}

	ConfigPtr MakeConfig(const fs::path& dataPath)
	{
		Json::Value json(Json::objectValue);
		json["DATA_PATH"] = dataPath.string();
		return Config::Load(json, Environment::AUTOMATED_TESTING);
	}
}

TEST_CASE("DbBackup::CheckpointOfOpenDatabaseReopensAtCommittedState")
{
	const fs::path root = TempRoot("reopen");
	ConfigPtr pConfig = MakeConfig(root / "A");

	IDatabasePtr pDatabase = DatabaseAPI::OpenDatabase(*pConfig);
	pDatabase->GetBlockDB()->Write()->SetVersion(3);

	// The source database stays open while the backup is taken.
	const fs::path backup = DbBackup::BackupBeforeMigration(
		pDatabase->GetBlockDB(),
		pConfig->GetDatabasePath(),
		3
	);
	REQUIRE(!backup.empty());
	REQUIRE(fs::exists(backup));
	REQUIRE(fs::exists(backup / "CURRENT"));

	// A committed write after the checkpoint must not appear in the snapshot.
	pDatabase->GetBlockDB()->Write()->SetVersion(7);
	pDatabase.reset();

	// Reopen the checkpoint through the normal database path: a backup is only
	// valid if it can be opened and serves every committed key as of the
	// checkpoint.
	ConfigPtr pReopenedConfig = MakeConfig(root / "B");
	fs::create_directories(root / "B" / "NODE" / "DB");
	fs::rename(backup, root / "B" / "NODE" / "DB" / "CHAIN");

	IDatabasePtr pReopened = DatabaseAPI::OpenDatabase(*pReopenedConfig);
	REQUIRE(pReopened->GetBlockDB()->Read()->GetVersion() == 3);
	pReopened.reset();

	std::error_code ec;
	fs::remove_all(root, ec);
}

TEST_CASE("DbBackup::FailsClosedWithoutDatabaseOrIntoExistingDestination")
{
	REQUIRE(DbBackup::BackupBeforeMigration(
		nullptr,
		fs::temp_directory_path() / "grinpp-dbcheckpoint-missing" / "DB",
		1
	).empty());

	const fs::path root = TempRoot("fail");
	ConfigPtr pConfig = MakeConfig(root / "A");
	IDatabasePtr pDatabase = DatabaseAPI::OpenDatabase(*pConfig);
	pDatabase->GetBlockDB()->Write()->SetVersion(1);

	const fs::path existing = root / "existing-checkpoint";
	fs::create_directories(existing);
	REQUIRE(DbBackup::BackupTo(pDatabase->GetBlockDB(), existing).empty());
	REQUIRE_FALSE(pDatabase->GetBlockDB()->Read()->CreateCheckpoint(existing));

	pDatabase.reset();
	std::error_code ec;
	fs::remove_all(root, ec);
}
