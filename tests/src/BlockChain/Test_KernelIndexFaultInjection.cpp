#include <catch.hpp>

#include <TestServer.h>
#include <TestChain.h>
#include <TxBuilder.h>

#include <BlockChain/BlockChain.h>
#include <Database/BlockDb.h>
#include <PMMR/KernelIndex.h>
#include <PMMR/TxHashSet.h>
#include <Wallet/Keychain/KeyChain.h>

#include <stdexcept>
#include <string>

namespace
{
// Safe fault injection at the index storage boundary. The proxy wraps the real
// RocksDB transaction only for index operations, so a thrown fault can be
// rolled back without touching chain blocks, headers or output/spent positions.
// It is test-only; production code has no switch that can reach it.
class ThrowingBlockDB final : public IBlockDB
{
public:
	enum class EFault
	{
		None,
		EntryBefore,
		EntryAfter,
		ReadState,
		StateAfter,
		RemoveAfter,
		CommitBefore
	};

	ThrowingBlockDB(std::shared_ptr<IBlockDB> pDB, const EFault fault)
		: m_pDB(std::move(pDB)), m_fault(fault) {}

	uint8_t GetVersion() const final { throw std::logic_error("unexpected GetVersion"); }
	void SetVersion(const uint8_t) final { throw std::logic_error("unexpected SetVersion"); }
	bool CreateCheckpoint(const fs::path&) const final { return false; }
		void MigrateBlocks() final { throw std::logic_error("unexpected MigrateBlocks"); }
	void Compact(const std::shared_ptr<const Chain>&) final { throw std::logic_error("unexpected Compact"); }
	BlockHeaderPtr GetBlockHeader(const Hash&) const final { throw std::logic_error("unexpected GetBlockHeader"); }
	void AddBlockHeader(BlockHeaderPtr) final { throw std::logic_error("unexpected AddBlockHeader"); }
	void AddBlockHeaders(const std::vector<BlockHeaderPtr>&) final { throw std::logic_error("unexpected AddBlockHeaders"); }
	void AddBlock(const FullBlock&) final { throw std::logic_error("unexpected AddBlock"); }
	std::unique_ptr<FullBlock> GetBlock(const Hash&) const final { throw std::logic_error("unexpected GetBlock"); }
	void ClearBlocks() final { throw std::logic_error("unexpected ClearBlocks"); }
	void AddBlockSums(const Hash&, const BlockSums&) final { throw std::logic_error("unexpected AddBlockSums"); }
	std::unique_ptr<BlockSums> GetBlockSums(const Hash&) const final { throw std::logic_error("unexpected GetBlockSums"); }
	void ClearBlockSums() final { throw std::logic_error("unexpected ClearBlockSums"); }
	void AddOutputPosition(const Commitment&, const OutputLocation&) final { throw std::logic_error("unexpected AddOutputPosition"); }
	std::unique_ptr<OutputLocation> GetOutputPosition(const Commitment&) const final { throw std::logic_error("unexpected GetOutputPosition"); }
	void RemoveOutputPositions(const std::vector<Commitment>&) final { throw std::logic_error("unexpected RemoveOutputPositions"); }
	void ClearOutputPositions() final { throw std::logic_error("unexpected ClearOutputPositions"); }
	void AddSpentPositions(const Hash&, const std::vector<SpentOutput>&) final { throw std::logic_error("unexpected AddSpentPositions"); }
	std::unordered_map<Commitment, OutputLocation> GetSpentPositions(const Hash&) const final { throw std::logic_error("unexpected GetSpentPositions"); }
	void ClearSpentPositions() final { throw std::logic_error("unexpected ClearSpentPositions"); }

