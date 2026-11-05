#include <catch.hpp>

#include <BlockChain/DbRecovery.h>

#include <filesystem.h>
#include <fstream>

namespace
{
	fs::path TempChainPath()
	{
		static uint32_t counter = 0;
		const fs::path dir = fs::temp_directory_path() /
			("grinpp-recovery-" + std::to_string(++counter)) / "CHAIN";
		fs::create_directories(dir);
		return dir;
	}

	void WriteFile(const fs::path& path, const std::string& contents)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << contents;
	}
}

TEST_CASE("DbRecovery::CopiesAffectedFilesAside")
{
	const fs::path chainPath = TempChainPath();
	WriteFile(chainPath / "candidate.chain", "candidate");
	WriteFile(chainPath / "header_mmr.bin", "mmr");
	// confirmed.chain and chain.checkpoint are intentionally absent.

	const uint64_t before = DbRecovery::GetRepairCount();
	REQUIRE(DbRecovery::CopyForRecovery(
		chainPath,
		{ "header_mmr.bin", "candidate.chain", "confirmed.chain", "chain.checkpoint" }));

	const fs::path parent = chainPath.parent_path();
	bool found = false;
	for (const auto& entry : fs::directory_iterator(parent)) {
		if (!entry.is_directory()) {
			continue;
		}

		const std::string name = entry.path().filename().string();
		if (name.rfind("recovery-", 0) != 0) {
			continue;
		}

		found = true;
		REQUIRE(fs::exists(entry.path() / "header_mmr.bin"));
		REQUIRE(fs::exists(entry.path() / "candidate.chain"));
		REQUIRE(!fs::exists(entry.path() / "confirmed.chain"));
	}

	REQUIRE(found);
	REQUIRE(DbRecovery::GetRepairCount() == before);
}

TEST_CASE("DbRecovery::RecordRepairUpdatesCounters")
{
	const uint64_t before = DbRecovery::GetRepairCount();
	const int64_t beforeTime = DbRecovery::GetLastRepairTime();

	DbRecovery::RecordRepair();

	REQUIRE(DbRecovery::GetRepairCount() == before + 1);
	REQUIRE(DbRecovery::GetLastRepairTime() >= beforeTime);
	REQUIRE(DbRecovery::GetLastRepairTime() > 0);
}
