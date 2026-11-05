#pragma once

#include "../Common/MMRUtil.h"

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

// Tuple that defines a segment of a given PMMR.
// Wire format matches grin v5.5.2 core/src/core/pmmr/segment.rs.
class SegmentIdentifier : public Traits::ISerializable
{
public:
	// Segment heights used by PIBD are 9..13. Anything above this bound cannot be
	// represented by our position math, so it is rejected on the wire instead of
	// risking an out-of-range shift.
	static constexpr uint8_t MAX_SUPPORTED_HEIGHT = 32;

	SegmentIdentifier(const uint8_t height, const uint64_t idx)
		: m_height(height), m_idx(idx)
	{
	}

	uint8_t GetHeight() const noexcept { return m_height; }
	uint64_t GetIdx() const noexcept { return m_idx; }

	// Maximum number of leaves in this segment (2**height).
	uint64_t SegmentCapacity() const
	{
		if (m_height >= 64) {
			throw DESERIALIZATION_EXCEPTION("Segment height is out of range");
		}

		return 1ULL << m_height;
	}

	// Offset (in leaf index) of the first leaf in the segment.
	uint64_t LeafOffset() const
	{
		const uint64_t capacity = SegmentCapacity();
		if (m_idx > (std::numeric_limits<uint64_t>::max)() / capacity) {
			throw DESERIALIZATION_EXCEPTION("Segment leaf offset overflow");
		}
		return m_idx * capacity;
	}

	// Number of leaves in this segment: capacity, except for the final segment
	// which can be smaller.
	uint64_t SegmentUnprunedSize(const uint64_t mmrSize) const
	{
		const uint64_t capacity = SegmentCapacity();
		const uint64_t leafOffset = LeafOffset();
		const uint64_t numLeaves = MMRUtil::GetNumLeaves(mmrSize);
		if (leafOffset >= numLeaves) {
			return 0;
		}

		return (std::min)(capacity, numLeaves - leafOffset);
	}

	bool IsFullSegment(const uint64_t mmrSize) const
	{
		return SegmentUnprunedSize(mmrSize) == SegmentCapacity();
	}

	// Inclusive (full) range of MMR positions for the segment.
	std::pair<uint64_t, uint64_t> SegmentPosRange(const uint64_t mmrSize) const
	{
		const uint64_t segmentSize = SegmentUnprunedSize(mmrSize);
		const uint64_t leafOffset = LeafOffset();
		const uint64_t first = MMRUtil::InsertionToPmmrIndex(leafOffset);
		const uint64_t last = IsFullSegment(mmrSize)
			? MMRUtil::InsertionToPmmrIndex(leafOffset + segmentSize - 1) + (uint64_t)m_height
			: mmrSize - 1;

		return { first, last };
	}

	// Number of segments of the given height needed to cover an MMR of mmrSize.
	static uint64_t CountSegmentsRequired(const uint64_t mmrSize, const uint8_t height)
	{
		const uint64_t segmentCapacity = 1ULL << height;
		const uint64_t numLeaves = MMRUtil::GetNumLeaves(mmrSize);
		return (numLeaves + segmentCapacity - 1) / segmentCapacity;
	}

	void Serialize(Serializer& serializer) const final
	{
		serializer.Append<uint8_t>(m_height);
		serializer.Append<uint64_t>(m_idx);
	}

	static SegmentIdentifier Deserialize(ByteBuffer& byteBuffer)
	{
		const uint8_t height = byteBuffer.ReadU8();
		if (height > MAX_SUPPORTED_HEIGHT) {
			throw DESERIALIZATION_EXCEPTION_F("Segment height {} exceeds supported maximum", height);
		}

		const uint64_t idx = byteBuffer.ReadU64();
		return SegmentIdentifier(height, idx);
	}

	bool operator==(const SegmentIdentifier& rhs) const noexcept
	{
		return m_height == rhs.m_height && m_idx == rhs.m_idx;
	}

	bool operator!=(const SegmentIdentifier& rhs) const noexcept { return !(*this == rhs); }

private:
	uint8_t m_height;
	uint64_t m_idx;
};