	std::unique_ptr<KernelIndexState> GetKernelIndexState() const final
	{
		MaybeThrow(EFault::ReadState, "state-read");
		return m_pDB->GetKernelIndexState();
	}
	void SetKernelIndexState(const KernelIndexState& state) final
	{
		m_pDB->SetKernelIndexState(state);
		MaybeThrow(EFault::StateAfter, "state");
	}
	void AddKernelPosition(uint64_t generation, const Commitment& excess, const OutputLocation& location) final
	{
		MaybeThrow(EFault::EntryBefore, "entry-before");
		m_pDB->AddKernelPosition(generation, excess, location);
		++m_writes;
		MaybeThrow(EFault::EntryAfter, "entry-after");
	}
	std::unique_ptr<OutputLocation> GetKernelPosition(uint64_t generation, const Commitment& excess) const final
	{
		return m_pDB->GetKernelPosition(generation, excess);
	}
	void RemoveKernelPosition(uint64_t generation, const Commitment& excess, const OutputLocation& location) final
	{
		m_pDB->RemoveKernelPosition(generation, excess, location);
	}
	void ClearKernelPositions(uint64_t generation) final { m_pDB->ClearKernelPositions(generation); }
	void ClearAllKernelPositions() final { m_pDB->ClearAllKernelPositions(); }
	std::pair<uint64_t, Hash> RemoveKernelPositionsFrom(uint64_t generation, uint64_t firstLeafIndex) final
	{
		const auto result = m_pDB->RemoveKernelPositionsFrom(generation, firstLeafIndex);
		MaybeThrow(EFault::RemoveAfter, "rewind-delete");
		return result;
	}
	uint64_t GetKernelPositionCount(uint64_t generation) const final { return m_pDB->GetKernelPositionCount(generation); }
	Hash GetKernelPositionChecksum(uint64_t generation) const final { return m_pDB->GetKernelPositionChecksum(generation); }
	std::pair<uint64_t, Hash> GetKernelPositionDigest(uint64_t generation) const final
	{
		return m_pDB->GetKernelPositionDigest(generation);
	}
	void Commit() final
	{
		MaybeThrow(EFault::CommitBefore, "commit");
		m_pDB->Commit();
	}
	void Rollback() noexcept final { m_pDB->Rollback(); }

private:
	void MaybeThrow(const EFault point, const char* name) const
	{
		if (m_fault == point) {
			throw std::runtime_error(std::string("injected index write failure at ") + name);
		}
	}

	std::shared_ptr<IBlockDB> m_pDB;
	EFault m_fault;
	uint64_t m_writes = 0;
};

struct Fixture
{
	Fixture()
	{
		pServer = TestServer::Create();
		pChain = pServer->GetBlockChain();
		pBlockDB = pServer->GetBlockDB();
		REQUIRE(pChain->EnsureKernelIndex());
		const KeyChain keyChain = KeyChain::FromRandom();
		TxBuilder txBuilder(keyChain);
		TestChain builder(pChain);
		for (uint64_t i = 1; i <= 4; i++) {
			const auto block = builder.AddNextBlock({ txBuilder.BuildCoinbaseTx(KeyChainPath({ 0, static_cast<uint32_t>(i) })) });
			REQUIRE(pChain->AddBlock(block.block) == EBlockChainStatus::SUCCESS);
			blocks.push_back(block);
		}
		for (uint64_t height = 0; height <= pChain->GetHeight(EChainType::CONFIRMED); height++) {
			headers.push_back(pChain->GetBlockHeaderByHeight(height, EChainType::CONFIRMED));
		}
	}

	KernelIndex::HeaderLookup Lookup() const
	{
		return [this](const uint64_t height) -> BlockHeaderPtr {
			return height < headers.size() ? headers[height] : nullptr;
		};
	}

	TestServer::Ptr pServer;
	IBlockChain::Ptr pChain;
	std::shared_ptr<Locked<IBlockDB>> pBlockDB;
	std::vector<MinedBlock> blocks;
	std::vector<BlockHeaderPtr> headers;
};

void RequireSameState(const KernelIndexState& expected, const KernelIndexState& actual)
{
	REQUIRE(actual.GetGeneration() == expected.GetGeneration());
	REQUIRE(actual.GetTipHeight() == expected.GetTipHeight());
	REQUIRE(actual.GetTipHash() == expected.GetTipHash());
	REQUIRE(actual.GetKernelRoot() == expected.GetKernelRoot());
	REQUIRE(actual.GetKernelCount() == expected.GetKernelCount());
	REQUIRE(actual.GetEntryCount() == expected.GetEntryCount());
	REQUIRE(actual.GetChecksum() == expected.GetChecksum());
}
}

