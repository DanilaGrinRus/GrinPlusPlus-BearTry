#pragma once

#include "InMemoryPmmr.h"
#include "Segment.h"
#include "SegmentBitmap.h"
#include "SegmentException.h"
#include "SegmentIdentifier.h"
#include "SegmentProof.h"

#include "../Common/MMRUtil.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

// Generate PMMR segments from an MMR.
// Ported from grin v5.5.2 Segment::from_pmmr / SegmentProof::generate:
//   * without a bitmap the segment is unpruned (all leaves in range, no hashes);
//   * with a bitmap the segment is pruned like a real PIBD peer would serve it:
//     only required leaves are included, internal hashes are included when the
//     source has them, and a fully pruned segment carries the first unpruned
//     parent hash plus the proof start position.
//
// Generation is expressed over two callbacks so both the in-memory model and the
// node's PruneableMMR files can serve segments without duplicating the logic.
template<class T>
class SegmentGenerator
{
public:
	static Segment<T> Generate(const InMemoryPmmr<T>& mmr, const SegmentIdentifier& identifier)
	{
		return Generate(mmr, identifier, nullptr);
	}

	static Segment<T> Generate(
		const InMemoryPmmr<T>& mmr,
		const SegmentIdentifier& identifier,
		const SegmentBitmap* bitmap)
	{
		return GenerateFromSource(
			identifier,
			mmr.GetSize(),
			bitmap,
			[&mmr](const uint64_t pos0) { return std::optional<T>(mmr.GetLeafData(pos0)); },
			[&mmr](const uint64_t pos0) { return std::optional<Hash>(mmr.GetHash(pos0)); });
	}

	static Segment<T> GenerateFromSource(
		const SegmentIdentifier& identifier,
		const uint64_t mmrSize,
		const SegmentBitmap* bitmap,
		const std::function<std::optional<T>(uint64_t)>& leafAt,
		const std::function<std::optional<Hash>(uint64_t)>& hashAt)
	{
		if (identifier.SegmentUnprunedSize(mmrSize) == 0) {
			throw SegmentException(
				ESegmentError::NonExistent,
				"Segment " + std::to_string(identifier.GetIdx()) + " does not exist at this MMR size");
		}

		const std::pair<uint64_t, uint64_t> range = identifier.SegmentPosRange(mmrSize);

		std::vector<uint64_t> hashPositions;
		std::vector<Hash> hashes;
		std::vector<uint64_t> leafPositions;
		std::vector<T> leaves;
		for (uint64_t pos0 = range.first; ; ++pos0) {
			if (MMRUtil::IsLeaf(pos0)) {
				const bool required = bitmap == nullptr || IsSegmentLeafRequired(pos0, mmrSize, *bitmap);
				std::optional<T> leaf = leafAt(pos0);
				if (leaf.has_value()) {
					// Match grin Segment::from_pmmr: when leaf data is
					// available it is sent even if the bitmap marks the output
					// as spent. The receiver applies the leaf and then removes
					// it from the leaf set. Omitting it here leaves a hole in
					// the sequential PMMR and later causes
					// "missing left sibling in tree".
					leafPositions.push_back(pos0);
					leaves.push_back(std::move(leaf.value()));
				} else if (required) {
					throw SegmentException(
						ESegmentError::MissingLeaf,
						"Source is missing required leaf data at position " + std::to_string(pos0));
				} else if (std::optional<Hash> hash = hashAt(pos0)) {
					hashPositions.push_back(pos0);
					hashes.push_back(std::move(*hash));
				}
			} else if (bitmap != nullptr) {
				std::optional<Hash> hash = hashAt(pos0);
				if (hash.has_value()) {
					hashPositions.push_back(pos0);
					hashes.push_back(std::move(hash.value()));
				}
			}

			if (pos0 == range.second) {
				break;
			}
		}

		std::optional<uint64_t> startPos;
		if (bitmap != nullptr && leaves.empty() && hashes.empty()) {
			// Fully pruned segment: walk the whole family branch to the first
			// stored ancestor. Rust does not assume branch[0] is present.
			const std::vector<std::pair<uint64_t, uint64_t>> branch =
				MMRUtil::FamilyBranch(range.second, mmrSize);
			for (const auto& ancestor : branch) {
				std::optional<Hash> hash = hashAt(ancestor.first);
				if (hash.has_value()) {
					hashPositions.push_back(ancestor.first);
					hashes.push_back(std::move(hash.value()));
					startPos = ancestor.first + 1;
					break;
				}
			}

			if (!startPos.has_value()) {
				throw SegmentException(
					ESegmentError::MissingHash, "No stored ancestor for the pruned segment");
			}
		}

		SegmentProof proof = GenerateProof(mmrSize, range.first, range.second, startPos, hashAt);
		return Segment<T>(
			identifier,
			std::move(hashPositions),
			std::move(hashes),
			std::move(leafPositions),
			std::move(leaves),
			std::move(proof));
	}

private:
	static SegmentProof GenerateProof(
		const uint64_t mmrSize,
		const uint64_t firstPos0,
		const uint64_t lastPos0,
		const std::optional<uint64_t> startPos,
		const std::function<std::optional<Hash>(uint64_t)>& hashAt)
	{
		std::vector<Hash> hashes;

		// 1. Siblings along the path from the subtree root to the peak.
		const std::vector<std::pair<uint64_t, uint64_t>> familyBranch =
			MMRUtil::FamilyBranch(lastPos0, mmrSize);
		for (const std::pair<uint64_t, uint64_t>& branch : familyBranch) {
			if (startPos.has_value() && branch.first < startPos.value()) {
				continue;
			}

			std::optional<Hash> hash = hashAt(branch.second);
			if (!hash.has_value()) {
				throw SegmentException(
					ESegmentError::MissingHash,
					"Source is missing the sibling hash at position " + std::to_string(branch.second));
			}

			hashes.push_back(std::move(hash.value()));
		}

		// 2. Bagged peaks strictly to the right of the peak.
		const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(mmrSize);
		const uint64_t peakPos0 = familyBranch.empty() ? lastPos0 : familyBranch.back().first;
		std::optional<Hash> rhs;
		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			if (*iter <= peakPos0) {
				continue;
			}

			std::optional<Hash> hash = hashAt(*iter);
			if (!hash.has_value()) {
				throw SegmentException(
					ESegmentError::MissingHash,
					"Source is missing the peak hash at position " + std::to_string(*iter));
			}

			rhs = rhs.has_value()
				? std::optional<Hash>(MMRHashUtil::HashParentWithIndex(hash.value(), rhs.value(), mmrSize))
				: hash;
		}

		if (rhs.has_value()) {
			hashes.push_back(rhs.value());
		}

		// 3. Peaks strictly to the left, right to left.
		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			if (*iter < firstPos0) {
				std::optional<Hash> hash = hashAt(*iter);
				if (!hash.has_value()) {
					throw SegmentException(
						ESegmentError::MissingHash,
						"Source is missing the peak hash at position " + std::to_string(*iter));
				}

				hashes.push_back(std::move(hash.value()));
			}
		}

		return SegmentProof(std::move(hashes));
	}
};
