#include <catch.hpp>

#include <Database/DbBackup.h>

#include <filesystem.h>
#include <fstream>

namespace
{
	fs::path TempDatabasePath()
	{
		static uint32_t counter = 0;
		const fs::path dir = fs::temp_directory_path() /
			("grinpp-dbbackup-" + std::to_string(++counter)) / "DB";
		fs::create_directories(dir / "CHAIN");
		return dir;
	}

	void WriteFile(const fs::path& path, const std::string& contents)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << contents;
	}
}

TEST_CASE("DbBackup::CopiesRecursivelyAndSkipsLock")
{
	const fs::path dbPath = TempDatabasePath();
	WriteFile(dbPath / "CURRENT", "current");
	WriteFile(dbPath / "LOCK", "locked");
	WriteFile(dbPath / "CHAIN" / "candidate.chain", "chain");

	const fs::path backup = DbBackup::BackupBeforeMigration(dbPath, 1);
	REQUIRE(!backup.empty());
	REQUIRE(fs::exists(backup));
	REQUIRE(fs::exists(backup / "CURRENT"));
	REQUIRE(fs::exists(backup / "CHAIN" / "candidate.chain"));
	// The RocksDB lock is deliberately not copied.
	REQUIRE(!fs::exists(backup / "LOCK"));

	std::ifstream input(backup / "CHAIN" / "candidate.chain", std::ios::binary);
	std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	REQUIRE(contents == "chain");
}

TEST_CASE("DbBackup::FailsForMissingDatabase")
{
	const fs::path missing = fs::temp_directory_path() / "grinpp-dbbackup-missing" / "DB";
	const fs::path backup = DbBackup::BackupBeforeMigration(missing, 1);
	REQUIRE(backup.empty());
}
