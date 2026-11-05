#include <catch.hpp>

#include <TestServer.h>
#include <TestChain.h>
#include <TxBuilder.h>

#include <BlockChain/BlockChain.h>
#include <Core/Global.h>
#include <Database/BlockDb.h>
#include <P2P/SyncStatus.h>
#include <PMMR/KernelIndex.h>
#include <PMMR/TxHashSetManager.h>
#include <Wallet/Keychain/KeyChain.h>

#include <Roaring.h>
#include <filesystem.h>
#include <fstream>

namespace
{
// A valid empty Roaring bitmap, not an empty file. PruneList::Load uses
// Roaring::readSafe; zero bytes fail with "failed alloc while reading".
void WriteEmptyPruneList(const fs::path& filePath)
{
	Roaring empty;
	empty.runOptimize();
	const size_t size = empty.getSizeInBytes();
	REQUIRE(size > 0);
	std::vector<unsigned char> bytes(size);
	empty.write(reinterpret_cast<char*>(bytes.data()));
	std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
	REQUIRE(out.good());
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	out.close();
	REQUIRE(out.good());
}
}

// The full Floonet PIBD network path is too large for the unit suite. This test
// runs the real BlockChain::ProcessTransactionHashSet activation path with a
// real TxHashSet archive produced from a live test state and requires the
// post-install hook to publish an index at the installed tip.
TEST_CASE("KernelIndex builds after TxHashSet install", "[pibd][.]")
{
	TestServer::Ptr pServer = TestServer::Create();
	IBlockChain::Ptr pChain = pServer->GetBlockChain();
	auto pBlockDB = pServer->GetBlockDB();
	REQUIRE(pChain->EnsureKernelIndex());

	const KeyChain keyChain = KeyChain::FromRandom();
	TxBuilder txBuilder(keyChain);
	{
		TestChain builder(pChain);
		for (uint64_t i = 1; i <= 4; i++) {
			const auto block = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, static_cast<uint32_t>(i) })) });
			REQUIRE(pChain->AddBlock(block.block) == EBlockChainStatus::SUCCESS);
		}
	}

	const auto pTip = pChain->GetTipBlockHeader(EChainType::CONFIRMED);
	REQUIRE(pTip != nullptr);
	const fs::path txHashSetPath = Global::GetConfig().GetTxHashSetPath();
	fs::create_directories(txHashSetPath / "output");
	fs::create_directories(txHashSetPath / "rangeproof");
	WriteEmptyPruneList(txHashSetPath / "output" / "pmmr_prun.bin");
	WriteEmptyPruneList(txHashSetPath / "rangeproof" / "pmmr_prun.bin");

	const auto zip = pServer->GetTxHashSetManagerShared()->SaveSnapshot(
		std::const_pointer_cast<IBlockDB>(pBlockDB->Read().GetShared()), pTip);
	REQUIRE(!zip.empty());

	// Force a stale index before the install; the hook must rebuild it from the
	// newly installed state, not rely on the pre-install generation.
	const auto before = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(before != nullptr);
	{
		auto pWriter = pBlockDB->BatchWrite();
		pWriter.GetShared()->SetKernelIndexState(KernelIndexState(
			before->GetGeneration() + 100, 0, Hash(), Hash(), 0, 0, Hash()));
		pWriter.GetShared()->Commit();
	}

	SyncStatus syncStatus;
	REQUIRE(pChain->ProcessTransactionHashSet(pTip->GetHash(), zip, syncStatus) == EBlockChainStatus::SUCCESS);
	REQUIRE(pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() == pTip->GetHash());

	const auto after = pBlockDB->Read()->GetKernelIndexState();
	REQUIRE(after != nullptr);
	REQUIRE(after->GetTipHash() == pTip->GetHash());
	REQUIRE(after->GetTipHeight() == pTip->GetHeight());
	REQUIRE(after->GetKernelCount() == pTip->GetNumKernels());
	REQUIRE(pBlockDB->Read()->GetKernelPositionDigest(after->GetGeneration()) ==
		std::make_pair(after->GetKernelCount(), after->GetChecksum()));
	const uint64_t installedGeneration = after->GetGeneration();

	// Clean restart after the install: the same published generation must be
	// accepted as current, not rebuilt, and still be at the installed tip.
	pServer->SetPreserveDataOnDestruct(true);
	pChain = nullptr;
	pBlockDB = nullptr;
	pServer = nullptr;
	auto pRestarted = TestServer::OpenExisting();
	const auto restarted = pRestarted->GetBlockDB()->Read()->GetKernelIndexState();
	REQUIRE(restarted != nullptr);
	REQUIRE(restarted->GetTipHash() == pTip->GetHash());
	REQUIRE(restarted->GetTipHeight() == pTip->GetHeight());
	REQUIRE(restarted->GetGeneration() == installedGeneration);
}

