#pragma once

#include "../../PMMR/KernelMMR.h"
#include "../../PMMR/OutputPMMR.h"
#include "../../PMMR/RangeProofPMMR.h"

#include "../../PMMR/Common/MMRUtil.h"
#include "../../PMMR/Models/BitmapChunk.h"
#include "../../PMMR/Models/BitmapSegment.h"
#include "../../PMMR/Models/InMemoryPmmr.h"
#include "../../PMMR/Models/Segment.h"
#include "../../PMMR/Models/SegmentBitmap.h"
#include "../../PMMR/Models/SegmentException.h"
#include "../../PMMR/Models/SegmentGenerator.h"
#include "../../PMMR/Models/SegmentIdentifier.h"

#include <Crypto/Models/Hash.h>
#include <Common/Logger.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>

// Serves PIBD segments from the node's file-backed PMMRs.
//
// The output bitmap is derived from the output leaf set (the unspent outputs), the
// bitmap MMR is rebuilt in memory from 1024-bit chunks, and the pruned segment is
// streamed straight from the PMMR: unpruned leaves come from the data file and
// pruned-subtree roots from the hash file. Kernels are served unpruned.
class PibdSegmentService
{
public:
	PibdSegmentService(
		std::shared_ptr<OutputPMMR> pOutputPMMR,
		std::shared_ptr<RangeProofPMMR> pRangeProofPMMR,
		std::shared_ptr<KernelMMR> pKernelMMR,
		Hash horizonHash = Hash())
		: m_pOutputPMMR(std::move(pOutputPMMR)),
		m_pRangeProofPMMR(std::move(pRangeProofPMMR)),
		m_pKernelMMR(std::move(pKernelMMR)),
		m_horizonHash(std::move(horizonHash))
	{
	}

	uint64_t GetOutputMmrSize() const noexcept { return m_pOutputPMMR->GetSize(); }
	uint64_t GetKernelMmrSize() const noexcept { return m_pKernelMMR->GetSize(); }

	// Output PMMR root carried by bitmap segments; the header output_root commits
	// to it together with the bitmap accumulator root.
	Hash GetOutputMmrRoot() const { return m_pOutputPMMR->Root(m_pOutputPMMR->GetSize()); }

	Hash GetBitmapMmrRoot() const
	{
		const uint64_t size = m_pOutputPMMR->GetSize();
		const Hash outputRoot = m_pOutputPMMR->Root(size);
		GetOutputBitmap();
		{
			std::lock_guard<std::mutex> lock(s_bitmapCacheMutex);
			if (Cache().valid && Cache().horizonHash == m_horizonHash &&
				Cache().size == size && Cache().outputRoot == outputRoot &&
				Cache().hasRoot) {
				return Cache().bitmapRoot;
			}
		}

		const Hash bitmapRoot = BuildBitmapMmr().GetRoot();
		{
			std::lock_guard<std::mutex> lock(s_bitmapCacheMutex);
			if (Cache().valid && Cache().horizonHash == m_horizonHash &&
				Cache().size == size && Cache().outputRoot == outputRoot) {
				Cache().bitmapRoot = bitmapRoot;
				Cache().hasRoot = true;
			}
		}
		return bitmapRoot;
	}

	// Unspent outputs, one bit per output leaf index. The cache key contains
	// horizon hash + MMR size + output root, so it cannot be reused for a
	// different state or a reorg.
	VectorSegmentBitmap GetOutputBitmap() const
	{
		const uint64_t size = m_pOutputPMMR->GetSize();
		const Hash outputRoot = m_pOutputPMMR->Root(size);
		{
			std::lock_guard<std::mutex> lock(s_bitmapCacheMutex);
			if (Cache().valid && Cache().horizonHash == m_horizonHash &&
				Cache().size == size && Cache().outputRoot == outputRoot) {
				return Cache().bitmap;
			}
		}

		const uint64_t numLeaves = MMRUtil::GetNumLeaves(size);
		VectorSegmentBitmap bitmap(numLeaves);
		for (uint64_t leafIndex = 0; leafIndex < numLeaves; ++leafIndex) {
			if (m_pOutputPMMR->IsUnpruned(LeafIndex::At(leafIndex))) {
				bitmap.Set(leafIndex, true);
			}
		}

		{
			std::lock_guard<std::mutex> lock(s_bitmapCacheMutex);
			Cache().horizonHash = m_horizonHash;
			Cache().size = size;
			Cache().outputRoot = outputRoot;
			Cache().bitmap = bitmap;
			Cache().hasRoot = false;
			Cache().valid = true;
		}
		return bitmap;
	}

	// The bitmap accumulator MMR rebuilt from the current leaf set.
	InMemoryPmmr<BitmapChunk> BuildBitmapMmr() const
	{
		const uint64_t numLeaves = MMRUtil::GetNumLeaves(m_pOutputPMMR->GetSize());
		const uint64_t numChunks = (numLeaves + BitmapChunk::LEN_BITS - 1) / BitmapChunk::LEN_BITS;

		std::vector<BitmapChunk> chunks((size_t)numChunks);
		for (uint64_t chunkIndex = 0; chunkIndex < numChunks; ++chunkIndex) {
			for (uint64_t bit = 0; bit < BitmapChunk::LEN_BITS; ++bit) {
				const uint64_t leafIndex = chunkIndex * BitmapChunk::LEN_BITS + bit;
				if (leafIndex < numLeaves && m_pOutputPMMR->IsUnpruned(LeafIndex::At(leafIndex))) {
					chunks[(size_t)chunkIndex].Set((size_t)bit, true);
				}
			}
		}

		return InMemoryPmmr<BitmapChunk>(std::move(chunks));
	}

