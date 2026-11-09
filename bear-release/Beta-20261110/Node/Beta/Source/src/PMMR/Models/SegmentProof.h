#pragma once

#include "SegmentException.h"

#include "../Common/MMRHashUtil.h"
#include "../Common/MMRUtil.h"

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>
#include <Crypto/Models/Hash.h>

#include <cstdint>
#include <vector>

// Merkle proof of a segment.
// Wire format matches grin v5.5.2 core/src/core/pmmr/segment.rs (SegmentProof).
class SegmentProof : public Traits::ISerializable
{
public:
	// Same bound as grin: MAX_SEGMENT_READ_ITEMS.
	static constexpr uint64_t MAX_HASHES = 1000000;

	SegmentProof(std::vector<Hash> hashes)
		: m_hashes(std::move(hashes))
	{
	}

	static SegmentProof Empty()
	{
		return SegmentProof(std::vector<Hash>());
	}

	const std::vector<Hash>& GetHashes() const noexcept { return m_hashes; }
	size_t Size() const noexcept { return m_hashes.size(); }

	void Serialize(Serializer& serializer) const final
	{
		serializer.Append<uint64_t>((uint64_t)m_hashes.size());
		for (const Hash& hash : m_hashes) {
			serializer.AppendBigInteger(hash);
		}
	}

	static SegmentProof Deserialize(ByteBuffer& byteBuffer)
	{
		const uint64_t numHashes = byteBuffer.ReadU64();
		if (numHashes > MAX_HASHES) {
			throw DESERIALIZATION_EXCEPTION_F("Segment proof declares {} hashes, limit is {}", numHashes, MAX_HASHES);
		}

		if (numHashes > byteBuffer.GetRemainingSize() / (uint64_t)HASH_SIZE) {
			throw DESERIALIZATION_EXCEPTION("Segment proof declares more hashes than the payload can hold");
		}

		std::vector<Hash> hashes;
		// Same preallocation cap as grin (SEGMENT_READ_PREALLOC_ITEMS): the
		// declared count is attacker-controlled until the payload is read.
		hashes.reserve((size_t)(numHashes < 1024 ? numHashes : 1024));
		for (uint64_t i = 0; i < numHashes; ++i) {
			hashes.push_back(byteBuffer.ReadBigInteger<32>());
		}

		return SegmentProof(std::move(hashes));
	}

	// Reconstruct the MMR root from this proof and the segment root.
	// Ported from grin v5.5.2 SegmentProof::reconstruct_root.
	Hash ReconstructRoot(
		const uint64_t lastPos,
		const uint64_t segmentFirstPos0,
		const uint64_t segmentLastPos0,
		const Hash& segmentRoot,
		const uint64_t segmentUnprunedPos
	) const
	{
		size_t nextHash = 0;
		const std::vector<std::pair<uint64_t, uint64_t>> familyBranch =
			MMRUtil::FamilyBranch(segmentLastPos0, lastPos);

		// 1. Siblings along the path from the subtree root to the peak.
		Hash root = segmentRoot;
		for (const std::pair<uint64_t, uint64_t>& branch : familyBranch) {
			const uint64_t parentPos0 = branch.first;
			const uint64_t siblingPos0 = branch.second;
			if (parentPos0 < segmentUnprunedPos) {
				continue;
			}

			if (nextHash >= m_hashes.size()) {
				throw SegmentException(
					ESegmentError::MissingHash,
					"Segment proof is missing the hash for sibling " + std::to_string(1 + siblingPos0));
			}

			const Hash& siblingHash = m_hashes[nextHash++];
			root = MMRUtil::IsLeftSibling(siblingPos0)
				? MMRHashUtil::HashParentWithIndex(siblingHash, root, parentPos0)
				: MMRHashUtil::HashParentWithIndex(root, siblingHash, parentPos0);
		}

		// 2. Bagged peaks to the right.
		const uint64_t peakPos0 = familyBranch.empty() ? segmentLastPos0 : familyBranch.back().first;
		const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(lastPos);
		for (const uint64_t pos0 : peaks) {
			if (pos0 > peakPos0) {
				if (nextHash >= m_hashes.size()) {
					throw SegmentException(
						ESegmentError::MissingHash,
						"Segment proof is missing the hash for peak " + std::to_string(1 + pos0));
				}

				root = MMRHashUtil::HashParentWithIndex(root, m_hashes[nextHash++], lastPos);
				break;
			}
		}

		// 3. Peaks to the left (right to left).
		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			const uint64_t pos0 = *iter;
			if (pos0 >= segmentFirstPos0) {
				continue;
			}

			if (nextHash >= m_hashes.size()) {
				throw SegmentException(
					ESegmentError::MissingHash,
					"Segment proof is missing the hash for peak " + std::to_string(1 + pos0));
			}

			root = MMRHashUtil::HashParentWithIndex(m_hashes[nextHash++], root, lastPos);
		}

		return root;
	}

	// Validate this proof when the final hashing step also includes `otherRoot`
	// (grin PIBD: header output_root commits to output PMMR root and bitmap root).
	void ValidateWith(
		const uint64_t lastPos,
		const Hash& mmrRoot,
		const uint64_t segmentFirstPos0,
		const uint64_t segmentLastPos0,
		const Hash& segmentRoot,
		const uint64_t segmentUnprunedPos,
		const uint64_t hashLastPos,
		const Hash& otherRoot,
		const bool otherIsLeft
	) const
	{
		Hash root = ReconstructRoot(
			lastPos, segmentFirstPos0, segmentLastPos0, segmentRoot, segmentUnprunedPos);
		root = otherIsLeft
			? MMRHashUtil::HashParentWithIndex(otherRoot, root, hashLastPos)
			: MMRHashUtil::HashParentWithIndex(root, otherRoot, hashLastPos);

		if (root != mmrRoot) {
			throw SegmentException(
				ESegmentError::Mismatch, "Segment proof does not reconstruct the combined MMR root");
		}
	}

	// Validate this proof by equating the reconstructed root with the actual one.
	void Validate(
		const uint64_t lastPos,
		const Hash& mmrRoot,
		const uint64_t segmentFirstPos0,
		const uint64_t segmentLastPos0,
		const Hash& segmentRoot,
		const uint64_t segmentUnprunedPos
	) const
	{
		const Hash root = ReconstructRoot(
			lastPos, segmentFirstPos0, segmentLastPos0, segmentRoot, segmentUnprunedPos);
		if (root != mmrRoot) {
			throw SegmentException(ESegmentError::Mismatch, "Segment proof does not reconstruct the MMR root");
		}
	}

private:
	std::vector<Hash> m_hashes;
};
