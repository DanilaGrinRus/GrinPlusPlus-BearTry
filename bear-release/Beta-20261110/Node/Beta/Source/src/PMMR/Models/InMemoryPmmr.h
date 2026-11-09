#pragma once

#include "SegmentException.h"
#include "SegmentIdentifier.h"

#include "../Common/MMRHashUtil.h"
#include "../Common/MMRUtil.h"

#include <Common/Util/BitUtil.h>
#include <Core/Serialization/Serializer.h>
#include <Crypto/Models/Hash.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// In-memory PMMR used to generate and re-assemble PIBD segments.
// The representation matches grin: nodes are stored in postorder position order,
// every leaf is hashed with its position (blake2b(index || serialized element))
// and every parent with (blake2b(index || left || right)).
//
// This is a modelling/test structure; the file-backed PMMR of the node keeps its
// own representation and is used by the streaming implementation later.
template<class T>
class InMemoryPmmr
{
public:
	explicit InMemoryPmmr(std::vector<T> leaves)
		: m_leaves(std::move(leaves))
	{
		Build();
	}

	uint64_t GetSize() const noexcept { return (uint64_t)m_hashes.size(); }
	uint64_t GetNumLeaves() const noexcept { return (uint64_t)m_leaves.size(); }

	const Hash& GetHash(const uint64_t pos0) const
	{
		if (pos0 >= m_hashes.size()) {
			throw SegmentException(
				ESegmentError::MissingHash, "PMMR has no node at position " + std::to_string(pos0));
		}

		return m_hashes[(size_t)pos0];
	}

	bool IsLeafAt(const uint64_t pos0) const { return MMRUtil::IsLeaf(pos0); }

	const T& GetLeafData(const uint64_t pos0) const
	{
		if (pos0 >= m_leafIndex.size() || m_leafIndex[(size_t)pos0] < 0) {
			throw SegmentException(
				ESegmentError::MissingLeaf, "PMMR has no leaf at position " + std::to_string(pos0));
		}

		return m_leaves[(size_t)m_leafIndex[(size_t)pos0]];
	}

	// Root of the MMR: the peaks are bagged right to left, exactly as grin does.
	Hash GetRoot() const
	{
		if (m_hashes.empty()) {
			return ZERO_HASH;
		}

		Hash hash = ZERO_HASH;
		const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(GetSize());
		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			const Hash& peakHash = m_hashes[(size_t)*iter];
			hash = (hash == ZERO_HASH)
				? peakHash
				: MMRHashUtil::HashParentWithIndex(peakHash, hash, GetSize());
		}

		return hash;
	}

	// Bag the peaks strictly to the right of peakPos0 (used by segment proofs).
	std::optional<Hash> BagTheRhs(const uint64_t peakPos0) const
	{
		std::optional<Hash> result;
		const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(GetSize());
		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			const uint64_t peakPos = *iter;
			if (peakPos <= peakPos0) {
				continue;
			}

			const Hash& peakHash = m_hashes[(size_t)peakPos];
			result = result.has_value()
				? std::optional<Hash>(MMRHashUtil::HashParentWithIndex(peakHash, result.value(), GetSize()))
				: std::optional<Hash>(peakHash);
		}

		return result;
	}

private:
	void Build()
	{
		const uint64_t numLeaves = (uint64_t)m_leaves.size();
		if (numLeaves == 0) {
			return;
		}

		const uint64_t size = 2 * numLeaves - (uint64_t)BitUtil::CountBitsSet(numLeaves);
		m_hashes.reserve((size_t)size);
		m_leafIndex.assign((size_t)size, -1);

		uint64_t leafIndex = 0;
		for (uint64_t pos0 = 0; pos0 < size; ++pos0) {
			if (MMRUtil::IsLeaf(pos0)) {
				Serializer serializer;
				m_leaves[(size_t)leafIndex].Serialize(serializer);
				m_hashes.push_back(MMRHashUtil::HashLeafWithIndex(serializer.GetBytes(), pos0));
				m_leafIndex[(size_t)pos0] = (int64_t)leafIndex;
				++leafIndex;
			} else {
				const uint64_t height = MMRUtil::BintreePostorderHeight(pos0);
				const uint64_t leftChild = pos0 - (1ULL << height);
				const uint64_t rightChild = pos0 - 1;
				m_hashes.push_back(
					MMRHashUtil::HashParentWithIndex(m_hashes[(size_t)leftChild], m_hashes[(size_t)rightChild], pos0));
			}
		}
	}

	std::vector<T> m_leaves;
	std::vector<Hash> m_hashes;
	std::vector<int64_t> m_leafIndex;
};
