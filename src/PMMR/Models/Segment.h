#pragma once

#include "SegmentBitmap.h"
#include "SegmentException.h"
#include "SegmentIdentifier.h"
#include "SegmentProof.h"

#include "../Common/MMRHashUtil.h"
#include "../Common/MMRUtil.h"

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>
#include <Crypto/Models/Hash.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SegmentDetail
{
	// Same bound as grin: MAX_SEGMENT_READ_ITEMS.
	static constexpr uint64_t MAX_SEGMENT_READ_ITEMS = 1000000;

	inline void CheckItemCount(const uint64_t count)
	{
		if (count > MAX_SEGMENT_READ_ITEMS) {
			throw DESERIALIZATION_EXCEPTION_F(
				"Segment declares {} items, limit is {}", count, MAX_SEGMENT_READ_ITEMS);
		}
	}

	// Positions are 1-based on the wire and strictly increasing; they are stored
	// 0-based internally (same convention as grin).
	inline std::vector<uint64_t> ReadPositions(ByteBuffer& byteBuffer, const uint64_t count)
	{
		if (count > byteBuffer.GetRemainingSize() / sizeof(uint64_t)) {
			throw DESERIALIZATION_EXCEPTION("Segment position list does not fit in the payload");
		}

		std::vector<uint64_t> positions;
		positions.reserve((size_t)count);
		uint64_t lastPos = 0;
		for (uint64_t i = 0; i < count; ++i) {
			const uint64_t pos = byteBuffer.ReadU64();
			if (pos <= lastPos) {
				throw DESERIALIZATION_EXCEPTION("Segment positions are not strictly increasing");
			}

			lastPos = pos;
			positions.push_back(pos - 1);
		}

		return positions;
	}

	inline void WritePositions(Serializer& serializer, const std::vector<uint64_t>& positions)
	{
		serializer.Append<uint64_t>((uint64_t)positions.size());
		for (const uint64_t pos : positions) {
			serializer.Append<uint64_t>(pos + 1);
		}
	}
}

// Segment of a PMMR: unpruned leaves and the data required to verify membership
// in the original MMR. Wire format matches grin v5.5.2 Segment<T>.
template<class T>
class Segment : public Traits::ISerializable
{
public:
	Segment(
		SegmentIdentifier identifier,
		std::vector<uint64_t> hashPositions,
		std::vector<Hash> hashes,
		std::vector<uint64_t> leafPositions,
		std::vector<T> leafData,
		SegmentProof proof
	)
		: m_identifier(identifier),
		m_hashPositions(std::move(hashPositions)),
		m_hashes(std::move(hashes)),
		m_leafPositions(std::move(leafPositions)),
		m_leafData(std::move(leafData)),
		m_proof(std::move(proof))
	{
	}

	const SegmentIdentifier& GetIdentifier() const noexcept { return m_identifier; }
	const std::vector<uint64_t>& GetHashPositions() const noexcept { return m_hashPositions; }
	const std::vector<Hash>& GetHashes() const noexcept { return m_hashes; }
	const std::vector<uint64_t>& GetLeafPositions() const noexcept { return m_leafPositions; }
	const std::vector<T>& GetLeafData() const noexcept { return m_leafData; }
	const SegmentProof& GetProof() const noexcept { return m_proof; }

	const T& GetLeafDataAt(const uint64_t pos0) const
	{
		for (size_t i = 0; i < m_leafPositions.size(); ++i) {
			if (m_leafPositions[i] == pos0) {
				return m_leafData[i];
			}
		}

		throw SegmentException(
			ESegmentError::MissingLeaf, "Segment is missing the leaf at position " + std::to_string(pos0));
	}

