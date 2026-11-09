#include <catch.hpp>

#include <TestMiner.h>
#include <TestServer.h>

#include <BlockChain/BlockChain.h>
#include <Core/Config.h>
#include <Core/Global.h>
#include <Core/Models/OutputLocation.h>
#include <Crypto/Models/Hash.h>
#include <Database/BlockDb.h>
#include <Database/KernelIndexState.h>
#include <Database/RocksDB/DBEntry.h>
#include <Database/RocksDB/RocksDBFactory.h>
#include <PMMR/KernelIndex.h>
#include <Wallet/Keychain/KeyChain.h>

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <cstring>
#include <vector>

namespace
{
	Commitment MakeExcess(const uint8_t tag)
	{
		return Commitment(CBigInteger<33>::ValueOf(tag));
	}

	OutputLocation MakeLocation(const uint64_t leafIndex, const uint64_t height)
	{
		return OutputLocation(LeafIndex::At(leafIndex), height);
	}
}

// Schema-v2 kernel index: one primary entry per kernel leaf (duplicates by excess
// remain distinct), secondary excess->position lookup returning the latest
// occurrence, and count/checksum over every leaf.
TEST_CASE("KernelIndex schema v2 keeps duplicate excesses and latest position")
{
	TestServer::Ptr pServer = TestServer::Create();
	auto pBlockDB = pServer->GetBlockDB();
	const uint64_t generation = 42;

	const Commitment excessA = MakeExcess(1);
	const Commitment excessB = MakeExcess(2);
	const OutputLocation a1 = MakeLocation(1, 1);
	const OutputLocation b2 = MakeLocation(2, 2);
	const OutputLocation a4 = MakeLocation(4, 4);
	const OutputLocation a6 = MakeLocation(6, 6);

	{
		auto pWriter = pBlockDB->Write();
		pWriter->AddKernelPosition(generation, excessA, a1);
		pWriter->AddKernelPosition(generation, excessB, b2);
		pWriter->AddKernelPosition(generation, excessA, a4);
		pWriter->AddKernelPosition(generation, excessA, a6);
		pWriter->Commit();
	}

	// Every leaf is counted, including duplicate excesses.
	REQUIRE(pBlockDB->Read()->GetKernelPositionCount(generation) == 4);

	// Latest occurrence wins, matching Rust find_kernel's backward scan.
	{
		const std::unique_ptr<OutputLocation> pLatest = pBlockDB->Read()->GetKernelPosition(generation, excessA);
		REQUIRE(pLatest != nullptr);
		REQUIRE(pLatest->GetLeafIndex().GetPosition() == a6.GetLeafIndex().GetPosition());
		REQUIRE(pLatest->GetBlockHeight() == a6.GetBlockHeight());
	}

	// Checksum is over every primary entry (all four leaves), not unique excesses.
	{
		Hash expected;
		expected = KernelIndexXorHash(expected, excessA, a1);
		expected = KernelIndexXorHash(expected, excessB, b2);
		expected = KernelIndexXorHash(expected, excessA, a4);
		expected = KernelIndexXorHash(expected, excessA, a6);
		REQUIRE(pBlockDB->Read()->GetKernelPositionChecksum(generation) == expected);
	}

	// Removing an exact position leaves earlier/later occurrences of the same
	// excess intact (no excess-only removal API).
	{
		auto pWriter = pBlockDB->Write();
		pWriter->RemoveKernelPosition(generation, excessA, a4);
		pWriter->Commit();
	}
	REQUIRE(pBlockDB->Read()->GetKernelPositionCount(generation) == 3);
	{
		const std::unique_ptr<OutputLocation> pLatest = pBlockDB->Read()->GetKernelPosition(generation, excessA);
		REQUIRE(pLatest != nullptr);
		REQUIRE(pLatest->GetLeafIndex().GetPosition() == a6.GetLeafIndex().GetPosition());
	}
	{
		auto pWriter = pBlockDB->Write();
		pWriter->RemoveKernelPosition(generation, excessA, a1);
		pWriter->Commit();
	}
	REQUIRE(pBlockDB->Read()->GetKernelPositionCount(generation) == 2);
	{
		auto pWriter = pBlockDB->Write();
		pWriter->RemoveKernelPosition(generation, excessA, a6);
		pWriter->Commit();
	}
	REQUIRE(pBlockDB->Read()->GetKernelPositionCount(generation) == 1);
	REQUIRE(pBlockDB->Read()->GetKernelPosition(generation, excessA) == nullptr);
	REQUIRE(pBlockDB->Read()->GetKernelPosition(generation, excessB) != nullptr);

	{
		auto pWriter = pBlockDB->Write();
		pWriter->ClearKernelPositions(generation);
		pWriter->Commit();
	}
	REQUIRE(pBlockDB->Read()->GetKernelPositionCount(generation) == 0);
}

