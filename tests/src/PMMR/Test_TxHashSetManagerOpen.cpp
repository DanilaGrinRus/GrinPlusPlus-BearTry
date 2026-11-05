#include <catch.hpp>

#include <Core/Config.h>
#include <Core/Global.h>
#include <Core/Models/FullBlock.h>
#include <PMMR/TxHashSetManager.h>

#include <filesystem.h>
#include <fstream>

TEST_CASE("TxHashSetManager propagates parallel MMR load failures")
{
	const fs::path root = fs::temp_directory_path() / "grinpp-manager-load-failure";
	std::error_code ec;
	fs::remove_all(root, ec);
	Json::Value json;
	json["DATA_PATH"] = root.string();
	auto config = Config::Load(json, Environment::AUTOMATED_TESTING);
	const fs::path kernelDir = config->GetTxHashSetPath() / "kernel";
	fs::remove_all(kernelDir);
	std::ofstream(kernelDir, std::ios::binary).put('x');

	TxHashSetManager manager(*config);
	REQUIRE_THROWS(manager.Open(Global::GetGenesisBlock().GetHeader()));
	fs::remove_all(root, ec);
}
