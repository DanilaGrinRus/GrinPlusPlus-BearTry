#pragma once

// Shared helpers for the NRD differential and index-rebuild tests.
//
// These helpers build a synthetic kernel MMR + header chain + block DB so the
// real TxHashSet NRD code (ApplyBlock / Rewind / RebuildRecentKernelIndex) can
// be exercised without real blocks, proofs or a network. Every helper keeps the
// same position conventions as production code:
//   - header.kernel_mmr_size is LeafIndex::At(numKernels).GetPosition() (the
//     node count Rust grin stores in kernel_mmr_size),
//   - a kernel's stored position is the MMR node position of its leaf, which is
//     exactly KernelMMR::GetSize() right before ApplyKernel (verified by
//     Test_KernelLookup.cpp).

#include <catch.hpp>

#include <Core/Global.h>
#include <Core/Models/BlockHeader.h>
#include <Core/Models/BlockSums.h>
#include <Core/Models/Fee.h>
#include <Core/Models/Features.h>
#include <Core/Models/FullBlock.h>
#include <Core/Models/OutputLocation.h>
#include <Core/Models/ProofOfWork.h>
#include <Core/Models/SpentOutput.h>
#include <Core/Models/TransactionBody.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/Signature.h>
#include <Database/BlockDb.h>
#include <PMMR/Common/LeafIndex.h>
#include <PMMR/KernelMMR.h>
#include <PMMR/OutputPMMR.h>
#include <PMMR/RangeProofPMMR.h>
#include <PMMR/TxHashSetImpl.h>
#include <Consensus.h>