	// Root hash of this segment, computed from its leaves and (for pruned data)
	// the stored hashes. Ported from grin Segment::root for the non-prunable
	// case (a peer that serves full segments).
	Hash Root(const uint64_t mmrSize) const
	{
		const std::pair<uint64_t, uint64_t> range = m_identifier.SegmentPosRange(mmrSize);
		std::vector<Hash> hashes;
		for (uint64_t pos0 = range.first; ; ++pos0) {
			const uint64_t height = MMRUtil::BintreePostorderHeight(pos0);
			if (height == 0) {
				Serializer leafSerializer;
				GetLeafDataAt(pos0).Serialize(leafSerializer);
				hashes.push_back(MMRHashUtil::HashLeafWithIndex(leafSerializer.GetBytes(), pos0));
			} else {
				if (hashes.size() < 2) {
					throw SegmentException(
						ESegmentError::MissingHash,
						"Segment is missing a child hash below position " + std::to_string(pos0));
				}

				const Hash rightChild = hashes.back();
				hashes.pop_back();
				const Hash leftChild = hashes.back();
				hashes.pop_back();
				hashes.push_back(MMRHashUtil::HashParentWithIndex(leftChild, rightChild, pos0));
			}

			if (pos0 == range.second) {
				break;
			}
		}

		if (m_identifier.IsFullSegment(mmrSize)) {
			if (hashes.size() != 1) {
				throw SegmentException(ESegmentError::Mismatch, "Full segment did not reduce to a single hash");
			}

			return hashes.back();
		}

		// Final (partial) segment: bag the peaks contained in the segment range,
		// right to left, exactly as grin does.
		const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(mmrSize);
		std::optional<Hash> root;
		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			const uint64_t peakPos0 = *iter;
			if (peakPos0 < range.first || peakPos0 > range.second) {
				continue;
			}

			if (hashes.empty()) {
				throw SegmentException(
					ESegmentError::MissingHash,
					"Segment is missing the peak hash at position " + std::to_string(1 + peakPos0));
			}

			const Hash peakHash = hashes.back();
			hashes.pop_back();
			root = root.has_value()
				? MMRHashUtil::HashParentWithIndex(peakHash, root.value(), mmrSize)
				: peakHash;
		}

		if (!root.has_value()) {
			throw SegmentException(ESegmentError::NonExistent, "Segment contains no peaks for this MMR size");
		}

