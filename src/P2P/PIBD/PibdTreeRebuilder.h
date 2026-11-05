#pragma once

#include "../../PMMR/Common/MMRHashUtil.h"
#include "../../PMMR/Common/MMRUtil.h"
#include "../../PMMR/Models/SegmentException.h"

#include <Core/Serialization/Serializer.h>
#include <Crypto/Models/Hash.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace PibdTreeRebuilderDetail
{
	// Hash of a child node: computed already, or a pruned-subtree root whose hash
	// the segments carried. Interior nodes of a pruned subtree are never asked for.
	inline Hash ResolveChild(
		const uint64_t childPos,
		const std::vector<std::optional<Hash>>& hashes,
		const std::map<uint64_t, Hash>& storedHashes,
		const std::vector<uint8_t>& prunedRoot)
	{
		if (hashes[(size_t)childPos].has_value()) {
			return hashes[(size_t)childPos].value();
		}

		if (prunedRoot[(size_t)childPos] == 0) {
			throw SegmentException(
				ESegmentError::MissingHash,
				"Missing hash below position " + std::to_string(childPos));
		}

		const auto stored = storedHashes.find(childPos);
		if (stored == storedHashes.end()) {
			throw SegmentException(
				ESegmentError::MissingHash,
				"Missing pruned-subtree hash at position " + std::to_string(childPos));
		}

		return stored->second;
	}
}

// Rebuild the root of a possibly pruned PMMR from the leaves we hold plus the
// subtree hashes carried by segments.
//
// This is the in-memory equivalent of what the desegmenter writes into the PMMR
// hash file. Pruning is identified the same way the writer does it: a node is a
// pruned-subtree root when its subtree holds no leaf data but its parent does.
// Nodes *inside* such a subtree have no hash and need none - only the root hash
// is carried by the segments.
template<class T>
inline Hash ComputePrunedTreeRoot(
	const uint64_t mmrSize,
	const std::vector<std::pair<uint64_t, T>>& leavesByLeafIndex,
	const std::map<uint64_t, Hash>& storedHashes)
{
	if (mmrSize == 0) {
		throw SegmentException(ESegmentError::NonExistent, "Empty PMMR has no root");
	}

	const uint64_t numLeaves = MMRUtil::GetNumLeaves(mmrSize);
	std::map<uint64_t, const T*> leaves;
	for (const std::pair<uint64_t, T>& leaf : leavesByLeafIndex) {
		if (leaf.first >= numLeaves) {
			throw SegmentException(
				ESegmentError::Mismatch,
				"Leaf index " + std::to_string(leaf.first) + " is out of range for this MMR");
		}

		if (!leaves.emplace(leaf.first, &leaf.second).second) {
			throw SegmentException(
				ESegmentError::Mismatch,
				"Leaf index " + std::to_string(leaf.first) + " appears twice");
		}
	}

	// 1. Which subtrees hold leaf data, and who is whose parent.
	std::vector<uint8_t> hasData((size_t)mmrSize, 0);
	std::vector<int64_t> parent((size_t)mmrSize, -1);
	uint64_t leafIndex = 0;
	for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
		if (MMRUtil::IsLeaf(pos0)) {
			hasData[(size_t)pos0] = leaves.find(leafIndex) != leaves.end() ? 1 : 0;
			++leafIndex;
			continue;
		}

		const uint64_t height = MMRUtil::BintreePostorderHeight(pos0);
		const uint64_t leftChild = pos0 - (1ULL << height);
		const uint64_t rightChild = pos0 - 1;
		parent[(size_t)leftChild] = (int64_t)pos0;
		parent[(size_t)rightChild] = (int64_t)pos0;
		hasData[(size_t)pos0] =
			(hasData[(size_t)leftChild] != 0 || hasData[(size_t)rightChild] != 0) ? 1 : 0;
	}

	if (leaves.empty()) {
		throw SegmentException(
			ESegmentError::MissingHash, "Tree has no leaf data and cannot produce a root");
	}

	// 2. Roots of maximal fully-pruned subtrees. Interior nodes are skipped: their
	// hashes are not part of the state and must not be required.
	std::vector<uint8_t> prunedRoot((size_t)mmrSize, 0);
	for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
		const int64_t parentPos = parent[(size_t)pos0];
		if (hasData[(size_t)pos0] == 0 && (parentPos < 0 || hasData[(size_t)parentPos] != 0)) {
			// A fully pruned peak has no parent but is still a carried subtree root.
			prunedRoot[(size_t)pos0] = 1;
		}
	}

	// 3. Hashes: leaves from data, internal nodes from children (with a stored hash
	// for a pruned child), pruned roots from the carried subtree hash.
	std::vector<std::optional<Hash>> hashes((size_t)mmrSize);
	leafIndex = 0;
	for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
		if (MMRUtil::IsLeaf(pos0)) {
			const auto leaf = leaves.find(leafIndex);
			if (leaf != leaves.end()) {
				Serializer serializer;
				leaf->second->Serialize(serializer);
				hashes[(size_t)pos0] = MMRHashUtil::HashLeafWithIndex(serializer.GetBytes(), pos0);
			} else {
				// Hash-only boundary leaf: no data, but its hash may be carried
				// by the segment. It must never be a pruned internal position.
				const int64_t parentPos = parent[(size_t)pos0];
				const auto stored = storedHashes.find(pos0);
				if (stored != storedHashes.end() && parentPos >= 0 && hasData[(size_t)parentPos] != 0) {
					hashes[(size_t)pos0] = stored->second;
				}
			}

			++leafIndex;
			continue;
		}

		if (prunedRoot[(size_t)pos0] != 0) {
			const auto stored = storedHashes.find(pos0);
			if (stored == storedHashes.end()) {
				throw SegmentException(
					ESegmentError::MissingHash,
					"Missing pruned-subtree hash at position " + std::to_string(pos0));
			}

			hashes[(size_t)pos0] = stored->second;
			continue;
		}

		if (hasData[(size_t)pos0] == 0) {
			continue;   // inside a pruned subtree
		}

		const uint64_t height = MMRUtil::BintreePostorderHeight(pos0);
		const uint64_t leftChild = pos0 - (1ULL << height);
		const uint64_t rightChild = pos0 - 1;
		hashes[(size_t)pos0] = MMRHashUtil::HashParentWithIndex(
			PibdTreeRebuilderDetail::ResolveChild(leftChild, hashes, storedHashes, prunedRoot),
			PibdTreeRebuilderDetail::ResolveChild(rightChild, hashes, storedHashes, prunedRoot),
			pos0);
	}

	// 4. Bag the peaks, exactly like MMRHashUtil::Root.
	Hash root = ZERO_HASH;
	const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(mmrSize);
	for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
		const uint64_t peakPos0 = *iter;
		std::optional<Hash> peakHash = hashes[(size_t)peakPos0];
		if (!peakHash.has_value()) {
			const auto stored = storedHashes.find(peakPos0);
			if (prunedRoot[(size_t)peakPos0] == 0 || stored == storedHashes.end()) {
				throw SegmentException(
					ESegmentError::MissingHash,
					"Missing hash for peak at position " + std::to_string(peakPos0));
			}

			peakHash = stored->second;
		}

		root = (root == ZERO_HASH)
			? peakHash.value()
			: MMRHashUtil::HashParentWithIndex(peakHash.value(), root, mmrSize);
	}

	if (root == ZERO_HASH) {
		throw SegmentException(ESegmentError::NonExistent, "Tree produced no root");
	}

	return root;
}
