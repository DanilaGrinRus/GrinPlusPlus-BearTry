#include <catch.hpp>

#include <TestServer.h>
#include <TestMiner.h>

#include <BlockChain/BlockChain.h>
#include <Core/Config.h>
#include <Core/Global.h>
#include <PMMR/TxHashSetImpl.h>
#include <Wallet/Keychain/KeyChain.h>

#include <chrono>
#include <cstdlib>
#include <filesystem.h>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace
{
	std::string GetEnv(const char* name)
	{
#if defined(_WIN32)
		char* value = nullptr;
		size_t size = 0;
		_dupenv_s(&value, &size, name);
		std::string result = value != nullptr ? std::string(value) : std::string();
		std::free(value);
		return result;
#else
		const char* value = std::getenv(name);
		return value != nullptr ? std::string(value) : std::string();
#endif
	}

	uint64_t ReadStoredHeight(const std::string& path)
	{
		std::ifstream in(path);
		REQUIRE(in.good());
		uint64_t height = 0;
		in >> height;
		REQUIRE(in.good());
		return height;
	}

	std::shared_ptr<TxHashSet> RequireCompleteIndex(const IBlockChain::Ptr& pChain)
	{
		auto pTxHashSet = std::dynamic_pointer_cast<TxHashSet>(pChain->GetTxHashSet());
		REQUIRE(pTxHashSet != nullptr);
		REQUIRE(pTxHashSet->IsRecentNrdIndexComplete());
		return pTxHashSet;
	}
}

// External-kill harness child. Hidden by default ([.]) and only selected by
// tools/audit/nrd/rebuild-kill-harness.ps1, which kills this process after the
// state marker appears. The child mines a real chain, then repeatedly closes and
// reopens the chain (each reopen runs the startup NRD rebuild) until killed.
TEST_CASE("RebuildKillChild", "[.]")
{
	const std::string statePath = GetEnv("GRINPP_NRD_KILL_STATE");
	REQUIRE(!statePath.empty());

	uint64_t expectedHeight = 0;
	{
		TestServer::Ptr pServer = TestServer::Create();
		pServer->SetPreserveDataOnDestruct(true);
		TestMiner miner(pServer);
		KeyChain keyChain = KeyChain::FromRandom();
		const std::vector<MinedBlock> chain = miner.MineChain(keyChain, 60);
		REQUIRE(chain.size() == 60);

		const BlockHeaderPtr pTip = pServer->GetBlockChain()->GetTipBlockHeader(EChainType::CONFIRMED);
		REQUIRE(pTip != nullptr);
		expectedHeight = pTip->GetHeight();
		REQUIRE(expectedHeight == 59);

		std::ofstream out(statePath, std::ios::trunc);
		REQUIRE(out.good());
		out << expectedHeight << std::endl;
		out.close();
	}

	// Reopen loop: each iteration runs the startup rebuild over the real DB and
	// gives the harness a kill window. Reaching the end means the harness failed
	// to kill us.
	for (int i = 0; i < 1000; ++i) {
		TestServer::Ptr pServer = TestServer::OpenExisting();
		pServer->SetPreserveDataOnDestruct(true);
		REQUIRE(pServer->GetBlockChain()->GetHeight(EChainType::CONFIRMED) == expectedHeight);
		RequireCompleteIndex(pServer->GetBlockChain());
		pServer.reset();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	FAIL("child was not killed by the external harness");
}

// Recovery half of the external-kill harness: reopen the same data dirs after the
// child process was force-killed mid restart/rebuild and prove a complete NRD
// index is rebuilt (no partially rebuilt index is published or trusted).
TEST_CASE("RebuildKillRecovery", "[.]")
{
	const std::string statePath = GetEnv("GRINPP_NRD_KILL_STATE");
	const std::string backupPath = GetEnv("GRINPP_NRD_KILL_BACKUP");
	REQUIRE(!statePath.empty());

	// The test runtime deletes the AUTOMATED_TESTING data dir at process start, so
	// the harness snapshots the killed child's dirs and this recovery process
	// restores them before reopening.
	if (!backupPath.empty()) {
		const fs::path dataDir = Global::GetConfig().GetDataDirectory();
		const fs::path backupDir(backupPath);
		REQUIRE(fs::exists(backupDir));

		std::error_code error;
		fs::remove_all(dataDir, error);
		fs::create_directories(dataDir, error);
		REQUIRE(!error);

		// Copy file-by-file. Directory-copy semantics differ for an existing
		// per-process data dir (created by the test runtime), which made the
		// single fs::copy of the whole backup fail.
		for (const auto& entry : fs::recursive_directory_iterator(backupDir, error)) {
			REQUIRE(!error);
			const fs::path relative = fs::relative(entry.path(), backupDir, error);
			REQUIRE(!error);
			const fs::path destination = dataDir / relative;
			if (entry.is_directory()) {
				fs::create_directories(destination, error);
			} else {
				fs::create_directories(destination.parent_path(), error);
				REQUIRE(!error);
				fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing, error);
			}
			REQUIRE(!error);
		}
	}

	const uint64_t expectedHeight = ReadStoredHeight(statePath);

	TestServer::Ptr pServer = TestServer::OpenExisting();
	pServer->SetPreserveDataOnDestruct(true);
	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	REQUIRE(pChain->GetHeight(EChainType::CONFIRMED) == expectedHeight);
	RequireCompleteIndex(pChain);
	REQUIRE(pChain->GetTipBlockHeader(EChainType::CONFIRMED) != nullptr);
	REQUIRE(pChain->GetBlockByHeight(expectedHeight) != nullptr);
}