		return root.value();
	}

	// First unpruned parent hash of this segment and its 1-based position.
	std::pair<Hash, uint64_t> FirstUnprunedParent(const uint64_t mmrSize) const
	{
		const Hash root = Root(mmrSize);
		const std::pair<uint64_t, uint64_t> range = m_identifier.SegmentPosRange(mmrSize);
		return { root, range.second + 1 };
	}

	// Validate this segment against the MMR root by recomputing its root and
	// checking the merkle proof.
	void Validate(const uint64_t mmrSize, const Hash& mmrRoot) const
	{
		const std::pair<uint64_t, uint64_t> range = m_identifier.SegmentPosRange(mmrSize);
		const std::pair<Hash, uint64_t> segmentRoot = FirstUnprunedParent(mmrSize);
		m_proof.Validate(mmrSize, mmrRoot, range.first, range.second, segmentRoot.first, segmentRoot.second);
	}

	// Hash stored in this segment at the given 0-based MMR position.
	Hash GetHashAt(const uint64_t pos0) const
	{
		for (size_t i = 0; i < m_hashPositions.size(); ++i) {
			if (m_hashPositions[i] == pos0) {
				return m_hashes[i];
			}
		}

		throw SegmentException(
			ESegmentError::MissingHash, "Segment has no hash at position " + std::to_string(pos0));
	}

	// Bitmap-aware root for a prunable MMR: leaves that the bitmap does not
	// require are absent and their subtree hashes come from this segment.
	// Returns nullopt only for a full segment that is completely pruned.
	// Ported from grin Segment::root.
	std::optional<Hash> RootWithBitmap(const uint64_t mmrSize, const SegmentBitmap& bitmap) const
	{
		const std::pair<uint64_t, uint64_t> range = m_identifier.SegmentPosRange(mmrSize);
		std::vector<std::optional<Hash>> hashes;
		for (uint64_t pos0 = range.first; ; ++pos0) {
			const uint64_t height = MMRUtil::BintreePostorderHeight(pos0);
			std::optional<Hash> hash;
			if (height == 0) {
				if (IsSegmentLeafRequired(pos0, mmrSize, bitmap)) {
					Serializer serializer;
					GetLeafDataAt(pos0).Serialize(serializer);
					hash = MMRHashUtil::HashLeafWithIndex(serializer.GetBytes(), pos0);
				}
			} else {
				if (hashes.size() < 2) {
					throw SegmentException(
						ESegmentError::MissingHash,
						"Segment is missing a child hash below position " + std::to_string(pos0));
				}

				const std::optional<Hash> rightChild = hashes.back();
				hashes.pop_back();
				const std::optional<Hash> leftChild = hashes.back();
				hashes.pop_back();

				if (leftChild.has_value() && rightChild.has_value()) {
					hash = MMRHashUtil::HashParentWithIndex(
						leftChild.value(), rightChild.value(), pos0);
				} else if (!leftChild.has_value() && !rightChild.has_value()) {
					hash = std::nullopt;
				} else if (!leftChild.has_value()) {
					const uint64_t leftChildPos = pos0 - (1ULL << height);
					hash = MMRHashUtil::HashParentWithIndex(
						GetHashAt(leftChildPos), rightChild.value(), pos0);
				} else {
					const uint64_t rightChildPos = pos0 - 1;
					hash = MMRHashUtil::HashParentWithIndex(
						leftChild.value(), GetHashAt(rightChildPos), pos0);
				}
			}

			hashes.push_back(hash);
			if (pos0 == range.second) {
				break;
			}
		}

		if (m_identifier.IsFullSegment(mmrSize)) {
			if (hashes.size() != 1) {
				throw SegmentException(ESegmentError::Mismatch, "Full segment did not reduce to a single hash");
			}

			return hashes.back();
		}

		const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(mmrSize);
		std::optional<Hash> root;
		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			const uint64_t peakPos0 = *iter;
			if (peakPos0 < range.first || peakPos0 > range.second) {
				continue;
			}

			if (hashes.empty()) {
				throw SegmentException(
					ESegmentError::MissingHash,
					"Segment is missing the peak hash at position " + std::to_string(1 + peakPos0));
			}

			std::optional<Hash> peakHash = hashes.back();
			hashes.pop_back();
			if (!peakHash.has_value()) {
				peakHash = GetHashAt(peakPos0);
			}

			root = root.has_value()
				? MMRHashUtil::HashParentWithIndex(peakHash.value(), root.value(), mmrSize)
				: peakHash;
		}

		if (!root.has_value()) {
			throw SegmentException(ESegmentError::NonExistent, "Segment contains no peaks for this MMR size");
		}

		return root;
	}

	// First unpruned parent hash of this segment and its 1-based position, for a
	// prunable MMR. When the segment root is fully pruned the hash is looked up
	// along the family branch. Ported from grin Segment::first_unpruned_parent.
	std::pair<Hash, uint64_t> FirstUnprunedParentWithBitmap(
		const uint64_t mmrSize, const SegmentBitmap& bitmap) const
	{
		const std::optional<Hash> root = RootWithBitmap(mmrSize, bitmap);
		const std::pair<uint64_t, uint64_t> range = m_identifier.SegmentPosRange(mmrSize);
		if (root.has_value()) {
			return { root.value(), range.second + 1 };
		}

		const uint64_t numLeaves = MMRUtil::GetNumLeaves(mmrSize);
		uint64_t cardinality = 0;
		uint64_t pos0 = range.second;
		const std::vector<std::pair<uint64_t, uint64_t>> branch = MMRUtil::FamilyBranch(range.second, mmrSize);
		size_t nextBranch = 0;
		while (cardinality == 0) {
			try {
				return { GetHashAt(pos0), pos0 + 1 };
			} catch (const SegmentException&) {
				// No stored hash at this position; keep walking up.
			}

			if (nextBranch >= branch.size()) {
				break;
			}

			pos0 = branch[nextBranch].first;
			++nextBranch;
			const uint64_t leftmost = MMRUtil::BintreeLeftmost(pos0);
			const uint64_t rightmost = MMRUtil::BintreeRightmost(pos0);
			const uint64_t start = MMRUtil::GetNumLeaves(1 + leftmost) - 1;
			const uint64_t end = (std::min)(MMRUtil::GetNumLeaves(1 + rightmost), numLeaves);
			cardinality = bitmap.RangeCardinality(start, end);
		}

		throw SegmentException(
			ESegmentError::MissingHash, "Segment has no unpruned parent hash for this bitmap");
	}

	// Validate a pruned segment against the MMR root using the output bitmap.
	void ValidateWithBitmap(
		const uint64_t mmrSize, const SegmentBitmap& bitmap, const Hash& mmrRoot) const
	{
		const std::pair<uint64_t, uint64_t> range = m_identifier.SegmentPosRange(mmrSize);
		const std::pair<Hash, uint64_t> segmentRoot = FirstUnprunedParentWithBitmap(mmrSize, bitmap);
		m_proof.Validate(mmrSize, mmrRoot, range.first, range.second, segmentRoot.first, segmentRoot.second);
	}

	// Validate a segment whose tree root is combined with `otherRoot` before the
	// final comparison (grin PIBD: header output_root commits the output PMMR
	// root and the bitmap accumulator root).
	// `bitmap` may be null for a non-pruned tree (bitmap segments).
	void ValidateWithOtherRoot(
		const uint64_t mmrSize,
		const SegmentBitmap* bitmap,
		const Hash& mmrRoot,
		const uint64_t hashLastPos,
		const Hash& otherRoot,
		const bool otherIsLeft
	) const
	{
		const std::pair<uint64_t, uint64_t> range = m_identifier.SegmentPosRange(mmrSize);
		Hash segmentRoot;
		uint64_t segmentUnprunedPos;
		if (bitmap == nullptr) {
			segmentRoot = Root(mmrSize);
			segmentUnprunedPos = range.second + 1;
		} else {
			const std::pair<Hash, uint64_t> unpruned = FirstUnprunedParentWithBitmap(mmrSize, *bitmap);
			segmentRoot = unpruned.first;
			segmentUnprunedPos = unpruned.second;
		}

		m_proof.ValidateWith(
			mmrSize,
			mmrRoot,
			range.first,
			range.second,
			segmentRoot,
			segmentUnprunedPos,
			hashLastPos,
			otherRoot,
			otherIsLeft);
	}

	void Serialize(Serializer& serializer) const final
	{
		m_identifier.Serialize(serializer);
		SegmentDetail::WritePositions(serializer, m_hashPositions);
		for (const Hash& hash : m_hashes) {
			serializer.AppendBigInteger(hash);
		}

		SegmentDetail::WritePositions(serializer, m_leafPositions);
		for (const T& leaf : m_leafData) {
			leaf.Serialize(serializer);
		}

		m_proof.Serialize(serializer);
	}

	static Segment<T> Deserialize(ByteBuffer& byteBuffer)
	{
		const SegmentIdentifier identifier = SegmentIdentifier::Deserialize(byteBuffer);

		const uint64_t numHashes = byteBuffer.ReadU64();
		SegmentDetail::CheckItemCount(numHashes);
		std::vector<uint64_t> hashPositions = SegmentDetail::ReadPositions(byteBuffer, numHashes);

		if (numHashes > byteBuffer.GetRemainingSize() / (uint64_t)HASH_SIZE) {
			throw DESERIALIZATION_EXCEPTION("Segment hash list does not fit in the payload");
		}

		std::vector<Hash> hashes;
		hashes.reserve((size_t)numHashes);
		for (uint64_t i = 0; i < numHashes; ++i) {
			hashes.push_back(byteBuffer.ReadBigInteger<32>());
		}

		const uint64_t numLeaves = byteBuffer.ReadU64();
		SegmentDetail::CheckItemCount(numLeaves);
		// Every serialized leaf consumes at least one byte; refuse unreasonable
		// counts before reserving leaf storage.
		if (numLeaves > byteBuffer.GetRemainingSize()) {
			throw DESERIALIZATION_EXCEPTION("Segment leaf list does not fit in the payload");
		}
		std::vector<uint64_t> leafPositions = SegmentDetail::ReadPositions(byteBuffer, numLeaves);

		std::vector<T> leafData;
		leafData.reserve((size_t)numLeaves);
		for (uint64_t i = 0; i < numLeaves; ++i) {
			leafData.push_back(T::Deserialize(byteBuffer));
		}

		SegmentProof proof = SegmentProof::Deserialize(byteBuffer);

		return Segment<T>(
			identifier,
			std::move(hashPositions),
			std::move(hashes),
			std::move(leafPositions),
			std::move(leafData),
			std::move(proof)
		);
	}

private:
	SegmentIdentifier m_identifier;
	std::vector<uint64_t> m_hashPositions;
	std::vector<Hash> m_hashes;
	std::vector<uint64_t> m_leafPositions;
	std::vector<T> m_leafData;
	SegmentProof m_proof;
};