TEST_CASE("KernelIndex write faults never publish a partial index", "[fault][.]")
{
	SECTION("entry write")
	{
		Fixture fixture;
		const auto current = fixture.pBlockDB->Read()->GetKernelIndexState();
		REQUIRE(current != nullptr);
		const KernelIndexState stale(
			current->GetGeneration() + 100, 0, Hash(), Hash(), 0, 0, Hash());
		{
			auto pWriter = fixture.pBlockDB->BatchWrite();
			pWriter.GetShared()->SetKernelIndexState(stale);
			pWriter.GetShared()->Commit();
		}

		const auto pTxHashSet = fixture.pServer->GetTxHashSetManagerShared()->GetTxHashSet();
		const auto pTip = fixture.pChain->GetTipBlockHeader(EChainType::CONFIRMED);
		{
			auto pWriter = fixture.pBlockDB->BatchWrite();
			const auto pFaulty = std::make_shared<ThrowingBlockDB>(
				pWriter.GetShared(), ThrowingBlockDB::EFault::EntryAfter);
			REQUIRE_THROWS_WITH(
				KernelIndex::Ensure(pFaulty, pTxHashSet, pTip, fixture.Lookup()),
				Catch::Contains("injected index write failure"));
			pWriter.GetShared()->Rollback();
		}

		// The failed entry/state writes must be gone; the old state is still the
		// only published state, and the chain tip is untouched.
		RequireSameState(stale, *fixture.pBlockDB->Read()->GetKernelIndexState());
		REQUIRE(fixture.pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() ==
			fixture.blocks.back().block.GetHash());

		// A normal Ensure after the failed attempt recovers a complete index.
		REQUIRE(fixture.pChain->EnsureKernelIndex());
		const auto after = fixture.pBlockDB->Read()->GetKernelIndexState();
		REQUIRE(after != nullptr);
		REQUIRE(after->GetTipHash() == fixture.pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash());
		REQUIRE(fixture.pBlockDB->Read()->GetKernelPositionDigest(after->GetGeneration()) ==
			std::make_pair(after->GetKernelCount(), after->GetChecksum()));

		// ... and a real restart starts from the recovered state without a rebuild.
		fixture.pServer->SetPreserveDataOnDestruct(true);
		fixture.pChain = nullptr;
		fixture.pBlockDB = nullptr;
		fixture.pServer = nullptr;
		auto pRestarted = TestServer::OpenExisting();
		const auto restarted = pRestarted->GetBlockDB()->Read()->GetKernelIndexState();
		REQUIRE(restarted != nullptr);
		REQUIRE(restarted->GetTipHash() == after->GetTipHash());
		REQUIRE(restarted->GetGeneration() == after->GetGeneration());
	}

	SECTION("state read")
	{
		Fixture fixture;
		const auto before = fixture.pBlockDB->Read()->GetKernelIndexState();
		REQUIRE(before != nullptr);
		const auto pTxHashSet = fixture.pServer->GetTxHashSetManagerShared()->GetTxHashSet();
		const auto pTip = fixture.pChain->GetTipBlockHeader(EChainType::CONFIRMED);
		{
			auto pWriter = fixture.pBlockDB->BatchWrite();
			const auto pFaulty = std::make_shared<ThrowingBlockDB>(
				pWriter.GetShared(), ThrowingBlockDB::EFault::ReadState);
			REQUIRE_THROWS_WITH(
				KernelIndex::Ensure(pFaulty, pTxHashSet, pTip, fixture.Lookup()),
				Catch::Contains("injected index write failure"));
			pWriter.GetShared()->Rollback();
		}
		RequireSameState(*before, *fixture.pBlockDB->Read()->GetKernelIndexState());
		REQUIRE(fixture.pChain->GetTipBlockHeader(EChainType::CONFIRMED)->GetHash() ==
			fixture.blocks.back().block.GetHash());
	}

	SECTION("state write")
	{
		Fixture fixture;
		const auto current = fixture.pBlockDB->Read()->GetKernelIndexState();
		const KernelIndexState stale(
			current->GetGeneration() + 100, 0, Hash(), Hash(), 0, 0, Hash());
		{
			auto pWriter = fixture.pBlockDB->BatchWrite();
			pWriter.GetShared()->SetKernelIndexState(stale);
			pWriter.GetShared()->Commit();
		}

		const auto pTxHashSet = fixture.pServer->GetTxHashSetManagerShared()->GetTxHashSet();
		const auto pTip = fixture.pChain->GetTipBlockHeader(EChainType::CONFIRMED);
		{
			auto pWriter = fixture.pBlockDB->BatchWrite();
			const auto pFaulty = std::make_shared<ThrowingBlockDB>(
				pWriter.GetShared(), ThrowingBlockDB::EFault::StateAfter);
			REQUIRE_THROWS_WITH(
				KernelIndex::Ensure(pFaulty, pTxHashSet, pTip, fixture.Lookup()),
				Catch::Contains("injected index write failure"));
			pWriter.GetShared()->Rollback();
		}

		RequireSameState(stale, *fixture.pBlockDB->Read()->GetKernelIndexState());
	}

	SECTION("commit write")
	{
		Fixture fixture;
		const auto current = fixture.pBlockDB->Read()->GetKernelIndexState();
		const KernelIndexState stale(
			current->GetGeneration() + 100, 0, Hash(), Hash(), 0, 0, Hash());
		{
			auto pWriter = fixture.pBlockDB->BatchWrite();
			pWriter.GetShared()->SetKernelIndexState(stale);
			pWriter.GetShared()->Commit();
		}

		const auto pTxHashSet = fixture.pServer->GetTxHashSetManagerShared()->GetTxHashSet();
		const auto pTip = fixture.pChain->GetTipBlockHeader(EChainType::CONFIRMED);
		{
			auto pWriter = fixture.pBlockDB->BatchWrite();
			const auto pFaulty = std::make_shared<ThrowingBlockDB>(
				pWriter.GetShared(), ThrowingBlockDB::EFault::CommitBefore);
			REQUIRE_THROWS_WITH(
				KernelIndex::Ensure(pFaulty, pTxHashSet, pTip, fixture.Lookup()),
				Catch::Contains("injected index write failure"));
			pWriter.GetShared()->Rollback();
		}

		RequireSameState(stale, *fixture.pBlockDB->Read()->GetKernelIndexState());
	}

	SECTION("rewind delete")
	{
		Fixture fixture;
		const auto tipBefore = fixture.pBlockDB->Read()->GetKernelIndexState();
		REQUIRE(tipBefore != nullptr);
		const auto tipKernel = fixture.blocks.back().block.GetKernels().front();
		const auto tipLocation = fixture.pBlockDB->Read()->GetKernelPosition(
			tipBefore->GetGeneration(), tipKernel.GetExcessCommitment());
		REQUIRE(tipLocation != nullptr);

		const auto pAncestor = fixture.pChain->GetBlockHeaderByHeight(2, EChainType::CONFIRMED);
		const auto pTip = fixture.pChain->GetTipBlockHeader(EChainType::CONFIRMED);
		{
			auto pWriter = fixture.pBlockDB->BatchWrite();
			const auto pFaulty = std::make_shared<ThrowingBlockDB>(
				pWriter.GetShared(), ThrowingBlockDB::EFault::RemoveAfter);
			REQUIRE_THROWS_WITH(
				KernelIndex::RewindToAncestor(pFaulty, pAncestor, pTip, fixture.Lookup()),
				Catch::Contains("injected index write failure"));
			pWriter.GetShared()->Rollback();
		}

		// The delete was staged but the transaction was rolled back: the index is
		// still the complete tip state and the tip kernel is still addressable.
		const auto after = fixture.pBlockDB->Read()->GetKernelIndexState();
		REQUIRE(after != nullptr);
		RequireSameState(*tipBefore, *after);
		const auto afterLocation = fixture.pBlockDB->Read()->GetKernelPosition(
			after->GetGeneration(), tipKernel.GetExcessCommitment());
		REQUIRE(afterLocation != nullptr);
		REQUIRE(afterLocation->GetBlockHeight() == tipLocation->GetBlockHeight());
	}
}











