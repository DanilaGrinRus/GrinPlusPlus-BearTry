#pragma once

#include "PibdParams.h"
#include "../../PMMR/Models/BitmapChunk.h"
#include "../../PMMR/Models/InMemoryPmmr.h"
#include "../../PMMR/Models/BitmapSegment.h"
#include "../../PMMR/Models/SegmentBitmap.h"
#include "../../PMMR/Models/SegmentException.h"
#include "../../PMMR/Models/SegmentIdentifier.h"

#include "../../PMMR/Common/MMRUtil.h"

#include <Common/Util/BitUtil.h>
#include <Common/Logger.h>
#include "../../PMMR/Common/MMRHashUtil.h"
#include <Core/Serialization/Serializer.h>
#include <Crypto/Models/Hash.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// Assembles the output bitmap (the PIBD pruning oracle) from bitmap segments.
//
// Phase 1 of PIBD: the peer sends bitmap segments of the bitmap accumulator MMR
// (one leaf per 1024-bit chunk of the output bitmap). Each segment is validated
// against the bitmap MMR root from the horizon header, so a peer cannot move the
// horizon or lie about which outputs are unspent.
//
// Segment height and the "bitmap covers output leaf indices" convention follow
// grin chain/src/pibd_params.rs (BITMAP_SEGMENT_HEIGHT = 9).
class BitmapSegmentAssembler
{
public:
	static constexpr uint8_t SEGMENT_HEIGHT = PibdParams::BITMAP_SEGMENT_HEIGHT;
	static constexpr uint64_t CHUNK_CHUNKS = 1ULL << SEGMENT_HEIGHT;   // chunks per segment
	static constexpr uint64_t CHUNK_BITS = BitmapChunk::LEN_BITS;

	BitmapSegmentAssembler(const uint64_t outputMmrSize, Hash headerOutputRoot)
		: m_numOutputs(MMRUtil::GetNumLeaves(outputMmrSize)),
		m_numChunks((m_numOutputs + CHUNK_BITS - 1) / CHUNK_BITS),
		m_bitmapMmrSize(m_numChunks == 0 ? 0 : 2 * m_numChunks - (uint64_t)BitUtil::CountBitsSet(m_numChunks)),
		m_outputMmrSize(outputMmrSize),
		m_headerOutputRoot(std::move(headerOutputRoot))
	{
		const uint64_t required =
			m_bitmapMmrSize == 0 ? 0 : SegmentIdentifier::CountSegmentsRequired(m_bitmapMmrSize, SEGMENT_HEIGHT);
		m_segments.resize((size_t)required);
	}

	uint64_t GetNumOutputs() const noexcept { return m_numOutputs; }
	uint64_t GetNumChunks() const noexcept { return m_numChunks; }
	uint64_t GetBitmapMmrSize() const noexcept { return m_bitmapMmrSize; }
	size_t GetRequiredSegments() const noexcept { return m_segments.size(); }
	size_t GetReceivedSegments() const noexcept
	{
		size_t received = 0;
		for (const std::optional<std::vector<BitmapChunk>>& segment : m_segments) {
			if (segment.has_value()) {
				++received;
			}
		}

		return received;
	}

	bool IsComplete() const noexcept
	{
		for (const std::optional<std::vector<BitmapChunk>>& segment : m_segments) {
			if (!segment.has_value()) {
				return false;
			}
		}

		return true;
	}

	std::vector<SegmentIdentifier> GetMissing() const
	{
		std::vector<SegmentIdentifier> missing;
		for (size_t idx = 0; idx < m_segments.size(); ++idx) {
			if (!m_segments[idx].has_value()) {
				missing.emplace_back(SEGMENT_HEIGHT, (uint64_t)idx);
			}
		}

		return missing;
	}

