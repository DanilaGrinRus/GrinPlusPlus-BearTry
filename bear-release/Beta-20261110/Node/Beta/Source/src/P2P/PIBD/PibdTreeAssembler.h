#pragma once

#include "../../PMMR/Common/MMRUtil.h"
#include "../../PMMR/Models/Segment.h"
#include "../../PMMR/Models/SegmentBitmap.h"
#include "../../PMMR/Models/SegmentException.h"
#include "../../PMMR/Models/SegmentIdentifier.h"

#include <Crypto/Models/Hash.h>

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// Validates and stores the segments of one PIBD tree (outputs, rangeproofs or
// kernels) at a fixed horizon.
//
// Each segment is checked against the tree root from the horizon header, with
// the output bitmap for the pruned trees. The union of the accepted segments
// covers the tree exactly once (segments partition the MMR positions), so a
// completely assembled tree is fully authenticated.
template<class T>
class PibdTreeAssembler
{
public:
	PibdTreeAssembler(
		const uint64_t mmrSize,
		Hash root,
		const uint8_t segmentHeight,
		const SegmentBitmap* bitmap,
		std::optional<Hash> otherRoot = std::nullopt,
		uint64_t hashLastPos = 0,
		bool otherIsLeft = false
	)
		: m_mmrSize(mmrSize),
		m_root(std::move(root)),
		m_segmentHeight(segmentHeight),
		m_bitmap(bitmap),
		m_otherRoot(std::move(otherRoot)),
		m_hashLastPos(hashLastPos),
		m_otherIsLeft(otherIsLeft),
		m_required(mmrSize == 0 ? 0 : SegmentIdentifier::CountSegmentsRequired(mmrSize, segmentHeight))
	{
		m_segments.resize((size_t)m_required);
	}

	bool IsPruned() const noexcept { return m_bitmap != nullptr; }
	uint64_t GetMmrSize() const noexcept { return m_mmrSize; }
	uint8_t GetSegmentHeight() const noexcept { return m_segmentHeight; }
	size_t GetRequiredSegments() const noexcept { return (size_t)m_required; }

	size_t GetReceivedSegments() const noexcept
	{
		size_t received = 0;
		for (const std::optional<std::vector<std::pair<uint64_t, T>>>& segment : m_segments) {
			if (segment.has_value()) {
				++received;
			}
		}

		return received;
	}

	bool IsComplete() const noexcept
	{
		for (const std::optional<std::vector<std::pair<uint64_t, T>>>& segment : m_segments) {
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
				missing.emplace_back(m_segmentHeight, (uint64_t)idx);
			}
		}

		return missing;
	}

	// Validate and store a segment. Returns false for a duplicate, true when
	// newly accepted; throws SegmentException for an invalid segment.
	bool Accept(const Segment<T>& segment)
	{
		const SegmentIdentifier& identifier = segment.GetIdentifier();
		if (identifier.GetHeight() != m_segmentHeight) {
			throw SegmentException(
				ESegmentError::Mismatch,
				"Segment height " + std::to_string(identifier.GetHeight()) + " does not match the required " +
					std::to_string(m_segmentHeight));
		}

		if (identifier.GetIdx() >= m_required) {
			throw SegmentException(
				ESegmentError::Mismatch,
				"Segment index " + std::to_string(identifier.GetIdx()) + " is out of range");
		}

		std::optional<std::vector<std::pair<uint64_t, T>>>& slot = m_segments[(size_t)identifier.GetIdx()];
		if (slot.has_value()) {
			return false;
		}

		if (m_bitmap != nullptr && m_otherRoot.has_value()) {
			// Root is combined with another root before the final comparison.
			segment.ValidateWithOtherRoot(
				m_mmrSize, m_bitmap, m_root, m_hashLastPos, m_otherRoot.value(), m_otherIsLeft);
		} else if (m_bitmap != nullptr) {
			segment.ValidateWithBitmap(m_mmrSize, *m_bitmap, m_root);
		} else {
			segment.Validate(m_mmrSize, m_root);
		}

		std::vector<std::pair<uint64_t, T>> leaves;
		leaves.reserve(segment.GetLeafPositions().size());
		for (size_t i = 0; i < segment.GetLeafPositions().size(); ++i) {
			const uint64_t pos0 = segment.GetLeafPositions()[i];
			// A serving peer may include data for spent leaves too (grin's
			// Segment::from_pmmr does). The bitmap still defines which leaves
			// belong to the unspent set that this assembler publishes.
			if (m_bitmap != nullptr && !IsSegmentLeafRequired(pos0, m_mmrSize, *m_bitmap)) {
				continue;
			}
			leaves.emplace_back(pos0, segment.GetLeafData()[i]);
		}

		for (size_t i = 0; i < segment.GetHashPositions().size(); ++i) {
			const uint64_t pos0 = segment.GetHashPositions()[i];
			const Hash& hash = segment.GetHashes()[i];
			const auto existing = m_storedHashes.find(pos0);
			if (existing != m_storedHashes.end() && existing->second != hash) {
				throw SegmentException(
					ESegmentError::Mismatch,
					"Segment provides a different hash for position " + std::to_string(pos0));
			}

			m_storedHashes[pos0] = hash;
		}

		slot = std::move(leaves);
		return true;
	}

	// Number of leaves this tree must contain: every leaf for an unpruned tree,
	// only the bitmap-required leaves for a pruned one.
	uint64_t GetExpectedLeafCount() const
	{
		const uint64_t numLeaves = MMRUtil::GetNumLeaves(m_mmrSize);
		if (m_bitmap == nullptr) {
			return numLeaves;
		}

		uint64_t required = 0;
		for (uint64_t leafIdx = 0; leafIdx < numLeaves; ++leafIdx) {
			const uint64_t pos0 = MMRUtil::InsertionToPmmrIndex(leafIdx);
			if (IsSegmentLeafRequired(pos0, m_mmrSize, *m_bitmap)) {
				++required;
			}
		}

		return required;
	}

	uint64_t GetStoredLeafCount() const
	{
		uint64_t stored = 0;
		for (const std::optional<std::vector<std::pair<uint64_t, T>>>& segment : m_segments) {
			if (segment.has_value()) {
				stored += (uint64_t)segment->size();
			}
		}

		return stored;
	}

	// Internal subtree hashes carried by the segments; needed to rebuild the
	// tree with pruned leaves (they are written to the PMMR hash file).
	const std::map<uint64_t, Hash>& GetStoredHashes() const noexcept { return m_storedHashes; }

	// Leaves ordered by MMR position; only valid when IsComplete().
	std::vector<std::pair<uint64_t, T>> GetLeaves() const
	{
		if (!IsComplete()) {
			throw SegmentException(ESegmentError::NonExistent, "Tree segments are incomplete");
		}

		std::vector<std::pair<uint64_t, T>> leaves;
		leaves.reserve((size_t)GetStoredLeafCount());
		for (const std::optional<std::vector<std::pair<uint64_t, T>>>& segment : m_segments) {
			leaves.insert(leaves.end(), segment->cbegin(), segment->cend());
		}

		return leaves;
	}

private:
	uint64_t m_mmrSize;
	Hash m_root;
	uint8_t m_segmentHeight;
	const SegmentBitmap* m_bitmap;
	std::optional<Hash> m_otherRoot;
	uint64_t m_hashLastPos;
	bool m_otherIsLeft;
	uint64_t m_required;
	std::vector<std::optional<std::vector<std::pair<uint64_t, T>>>> m_segments;
	std::map<uint64_t, Hash> m_storedHashes;
};