#include <filesystem.h>

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NrdTest
{
	constexpr uint16_t kPostHf3HeaderVersion = 4;

	inline Commitment MakeExcess(const uint8_t tag)
	{
		return Commitment(CBigInteger<33>::ValueOf(tag));
	}

	inline Signature MakeSignature(const uint8_t tag)
	{
		return Signature(CBigInteger<64>::ValueOf(tag));
	}

	inline Hash MakeHash(const uint8_t tag)
	{
		return Hash::ValueOf(tag);
	}

	// 32-byte hash with the 64-bit tag in the last 8 bytes. Header hashes must
	// stay unique for long synthetic chains (a single byte collides after 256
	// headers and breaks the previous-hash chain).
	inline Hash MakeHeaderHash(const uint64_t tag)
	{
		std::vector<unsigned char> bytes(32, 0);
		for (size_t i = 0; i < 8; ++i) {
			bytes[31 - i] = static_cast<unsigned char>((tag >> (8 * i)) & 0xFF);
		}

		return Hash(CBigInteger<32>(std::move(bytes)));
	}

	inline TransactionKernel MakeNrdKernel(const uint8_t tag, const uint64_t relativeHeight)
	{
		return TransactionKernel(
			EKernelFeatures::NO_RECENT_DUPLICATE,
			Fee(0, 0),
			relativeHeight,
			MakeExcess(tag),
			MakeSignature(tag));
	}

	inline TransactionKernel MakePlainKernel(const uint8_t tag)
	{
		return TransactionKernel(
			EKernelFeatures::DEFAULT_KERNEL,
			Fee(0, 0),
			0,
			MakeExcess(tag),
			MakeSignature(tag));
	}

	inline uint64_t MmrSizeForLeaves(const uint64_t numLeaves)
	{
		return numLeaves == 0 ? 0 : LeafIndex::At(numLeaves).GetPosition();
	}

	inline BlockHeaderPtr MakeHeader(
		const uint16_t version,
		const uint64_t height,
		const Hash& previousHash,
		const uint64_t numKernels,
		const uint64_t powTag,
		const uint64_t numOutputs = 1)
	{
		ProofOfWork proofOfWork(
			Consensus::DEFAULT_MIN_EDGE_BITS,
			std::vector<uint64_t>{ powTag + 1 },
			MakeHeaderHash(powTag));

		return std::make_shared<BlockHeader>(
			version,
			height,
			1'600'000'000 + static_cast<int64_t>(height),
			Hash(previousHash),
			Hash(HASH::ZERO),
			Hash(HASH::ZERO),
			Hash(HASH::ZERO),
			Hash(HASH::ZERO),
			BlindingFactor(),
			MmrSizeForLeaves(numOutputs),
			MmrSizeForLeaves(numKernels),
			height,
			0,
			powTag,
			std::move(proofOfWork));
	}

	// Minimal IBlockDB. Only the calls used by ApplyBlock/Rewind/Rebuild are
	// implemented; everything else is an explicit no-op so a test that reaches
	// an unexpected path fails loudly instead of silently succeeding.
	class FakeBlockDB final : public IBlockDB
	{
	public:
		uint8_t GetVersion() const final { return 3; }
		void SetVersion(const uint8_t) final { }
		void MigrateBlocks() final { }
		void Compact(const std::shared_ptr<const Chain>&) final { }

		BlockHeaderPtr GetBlockHeader(const Hash& hash) const final
		{
			if (m_throwOnGetBlockHeader) {
				throw std::runtime_error("injected local header read failure");
			}

			const auto iter = m_headers.find(hash);
			if (iter == m_headers.end()) {
				return nullptr;
			}

			return iter->second;
		}

		// Fault injection for the rebuild-exception path: makes any header read
		// throw, emulating a local disk/DB error during NRD index rebuild.
		void SetThrowOnGetBlockHeader(const bool value) noexcept { m_throwOnGetBlockHeader = value; }

		void AddBlockHeader(BlockHeaderPtr pBlockHeader) final
		{
			m_headers[pBlockHeader->GetHash()] = pBlockHeader;
		}

		void AddBlockHeaders(const std::vector<BlockHeaderPtr>& blockHeaders) final
		{
			for (const BlockHeaderPtr& pHeader : blockHeaders) {
				AddBlockHeader(pHeader);
			}
		}

		void RemoveBlockHeader(const Hash& hash)
		{
			m_headers.erase(hash);
		}

		void AddBlock(const FullBlock& block) final
		{
			m_blocks.emplace(block.GetHash(), std::make_unique<FullBlock>(block));
		}

		std::unique_ptr<FullBlock> GetBlock(const Hash& hash) const final
		{
			const auto iter = m_blocks.find(hash);
			if (iter == m_blocks.end()) {
				return nullptr;
			}

			return std::make_unique<FullBlock>(*iter->second);
		}

		void ClearBlocks() final { m_blocks.clear(); }

		void AddBlockSums(const Hash&, const BlockSums&) final { }
		std::unique_ptr<BlockSums> GetBlockSums(const Hash&) const final { return nullptr; }
		void ClearBlockSums() final { }

		void AddOutputPosition(const Commitment&, const OutputLocation&) final { }
		std::unique_ptr<OutputLocation> GetOutputPosition(const Commitment&) const final { return nullptr; }
		void RemoveOutputPositions(const std::vector<Commitment>&) final { }
		void ClearOutputPositions() final { }

		void AddSpentPositions(const Hash&, const std::vector<SpentOutput>&) final { }
		std::unordered_map<Commitment, OutputLocation> GetSpentPositions(const Hash&) const final { return {}; }
		void ClearSpentPositions() final { }

		std::unique_ptr<KernelIndexState> GetKernelIndexState() const final { return nullptr; }
		void SetKernelIndexState(const KernelIndexState&) final { }
		void AddKernelPosition(uint64_t, const Commitment&, const OutputLocation&) final { }
		std::unique_ptr<OutputLocation> GetKernelPosition(uint64_t, const Commitment&) const final { return nullptr; }
		void RemoveKernelPosition(uint64_t, const Commitment&, const OutputLocation&) final { }
		void ClearKernelPositions(uint64_t) final { }
		void ClearAllKernelPositions() final { }
		uint64_t GetKernelPositionCount(uint64_t) const final { return 0; }
		Hash GetKernelPositionChecksum(uint64_t) const final { return HASH::ZERO; }

		void Commit() final { }
		void Rollback() noexcept final { }

		std::vector<BlockHeaderPtr> GetHeaders() const
		{
			std::vector<BlockHeaderPtr> headers;
			headers.reserve(m_headers.size());
			for (const auto& entry : m_headers) {
				headers.push_back(entry.second);
			}

			return headers;
		}

		size_t GetBlockCount() const noexcept { return m_blocks.size(); }

	private:
		std::map<Hash, BlockHeaderPtr> m_headers;
		std::unordered_map<Hash, std::unique_ptr<FullBlock>> m_blocks;
		bool m_throwOnGetBlockHeader = false;
	};

	// A synthetic chain backed by real (file-based) KernelMMR/OutputPMMR/
	// RangeProofPMMR instances in an isolated temp directory.
	class ChainBuilder
	{
	public:
		static fs::path MakeTempRoot(const std::string& name)
		{
			static uint32_t counter = 0;
			fs::path root = fs::temp_directory_path() /
				("grinpp-nrd-" + name + "-" + std::to_string(++counter));
			std::error_code error;
			fs::remove_all(root, error);
			fs::create_directories(root / "kernel");
			fs::create_directories(root / "output");
			fs::create_directories(root / "rangeproof");
			return root;
		}

		// reuseExisting=true reopens an existing txhashset directory (restart).
		explicit ChainBuilder(
			const fs::path& root,
			const bool reuseExisting = false,
			const bool initializeNrdIndex = true)
			: m_root(root)
		{
			if (!reuseExisting) {
				std::error_code error;
				fs::remove_all(m_root, error);
				fs::create_directories(m_root / "kernel");
				fs::create_directories(m_root / "output");
				fs::create_directories(m_root / "rangeproof");
			}

			m_pKernelMMR = KernelMMR::Load(m_root);
			m_pOutputPMMR = OutputPMMR::Load(m_root);
			m_pRangeProofPMMR = RangeProofPMMR::Load(m_root);

			// Freeze the genesis leaves so TxHashSet::Rollback has a valid
			// baseline (production commits / reopens at this boundary).
			m_pKernelMMR->Commit();
			m_pOutputPMMR->Commit();
			m_pRangeProofPMMR->Commit();

			m_pBlockDB = std::make_shared<FakeBlockDB>();
			m_pGenesisHeader = MakeHeader(
				kPostHf3HeaderVersion,
				0,
				HASH::ZERO,
				m_pKernelMMR->GetNumKernels(),
				0);
			m_pTip = m_pGenesisHeader;
			m_pBlockDB->AddBlockHeader(m_pGenesisHeader);
			m_pTxHashSet = std::make_shared<TxHashSet>(
				m_pKernelMMR,
				m_pOutputPMMR,
				m_pRangeProofPMMR,
				m_pGenesisHeader);

			// Mirror node startup: the NRD index is rebuilt from the committed
			// tip before any block is applied. Tests that need "index missing"
			// pass initializeNrdIndex=false.
			if (initializeNrdIndex) {
				m_pTxHashSet->RebuildRecentKernelIndex(m_pBlockDB, *m_pGenesisHeader);
			}
		}

		~ChainBuilder()
		{
			// Close file handles before deleting the directory (Windows).
			m_pTxHashSet.reset();
			m_pKernelMMR.reset();
			m_pOutputPMMR.reset();
			m_pRangeProofPMMR.reset();
			m_pBlockDB.reset();

			if (m_cleanup) {
				std::error_code error;
				fs::remove_all(m_root, error);
			}
		}

		ChainBuilder(const ChainBuilder&) = delete;
		ChainBuilder& operator=(const ChainBuilder&) = delete;

		const fs::path& Root() const noexcept { return m_root; }
		TxHashSet& TxHashSetRef() const noexcept { return *m_pTxHashSet; }
		std::shared_ptr<FakeBlockDB> BlockDB() const noexcept { return m_pBlockDB; }
		BlockHeaderPtr Tip() const noexcept { return m_pTip; }
		BlockHeaderPtr GenesisHeader() const noexcept { return m_pGenesisHeader; }
		uint64_t TipHeight() const noexcept { return m_pTip->GetHeight(); }
		uint64_t KernelCount() const noexcept { return m_pKernelMMR->GetNumKernels(); }
		void SetCleanup(const bool cleanup) noexcept { m_cleanup = cleanup; }

		// Apply the next block through the real TxHashSet::ApplyBlock. The header
		// and body are stored in the fake DB. Returns ApplyBlock's result.
		bool ApplyBlock(
			const std::vector<TransactionKernel>& kernels,
			const uint16_t version = kPostHf3HeaderVersion,
			const uint64_t powTagSalt = 0)
		{
			const BlockHeaderPtr pHeader = BuildNextHeader(
				kernels, m_pKernelMMR->GetNumKernels(), version, powTagSalt);
			m_pBlockDB->AddBlockHeader(pHeader);

			TransactionBody body(
				std::vector<TransactionInput>{},
				std::vector<TransactionOutput>{},
				std::vector<TransactionKernel>(kernels));
			FullBlock block(pHeader, std::move(body));
			m_pBlockDB->AddBlock(block);

			if (!m_pTxHashSet->ApplyBlock(m_pBlockDB, block)) {
				return false;
			}

			m_pTip = pHeader;
			return true;
		}

		// Append the next block's kernels + header to the MMR/DB without applying
		// it and without storing a body: the PIBD "headers + kernel MMR only"
		// state. The caller decides when to rebuild.
		void AppendWithoutBody(
			const std::vector<TransactionKernel>& kernels,
			const uint16_t version = kPostHf3HeaderVersion,
			const bool storeBody = false,
			const uint64_t powTagSalt = 0)
		{
			const uint64_t previousKernelCount = m_pKernelMMR->GetNumKernels();
			for (const TransactionKernel& kernel : kernels) {
				m_pKernelMMR->ApplyKernel(kernel);
			}

			const BlockHeaderPtr pHeader = BuildNextHeader(kernels, previousKernelCount, version, powTagSalt);
			m_pBlockDB->AddBlockHeader(pHeader);

			if (storeBody) {
				TransactionBody body(
					std::vector<TransactionInput>{},
					std::vector<TransactionOutput>{},
					std::vector<TransactionKernel>(kernels));
				m_pBlockDB->AddBlock(FullBlock(pHeader, std::move(body)));
			}

			m_pTip = pHeader;
		}

		void Rebuild() const
		{
			m_pTxHashSet->RebuildRecentKernelIndex(m_pBlockDB, *m_pTip);
		}

		// Rebuild an alternative/uncommitted branch without advancing the
		// last-commit backup (the production reorg-batch behavior).
		void RebuildPreservingBaseline() const
		{
			m_pTxHashSet->RebuildRecentKernelIndexPreservingBaseline(m_pBlockDB, *m_pTip);
		}

		// Advance the header chain while declaring more kernels than the kernel
		// MMR actually contains (a peer can claim any kernel_mmr_size). Used to
		// prove a rebuild with a short kernel MMR is never published.
		void AppendMismatchedHeader(const uint64_t declaredKernelCount)
		{
			const BlockHeaderPtr pHeader = MakeHeader(
				kPostHf3HeaderVersion,
				m_pTip->GetHeight() + 1,
				m_pTip->GetHash(),
				declaredKernelCount,
				m_pTip->GetHeight() + 1);
			m_pBlockDB->AddBlockHeader(pHeader);
			m_pTip = pHeader;
		}

		void Commit() const
		{
			m_pTxHashSet->Commit();
		}

		void Rollback() const
		{
			m_pTxHashSet->Rollback();
		}

		void RewindTo(const BlockHeaderPtr& pHeader)
		{
			m_pTxHashSet->Rewind(m_pBlockDB, *pHeader);
			m_pTip = pHeader;
		}

		void AddHeader(const BlockHeaderPtr& pHeader) const
		{
			m_pBlockDB->AddBlockHeader(pHeader);
		}

		void SetTip(const BlockHeaderPtr& pHeader) noexcept { m_pTip = pHeader; }

		TxHashSet::ENrdValidationResult LastNrdResult() const noexcept
		{
			return m_pTxHashSet->GetLastNrdValidationResult();
		}

		bool IsIndexComplete() const noexcept
		{
			return m_pTxHashSet->IsRecentNrdIndexComplete();
		}

		size_t IndexEntryCount() const noexcept
		{
			return m_pTxHashSet->GetRecentNrdKernelCount();
		}

		std::vector<TxHashSet::RecentKernelPosition> IndexEntries(const uint8_t excessTag) const
		{
			return m_pTxHashSet->GetRecentNrdEntries(MakeExcess(excessTag));
		}

	private:
		BlockHeaderPtr BuildNextHeader(
			const std::vector<TransactionKernel>& kernels,
			const uint64_t previousKernelCount,
			const uint16_t version,
			const uint64_t powTagSalt = 0) const
		{
			const uint64_t nextHeight = m_pTip->GetHeight() + 1;
			const uint64_t nextKernelCount = previousKernelCount + kernels.size();
			return MakeHeader(
				version,
				nextHeight,
				m_pTip->GetHash(),
				nextKernelCount,
				nextHeight + powTagSalt);
		}

		fs::path m_root;
		bool m_cleanup = true;
		std::shared_ptr<KernelMMR> m_pKernelMMR;
		std::shared_ptr<OutputPMMR> m_pOutputPMMR;
		std::shared_ptr<RangeProofPMMR> m_pRangeProofPMMR;
		std::shared_ptr<FakeBlockDB> m_pBlockDB;
		std::shared_ptr<TxHashSet> m_pTxHashSet;
		BlockHeaderPtr m_pGenesisHeader;
		BlockHeaderPtr m_pTip;
	};

	// Maps a TxHashSet NRD category to the machine-readable differential
	// category used in tools/audit/nrd/cases/nrd_cases.json.
	inline std::string CategoryName(const TxHashSet::ENrdValidationResult result)
	{
		switch (result) {
		case TxHashSet::ENrdValidationResult::NONE:
			return "none";
		case TxHashSet::ENrdValidationResult::ACCEPTED:
			return "accepted";
		case TxHashSet::ENrdValidationResult::INVALID_RELATIVE_HEIGHT:
			return "invalid_nrd_relative_height";
		case TxHashSet::ENrdValidationResult::DUPLICATE_IN_BLOCK:
			return "invalid_nrd_duplicate_in_block";
		case TxHashSet::ENrdValidationResult::MISSING_LOCAL_HISTORY:
			return "missing_local_history";
		}

		return "unknown";
	}
}
