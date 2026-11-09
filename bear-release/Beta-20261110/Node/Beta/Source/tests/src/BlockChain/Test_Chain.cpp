#include <catch.hpp>

#include <TestServer.h>

#include <BlockChain/Chain.h>
#include <Consensus.h>

TEST_CASE("Chain Batching")
{
	TestServer::Ptr pTestServer = TestServer::Create();
	auto chain_path = pTestServer->GenerateTempDir() / "candidate.chain";
	auto pAllocator = std::make_shared<BlockIndexAllocator>();
	auto pGenesisIndex = pAllocator->GetOrCreateIndex(Global::GetGenesisHash(), 0);

	Hash hash1 = CSPRNG::GenerateRandom32();
	Hash hash2a = CSPRNG::GenerateRandom32();
	Hash hash2b = CSPRNG::GenerateRandom32();
	Hash hash3b = CSPRNG::GenerateRandom32();
	Hash hash4b = CSPRNG::GenerateRandom32();

	Locked<Chain> chain(Chain::Load(pAllocator, EChainType::CANDIDATE, chain_path, pGenesisIndex));

	{
		auto pBatch = chain.BatchWrite();
		pBatch->AddBlock(hash1, 1);
		pBatch->AddBlock(hash2a, 2);
		pBatch->Commit();
	}

	{
		auto pBatch = chain.BatchWrite();
		pBatch->Rewind(1);
		pBatch->AddBlock(hash2b, 2);
		pBatch->Rollback();
	}

	{
		auto pBatch = chain.BatchWrite();
		pBatch->Rewind(1);
		pBatch->AddBlock(hash2b, 2);
		pBatch->AddBlock(hash3b, 3);
		pBatch->AddBlock(hash4b, 4);
		pBatch->Commit();
	}

	{
		REQUIRE_THROWS(chain.BatchWrite()->AddBlock(hash4b, 4));
	}

	{
		auto pReader = chain.Read();
		REQUIRE(pReader->GetHeight() == 4);
		REQUIRE(pReader->GetTipHash() == hash4b);
		for (size_t i = 0; i <= 4; i++)
		{
			REQUIRE(pReader->GetByHeight(i)->GetHeight() == i);
		}

		REQUIRE(pReader->GetByHeight(0)->GetHash() == Global::GetGenesisHash());
		REQUIRE(pReader->GetByHeight(1)->GetHash() == hash1);
		REQUIRE(pReader->GetByHeight(2)->GetHash() == hash2b);
		REQUIRE(pReader->GetByHeight(3)->GetHash() == hash3b);
		REQUIRE(pReader->GetByHeight(4)->GetHash() == hash4b);
	}
}

TEST_CASE("Compact removes only full blocks below the horizon")
{
	TestServer::Ptr pTestServer = TestServer::Create();
	pTestServer->MineChain(3);

	const IBlockChain::Ptr pBlockChain = pTestServer->GetBlockChain();
	const FullBlock& genesis = Global::GetGenesisBlock();
	const auto pBlock1 = pBlockChain->GetBlockByHeight(1);
	const auto pBlock2 = pBlockChain->GetBlockByHeight(2);
	REQUIRE(pBlock1 != nullptr);
	REQUIRE(pBlock2 != nullptr);

	// Genesis is normally stored as header/sums only; insert its body so the
	// test can prove the exact below-horizon deletion and preservation of 1/2.
	{
		auto pSeedDB = pTestServer->GetBlockDB()->Write();
		pSeedDB->AddBlock(genesis);
		pSeedDB->Commit();
	}

	// Only the chain height participates in BlockDB::Compact's horizon
	// calculation. Build a synthetic high chain without mining 10k blocks.
	auto pAllocator = std::make_shared<BlockIndexAllocator>();
	auto pGenesisIndex = pAllocator->GetOrCreateIndex(Global::GetGenesisHash(), 0);
	auto chainPath = pTestServer->GenerateTempDir() / "compact.chain";
	Locked<Chain> fakeChain(Chain::Load(
		pAllocator, EChainType::CANDIDATE, chainPath, pGenesisIndex));
	{
		auto pBatch = fakeChain.BatchWrite();
		for (uint64_t height = 1; height <= Consensus::CUT_THROUGH_HORIZON + 1; height++) {
			pBatch->AddBlock(CSPRNG::GenerateRandom32(), height);
		}
		pBatch->Commit();
	}

	auto pConfirmed = fakeChain.Read().GetShared();
	auto pBlockDB = pTestServer->GetBlockDB()->Write();
	pBlockDB->Compact(pConfirmed);
	pBlockDB->Commit();

	REQUIRE(pBlockDB->GetBlock(genesis.GetHash()) == nullptr);
	REQUIRE(pBlockDB->GetBlock(pBlock1->GetHash()) != nullptr);
	REQUIRE(pBlockDB->GetBlock(pBlock2->GetHash()) != nullptr);
}

TEST_CASE("Compact refuses a missing chain without deleting data")
{
	TestServer::Ptr pTestServer = TestServer::Create();
	pTestServer->MineChain(2);
	const IBlockChain::Ptr pBlockChain = pTestServer->GetBlockChain();
	const auto pBlock1 = pBlockChain->GetBlockByHeight(1);
	REQUIRE(pBlock1 != nullptr);

	auto pBlockDB = pTestServer->GetBlockDB()->Write();
	REQUIRE_THROWS(pBlockDB->Compact(std::shared_ptr<const Chain>()));
	pBlockDB->Commit();

	REQUIRE(pBlockDB->GetBlock(pBlock1->GetHash()) != nullptr);
}