	BitmapSegment GetBitmapSegment(const SegmentIdentifier& identifier) const
	{
		const InMemoryPmmr<BitmapChunk> bitmapMmr = BuildBitmapMmr();
		return BitmapSegment::FromSegment(
			SegmentGenerator<BitmapChunk>::Generate(bitmapMmr, identifier));
	}

	Segment<OutputIdentifier> GetOutputSegment(const SegmentIdentifier& identifier) const
	{
		const VectorSegmentBitmap bitmap = GetOutputBitmap();
		return SegmentGenerator<OutputIdentifier>::GenerateFromSource(
			identifier,
			m_pOutputPMMR->GetSize(),
			&bitmap,
			[this](const uint64_t pos0) -> std::optional<OutputIdentifier> {
				const std::optional<uint64_t> leafIndex = MMRUtil::PmmrLeafToInsertionIndex(pos0);
				if (!leafIndex.has_value()) {
					return std::nullopt;
				}

				std::unique_ptr<OutputIdentifier> pOutput =
					m_pOutputPMMR->GetRawAt(LeafIndex::At(leafIndex.value()));
				if (pOutput == nullptr) {
					return std::nullopt;
				}

				return std::optional<OutputIdentifier>(std::move(*pOutput));
			},
			[this](const uint64_t pos0) -> std::optional<Hash> {
				return LookupHash(m_pOutputPMMR, pos0);
			});
	}

	Segment<RangeProof> GetRangeProofSegment(const SegmentIdentifier& identifier) const
	{
		const VectorSegmentBitmap bitmap = GetOutputBitmap();
		return SegmentGenerator<RangeProof>::GenerateFromSource(
			identifier,
			m_pRangeProofPMMR->GetSize(),
			&bitmap,
			[this](const uint64_t pos0) -> std::optional<RangeProof> {
				const std::optional<uint64_t> leafIndex = MMRUtil::PmmrLeafToInsertionIndex(pos0);
				if (!leafIndex.has_value()) {
					return std::nullopt;
				}

				std::unique_ptr<RangeProof> pRangeProof =
					m_pRangeProofPMMR->GetRawAt(LeafIndex::At(leafIndex.value()));
				if (pRangeProof == nullptr) {
					return std::nullopt;
				}

				return std::optional<RangeProof>(std::move(*pRangeProof));
			},
			[this](const uint64_t pos0) -> std::optional<Hash> {
				return LookupHash(m_pRangeProofPMMR, pos0);
			});
	}

	Segment<TransactionKernel> GetKernelSegment(const SegmentIdentifier& identifier) const
	{
		Segment<TransactionKernel> segment = SegmentGenerator<TransactionKernel>::GenerateFromSource(
			identifier,
			m_pKernelMMR->GetSize(),
			nullptr,
			[this](const uint64_t pos0) -> std::optional<TransactionKernel> {
				const std::optional<uint64_t> leafIndex = MMRUtil::PmmrLeafToInsertionIndex(pos0);
				if (!leafIndex.has_value()) {
					return std::nullopt;
				}

				std::unique_ptr<TransactionKernel> pKernel =
					m_pKernelMMR->GetKernelAt(LeafIndex::At(leafIndex.value()));
				if (pKernel == nullptr) {
					return std::nullopt;
				}

				return std::optional<TransactionKernel>(std::move(*pKernel));
			},
			[this](const uint64_t pos0) -> std::optional<Hash> {
				std::unique_ptr<Hash> pHash = m_pKernelMMR->GetHashAt(Index::At(pos0));
				if (pHash == nullptr) {
					return std::nullopt;
				}

				return std::optional<Hash>(std::move(*pHash));
			});

		return segment;
	}

	template<class TMMR>
	static std::optional<Hash> LookupHash(const std::shared_ptr<TMMR>& pMMR, const uint64_t pos0)
	{
		try {
			std::unique_ptr<Hash> pHash = pMMR->GetHashAt(Index::At(pos0));
			if (pHash != nullptr) {
				return std::optional<Hash>(std::move(*pHash));
			}
		} catch (const std::exception&) {
			// Missing/compacted hashes are a normal serving outcome, not a crash.
		}

		// A compacted node has no readable hash. Rust get_from_file() also returns
		// None for compacted internal positions; only a stored pruned-root hash is
		// readable through the normal non-compacted path above.
		return std::nullopt;
	}
private:
	struct BitmapCacheEntry
	{
		bool valid = false;
		bool hasRoot = false;
		Hash horizonHash;
		uint64_t size = 0;
		Hash outputRoot;
		Hash bitmapRoot;
		VectorSegmentBitmap bitmap;
	};

	inline static std::mutex s_bitmapCacheMutex;
	static BitmapCacheEntry& Cache()
	{
		static BitmapCacheEntry s_bitmapCache;
		return s_bitmapCache;
	}

	std::shared_ptr<OutputPMMR> m_pOutputPMMR;
	std::shared_ptr<RangeProofPMMR> m_pRangeProofPMMR;
	std::shared_ptr<KernelMMR> m_pKernelMMR;
	Hash m_horizonHash;
};
