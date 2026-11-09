#include <catch.hpp>

#include <PMMR/TxHashSetStaging.h>

#include <filesystem.h>
#include <fstream>

namespace
{
	fs::path TempRoot()
	{
		static uint32_t counter = 0;
		const fs::path root = fs::temp_directory_path() /
			("grinpp-txhashset-staging-" + std::to_string(++counter));
		fs::create_directories(root);
		return root;
	}

	void WriteFile(const fs::path& path)
	{
		fs::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << "data";
	}

	void WriteMarker(const fs::path& live)
	{
		const fs::path marker = TxHashSetStaging::Marker(live);
		fs::create_directories(marker.parent_path());
		std::ofstream file(marker, std::ios::binary | std::ios::trunc);
		file << "pending";
	}
}

TEST_CASE("TxHashSetStaging::RemovesOrphanStagingAndMarker")
{
	const fs::path live = TempRoot() / "TXHASHSET";
	const fs::path staging = TxHashSetStaging::Staging(live);
	WriteFile(staging / "output" / "pmmr_data.bin");
	WriteMarker(live);

	TxHashSetStaging::Recover(live);

	REQUIRE(!fs::exists(staging));
	REQUIRE(!fs::exists(TxHashSetStaging::Marker(live)));
	REQUIRE(!fs::exists(live));
}

TEST_CASE("TxHashSetStaging::RestoresBackupWhenLiveIsMissing")
{
	const fs::path live = TempRoot() / "TXHASHSET";
	const fs::path backup = TxHashSetStaging::Backup(live);
	WriteFile(backup / "kernel" / "pmmr_data.bin");
	WriteMarker(live);

	TxHashSetStaging::Recover(live);

	REQUIRE(fs::exists(live / "kernel" / "pmmr_data.bin"));
	REQUIRE(!fs::exists(backup));
	REQUIRE(!fs::exists(TxHashSetStaging::Marker(live)));
}

TEST_CASE("TxHashSetStaging::KeepsInstalledLiveState")
{
	const fs::path live = TempRoot() / "TXHASHSET";
	const fs::path staging = TxHashSetStaging::Staging(live);
	WriteFile(live / "output" / "pmmr_data.bin");
	WriteFile(staging / "output" / "pmmr_data.bin");
	WriteMarker(live);

	TxHashSetStaging::Recover(live);

	REQUIRE(fs::exists(live / "output" / "pmmr_data.bin"));
	REQUIRE(!fs::exists(staging));
	REQUIRE(!fs::exists(TxHashSetStaging::Marker(live)));
}

TEST_CASE("TxHashSetStaging::RollsBackInstalledStateBeforeDbCommit")
{
	const fs::path live = TempRoot() / "TXHASHSET";
	WriteFile(live / "old.txt");
	TxHashSetStaging::Begin(live, "new-tip");
	fs::rename(live, TxHashSetStaging::Backup(live));
	WriteFile(live / "new.txt");

	TxHashSetStaging::Recover(live, "old-tip");

	REQUIRE(fs::exists(live / "old.txt"));
	REQUIRE_FALSE(fs::exists(live / "new.txt"));
	REQUIRE_FALSE(fs::exists(TxHashSetStaging::Backup(live)));
	REQUIRE_FALSE(fs::exists(TxHashSetStaging::Marker(live)));
}

TEST_CASE("TxHashSetStaging::KeepsInstalledStateAfterDbCommit")
{
	const fs::path live = TempRoot() / "TXHASHSET";
	WriteFile(live / "old.txt");
	TxHashSetStaging::Begin(live, "new-tip");
	fs::rename(live, TxHashSetStaging::Backup(live));
	WriteFile(live / "new.txt");

	TxHashSetStaging::Recover(live, "new-tip");

	REQUIRE(fs::exists(live / "new.txt"));
	REQUIRE_FALSE(fs::exists(live / "old.txt"));
	REQUIRE_FALSE(fs::exists(TxHashSetStaging::Backup(live)));
	REQUIRE_FALSE(fs::exists(TxHashSetStaging::Marker(live)));
}