	// Validate and store one bitmap segment.
	// `outputMmrRoot` is the output PMMR root carried by the response; every bitmap
	// segment must prove H(output_mmr_size || output_root || bitmap_root) equals the
	// horizon header output root (grin Segment::validate_with, other_is_left = true).
	// Returns false when the segment was already received (duplicate), true when
	// it was newly accepted. Throws SegmentException for an invalid segment.
	bool Accept(const BitmapSegment& segment, const Hash& outputMmrRoot)
	{
		const SegmentIdentifier& identifier = segment.GetIdentifier();
		if (identifier.GetHeight() != SEGMENT_HEIGHT) {
			throw SegmentException(
				ESegmentError::Mismatch,
				"Bitmap segment height " + std::to_string(identifier.GetHeight()) +
					" does not match the required " + std::to_string(SEGMENT_HEIGHT));
		}

		if (identifier.GetIdx() >= (uint64_t)m_segments.size()) {
			throw SegmentException(
				ESegmentError::Mismatch,
				"Bitmap segment index " + std::to_string(identifier.GetIdx()) + " is out of range");
		}

		if (m_segments[(size_t)identifier.GetIdx()].has_value()) {
			return false;
		}

		const Segment<BitmapChunk> pmmrSegment = segment.ToSegment();

		// The last segment can be shorter, but it must not be shorter than the
		// range it is supposed to cover.
		const uint64_t offset = identifier.GetIdx() * CHUNK_CHUNKS;
		const uint64_t expectedChunks = (std::min)(CHUNK_CHUNKS, m_numChunks - offset);
		if ((uint64_t)pmmrSegment.GetLeafData().size() != expectedChunks) {
			throw SegmentException(
				ESegmentError::Mismatch,
				"Bitmap segment " + std::to_string(identifier.GetIdx()) + " covers " +
					std::to_string(pmmrSegment.GetLeafData().size()) + " chunks, expected " +
					std::to_string(expectedChunks));
		}

		if (m_outputMmrRoot.has_value()) {
			if (m_outputMmrRoot.value() != outputMmrRoot) {
				throw SegmentException(
					ESegmentError::Mismatch, "Bitmap segments carry different output roots");
			}
		} else {
			m_outputMmrRoot = outputMmrRoot;
		}

		try {
			pmmrSegment.ValidateWithOtherRoot(
				m_bitmapMmrSize,
				nullptr,
				m_headerOutputRoot,
				m_outputMmrSize,
				outputMmrRoot,
				true);
		}
		catch (const std::exception& e) {
			const Hash bitmapRoot = pmmrSegment.Root(m_bitmapMmrSize);
			const Hash combined = MMRHashUtil::HashParentWithIndex(outputMmrRoot, bitmapRoot, m_outputMmrSize);
			LOG_WARNING_F(
				"Bitmap segment {} validation failed: {}; bitmapMmrSize={} chunks={} outputs={} bitmapRoot={} outputRoot={} combined={} expected={}",
				identifier.GetIdx(),
				e.what(),
				m_bitmapMmrSize,
				m_numChunks,
				m_numOutputs,
				bitmapRoot,
				outputMmrRoot,
				combined,
				m_headerOutputRoot);
			throw;
		}

		// The combined root is only meaningful if the segment proof reproduces the
		// bitmap MMR root, which ValidateWithOtherRoot already enforced.
		m_segments[(size_t)identifier.GetIdx()] = pmmrSegment.GetLeafData();
		return true;
	}

	// Assemble the output bitmap; requires IsComplete().
	const Hash& GetOutputMmrRoot() const
	{
		if (!m_outputMmrRoot.has_value()) {
			throw SegmentException(ESegmentError::NonExistent, "No bitmap segment received yet");
		}

		return m_outputMmrRoot.value();
	}

	// Root of the bitmap accumulator MMR assembled from the accepted segments.
	Hash BuildBitmapMmrRoot() const
	{
		if (!IsComplete()) {
			throw SegmentException(ESegmentError::NonExistent, "Bitmap segments are incomplete");
		}

		std::vector<BitmapChunk> chunks;
		for (const std::optional<std::vector<BitmapChunk>>& segment : m_segments) {
			chunks.insert(chunks.end(), segment->cbegin(), segment->cend());
		}

		const InMemoryPmmr<BitmapChunk> mmr(std::move(chunks));
		return mmr.GetRoot();
	}

	VectorSegmentBitmap BuildBitmap() const
	{
		if (!IsComplete()) {
			throw SegmentException(ESegmentError::NonExistent, "Bitmap segments are incomplete");
		}

		VectorSegmentBitmap bitmap(m_numChunks * CHUNK_BITS);
		for (size_t idx = 0; idx < m_segments.size(); ++idx) {
			const std::vector<BitmapChunk>& chunks = m_segments[idx].value();
			for (size_t chunkIdx = 0; chunkIdx < chunks.size(); ++chunkIdx) {
				const uint64_t chunkOffset = (uint64_t)idx * CHUNK_CHUNKS + (uint64_t)chunkIdx;
				for (size_t bit = 0; bit < CHUNK_BITS; ++bit) {
					if (chunks[chunkIdx].Get(bit)) {
						bitmap.Set(chunkOffset * CHUNK_BITS + (uint64_t)bit, true);
					}
				}
			}
		}

		return bitmap;
	}

private:
	uint64_t m_numOutputs;
	uint64_t m_numChunks;
	uint64_t m_bitmapMmrSize;
	uint64_t m_outputMmrSize;
	Hash m_headerOutputRoot;
	std::optional<Hash> m_outputMmrRoot;
	std::vector<std::optional<std::vector<BitmapChunk>>> m_segments;
};
