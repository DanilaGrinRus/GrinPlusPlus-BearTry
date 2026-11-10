#pragma once

#include "../Common/MMRUtil.h"

#include <cstdint>
#include <vector>

// Read-only view over the PIBD "output bitmap": bit i is set when the output
// with leaf index i is unspent (still in the UTXO set).
//
// grin uses a roaring bitmap here; the interface is kept minimal so the
// verification code does not depend on a specific bitmap implementation, and a
// roaring-backed adapter can be added without touching the segment logic.
class SegmentBitmap
{
public:
	virtual ~SegmentBitmap() = default;

	virtual bool Contains(uint64_t idx) const = 0;

	// Number of set bits in [start, end).
	virtual uint64_t RangeCardinality(uint64_t start, uint64_t end) const = 0;
};

// Simple vector-backed bitmap used by tests and by in-memory tooling.
class VectorSegmentBitmap : public SegmentBitmap
{
public:
	VectorSegmentBitmap() = default;

	explicit VectorSegmentBitmap(const uint64_t numBits)
		: m_bits((size_t)numBits, false)
	{
	}

	bool Contains(uint64_t idx) const final
	{
		return idx < (uint64_t)m_bits.size() && m_bits[(size_t)idx];
	}

	uint64_t RangeCardinality(uint64_t start, uint64_t end) const final
	{
		uint64_t count = 0;
		const uint64_t last = (end < (uint64_t)m_bits.size()) ? end : (uint64_t)m_bits.size();
		for (uint64_t i = start; i < last; ++i) {
			if (m_bits[(size_t)i]) {
				++count;
			}
		}

		return count;
	}

	void Set(const uint64_t idx, const bool value)
	{
		if (idx >= (uint64_t)m_bits.size()) {
			m_bits.resize((size_t)idx + 1, false);
		}

		m_bits[(size_t)idx] = value;
	}

private:
	std::vector<bool> m_bits;
};

// Whether the leaf at pos0 must be present in a segment, given the bitmap.
// Ported from grin Segment::root: a leaf is required if the bitmap has its own
// bit or its sibling's bit set, and the very last leaf of the MMR is always
// required (final segment can have an uneven number of leaves).
inline bool IsSegmentLeafRequired(const uint64_t pos0, const uint64_t mmrSize, const SegmentBitmap& bitmap)
{
	const uint64_t idx1 = MMRUtil::GetNumLeaves(pos0 + 1) - 1;
	const uint64_t idx2 = MMRUtil::IsLeftSibling(pos0) ? idx1 + 1 : idx1 - 1;
	return bitmap.Contains(idx1) || bitmap.Contains(idx2) || pos0 == mmrSize - 1;
}