namespace
{
	std::vector<rocksdb::ColumnFamilyDescriptor> ChainTables()
	{
		rocksdb::ColumnFamilyOptions options;
		options.OptimizeForPointLookup(1024);

		return {
			rocksdb::ColumnFamilyDescriptor(),
			rocksdb::ColumnFamilyDescriptor("BLOCK", options),
			rocksdb::ColumnFamilyDescriptor("HEADER", options),
			rocksdb::ColumnFamilyDescriptor("BLOCK_SUMS", options),
			rocksdb::ColumnFamilyDescriptor("OUTPUT_POS", options),
			rocksdb::ColumnFamilyDescriptor("INPUT_BITMAP", options),
			rocksdb::ColumnFamilyDescriptor("SPENT_OUTPUTS", options)
		};
	}
}

// Legacy keys from schema v1 (KERNELPOS/... and KERNELIDX/STATE) must never be
// read as the v2 index. They stay orphaned (cleanup is backlog), but a database
// containing them must still rebuild a correct v2 index at startup.
TEST_CASE("KernelIndex schema v2 ignores legacy keys")
{
	BlockHeaderPtr pTip;
	{
		TestServer::Ptr pServer = TestServer::Create();
		pServer->SetPreserveDataOnDestruct(true);
		TestMiner miner(pServer);
		KeyChain keyChain = KeyChain::FromRandom();
		const std::vector<MinedBlock> chain = miner.MineChain(keyChain, 3);
		REQUIRE(chain.size() == 3);
		pTip = pServer->GetBlockChain()->GetTipBlockHeader(EChainType::CONFIRMED);
		REQUIRE(pTip != nullptr);
	}

	{
		const fs::path dbPath = Global::GetConfig().GetDatabasePath() / "CHAIN/";
		std::unique_ptr<RocksDB> pRawDB = RocksDBFactory::Open(dbPath, ChainTables());
		const std::string table = "default";

		const std::string legacyStateKey = "KERNELIDX/STATE";
		std::string legacyPositionKey = "KERNELPOS/0000000000000001/";
		legacyPositionKey.append(33, static_cast<char>(0x01));

		const KernelIndexState legacyState(1, 1, Hash::ValueOf(7), Hash::ValueOf(7), 1, 1, Hash::ValueOf(7));
		pRawDB->Put(table, DBEntry<KernelIndexState>(rocksdb::Slice(legacyStateKey), legacyState));
		const OutputLocation legacyLocation = MakeLocation(1, 1);
		pRawDB->Put(table, DBEntry<OutputLocation>(rocksdb::Slice(legacyPositionKey), legacyLocation));

		// Remove the v2 state and every v2 position so reopening MUST rebuild the
		// index instead of reusing the committed v2 generation.
		pRawDB->Delete(table, rocksdb::Slice("KERNELIDX2/STATE"));
		std::vector<std::string> v2Keys;
		{
			auto pIterator = pRawDB->GetIterator(table);
			for (pIterator->Seek("KERNELPOS2/"); pIterator->Valid(); pIterator->Next()) {
				const std::string key = pIterator->key().ToString();
				if (key.rfind("KERNELPOS2/", 0) != 0) {
					break;
				}
				v2Keys.push_back(key);
			}
		}
		for (const std::string& key : v2Keys) {
			pRawDB->Delete(table, rocksdb::Slice(key));
		}

		// The legacy keys are still physically present; only the v2 rebuild proves
		// they are ignored.
		bool sawLegacyState = false;
		bool sawLegacyPosition = false;
		bool sawV2State = false;
		bool sawV2Position = false;
		{
			auto pIterator = pRawDB->GetIterator(table);
			for (pIterator->Seek("KERNEL"); pIterator->Valid(); pIterator->Next()) {
				const std::string key = pIterator->key().ToString();
				if (key.rfind("KERNEL", 0) != 0) {
					break;
				}
				if (key == legacyStateKey) {
					sawLegacyState = true;
				}
				if (key.rfind("KERNELPOS/", 0) == 0) {
					sawLegacyPosition = true;
				}
				if (key == "KERNELIDX2/STATE") {
					sawV2State = true;
				}
				if (key.rfind("KERNELPOS2/", 0) == 0) {
					sawV2Position = true;
				}
			}
		}
		REQUIRE(sawLegacyState);
		REQUIRE(sawLegacyPosition);
		// No v2 state/positions remain: the reopen below must rebuild from scratch.
		REQUIRE_FALSE(sawV2State);
		REQUIRE_FALSE(sawV2Position);
	}

	TestServer::Ptr pServer = TestServer::OpenExisting();
	pServer->SetPreserveDataOnDestruct(true);
	const std::unique_ptr<KernelIndexState> pState =
		pServer->GetBlockDB()->Read()->GetKernelIndexState();
	REQUIRE(pState != nullptr);
	REQUIRE(pState->GetTipHash() == pTip->GetHash());
	REQUIRE(pState->GetTipHeight() == pTip->GetHeight());
	REQUIRE(pState->GetKernelCount() == pTip->GetNumKernels());
	REQUIRE(pState->GetEntryCount() == pState->GetKernelCount());
	REQUIRE(
		pServer->GetBlockDB()->Read()->GetKernelPositionCount(pState->GetGeneration()) ==
		pTip->GetNumKernels());
	// Fresh rebuild read-back checksum matches the published state.
	REQUIRE(
		pServer->GetBlockDB()->Read()->GetKernelPositionChecksum(pState->GetGeneration()) ==
		pState->GetChecksum());
}
