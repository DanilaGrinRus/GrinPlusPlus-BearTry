#pragma once

#include "PibdDownload.h"

#include "../../PMMR/Common/MMRUtil.h"
#include "../../PMMR/Models/InMemoryPmmr.h"
#include "PibdTreeRebuilder.h"
#include "../../PMMR/Models/SegmentException.h"

#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Hash.h>
#include <Crypto/Models/RangeProof.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

// Result of a completed PIBD download, in the form the chain needs to apply it:
// every unpruned leaf tagged with its 0-based leaf index, plus the output bitmap
// that says which outputs are still unspent.
//
// Building the snapshot re-verifies the kernel tree end to end (kernels are never
// pruned, so the whole tree can be rebuilt from the received leaves) and checks
// that outputs and rangeproofs were pruned identically.
struct PibdSnapshot
{
	uint64_t Height = 0;
	Hash BlockHash;
	uint64_t OutputMmrSize = 0;
	uint64_t KernelMmrSize = 0;

	// Horizon roots, kept so a writer can verify what it produced.
	// Header output_root and the output PMMR root it commits to.
	Hash OutputRoot;
	Hash OutputMmrRoot;
	Hash RangeProofRoot;
	Hash KernelRoot;

	// (leaf index, leaf) sorted by leaf index; only the unpruned leaves.
	std::vector<std::pair<uint64_t, OutputIdentifier>> Outputs;
	std::vector<std::pair<uint64_t, RangeProof>> RangeProofs;
	std::vector<std::pair<uint64_t, TransactionKernel>> Kernels;

	// Pruned-subtree hashes carried by the segments (MMR position -> hash).
	std::map<uint64_t, Hash> OutputHashes;
	std::map<uint64_t, Hash> RangeProofHashes;
	std::map<uint64_t, Hash> KernelHashes;

	VectorSegmentBitmap Bitmap;
};

namespace PibdSnapshotDetail
{
	// Keep the bitmap-required leaf pairs. Optional data may differ between
	// output and rangeproof trees and cannot be stored as a single leaf with
	// the current prune-list data layout.
	template<class T>
	void NormalizeLeaves(
		const uint64_t mmrSize,
		const SegmentBitmap& bitmap,
		std::vector<std::pair<uint64_t, T>>& leaves,
		std::map<uint64_t, Hash>& storedHashes)
	{
		const uint64_t numLeaves = MMRUtil::GetNumLeaves(mmrSize);
		std::vector<std::optional<Hash>> available((size_t)mmrSize);
		std::vector<uint8_t> hasRequired((size_t)mmrSize, 0);
		std::vector<int64_t> parent((size_t)mmrSize, -1);
		size_t nextLeaf = 0;
		uint64_t leafIndex = 0;
		for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
			if (MMRUtil::IsLeaf(pos0)) {
				const bool required = IsSegmentLeafRequired(pos0, mmrSize, bitmap);
				const bool present = nextLeaf < leaves.size() && leaves[nextLeaf].first == leafIndex;
				if (required && !present) {
					throw SegmentException(ESegmentError::MissingLeaf,
						"Required leaf is missing at position " + std::to_string(pos0));
				}
				if (present) {
					Serializer serializer;
					leaves[nextLeaf].second.Serialize(serializer);
					available[(size_t)pos0] = MMRHashUtil::HashLeafWithIndex(serializer.GetBytes(), pos0);
					const auto found = storedHashes.find(pos0);
					if (found != storedHashes.end() && found->second != *available[(size_t)pos0]) {
						throw SegmentException(ESegmentError::Mismatch,
							"Conflicting leaf data and hash at position " + std::to_string(pos0));
					}
					++nextLeaf;
				} else {
					const auto found = storedHashes.find(pos0);
					if (found != storedHashes.end()) {
						available[(size_t)pos0] = found->second;
					}
				}
				hasRequired[(size_t)pos0] = required ? 1 : 0;
				++leafIndex;
				continue;
			}

			const uint64_t height = MMRUtil::BintreePostorderHeight(pos0);
			const uint64_t left = pos0 - (1ULL << height);
			const uint64_t right = pos0 - 1;
			parent[(size_t)left] = (int64_t)pos0;
			parent[(size_t)right] = (int64_t)pos0;
			hasRequired[(size_t)pos0] = hasRequired[(size_t)left] || hasRequired[(size_t)right];
			if (available[(size_t)left].has_value() && available[(size_t)right].has_value()) {
				available[(size_t)pos0] = MMRHashUtil::HashParentWithIndex(
					*available[(size_t)left], *available[(size_t)right], pos0);
				const auto found = storedHashes.find(pos0);
				if (found != storedHashes.end() && found->second != *available[(size_t)pos0]) {
					throw SegmentException(ESegmentError::Mismatch,
						"Conflicting subtree hash at position " + std::to_string(pos0));
				}
			} else {
				const auto found = storedHashes.find(pos0);
				if (found != storedHashes.end()) {
					available[(size_t)pos0] = found->second;
				}
			}
		}
		if (leafIndex != numLeaves || nextLeaf != leaves.size()) {
			throw SegmentException(ESegmentError::Mismatch, "Invalid leaf indices");
		}

		for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
			const int64_t parentPos = parent[(size_t)pos0];
			if (!hasRequired[(size_t)pos0] &&
				(parentPos < 0 || hasRequired[(size_t)parentPos])) {
				if (!available[(size_t)pos0].has_value()) {
					throw SegmentException(ESegmentError::MissingHash,
						"Missing normalized subtree hash at position " + std::to_string(pos0));
				}
				storedHashes[pos0] = *available[(size_t)pos0];
			}
		}
		leaves.erase(std::remove_if(leaves.begin(), leaves.end(), [&](const std::pair<uint64_t, T>& leaf) {
			const uint64_t pos0 = MMRUtil::InsertionToPmmrIndex(leaf.first);
			return !IsSegmentLeafRequired(pos0, mmrSize, bitmap);
		}), leaves.end());
	}

	template<class T>
	std::vector<std::pair<uint64_t, T>> ToLeafIndices(std::vector<std::pair<uint64_t, T>> leaves)
	{
		for (std::pair<uint64_t, T>& leaf : leaves) {
			const std::optional<uint64_t> leafIndex = MMRUtil::PmmrLeafToInsertionIndex(leaf.first);
			if (!leafIndex.has_value()) {
				throw SegmentException(
					ESegmentError::Mismatch,
					"Leaf position " + std::to_string(leaf.first) + " is not a leaf");
			}

			leaf.first = leafIndex.value();
		}

		std::sort(
			leaves.begin(),
			leaves.end(),
			[](const std::pair<uint64_t, T>& lhs, const std::pair<uint64_t, T>& rhs) {
				return lhs.first < rhs.first;
			});

		for (size_t i = 1; i < leaves.size(); ++i) {
			if (leaves[i].first == leaves[i - 1].first) {
				throw SegmentException(
					ESegmentError::Mismatch,
					"Duplicate leaf index " + std::to_string(leaves[i].first));
			}
		}

		return leaves;
	}
}

// Convert a completed download into a snapshot, re-verifying the parts that can
// be verified from the leaf data alone.
inline PibdSnapshot BuildSnapshot(const PibdDownload& download)
{
	if (!download.IsComplete()) {
		throw SegmentException(ESegmentError::NonExistent, "PIBD download is not complete");
	}

	PibdSnapshot snapshot;
	snapshot.Height = download.GetHorizon().Height;
	snapshot.BlockHash = download.GetHorizon().BlockHash;
	snapshot.OutputMmrSize = download.GetHorizon().OutputMmrSize;
	snapshot.KernelMmrSize = download.GetHorizon().KernelMmrSize;
	snapshot.OutputRoot = download.GetHorizon().OutputRoot;
	snapshot.OutputMmrRoot = download.GetOutputMmrRoot();
	snapshot.RangeProofRoot = download.GetHorizon().RangeProofRoot;
	snapshot.KernelRoot = download.GetHorizon().KernelRoot;
	snapshot.Bitmap = download.GetOutputBitmap();
	snapshot.Outputs = PibdSnapshotDetail::ToLeafIndices(download.GetOutputs().GetLeaves());
	snapshot.RangeProofs = PibdSnapshotDetail::ToLeafIndices(download.GetRangeProofs().GetLeaves());
	snapshot.Kernels = PibdSnapshotDetail::ToLeafIndices(download.GetKernels().GetLeaves());
	snapshot.OutputHashes = download.GetOutputs().GetStoredHashes();
	snapshot.RangeProofHashes = download.GetRangeProofs().GetStoredHashes();
	snapshot.KernelHashes = download.GetKernels().GetStoredHashes();
	PibdSnapshotDetail::NormalizeLeaves(snapshot.OutputMmrSize, snapshot.Bitmap,
		snapshot.Outputs, snapshot.OutputHashes);
	PibdSnapshotDetail::NormalizeLeaves(snapshot.OutputMmrSize, snapshot.Bitmap,
		snapshot.RangeProofs, snapshot.RangeProofHashes);

	// Kernels are never pruned: the whole tree must rebuild to the horizon root.
	const uint64_t kernelLeaves = MMRUtil::GetNumLeaves(snapshot.KernelMmrSize);
	if ((uint64_t)snapshot.Kernels.size() != kernelLeaves) {
		throw SegmentException(
			ESegmentError::Mismatch,
			"Received " + std::to_string(snapshot.Kernels.size()) + " kernels, expected " +
				std::to_string(kernelLeaves));
	}

	// Rebuild every tree from the leaves and the pruned-subtree hashes and check
	// it against the horizon. This is the same computation the desegmenter will
	// use to write the PMMR hash files.
	if (ComputePrunedTreeRoot(snapshot.KernelMmrSize, snapshot.Kernels, snapshot.KernelHashes)
		!= download.GetHorizon().KernelRoot) {
		throw SegmentException(
			ESegmentError::Mismatch, "Rebuilt kernel tree root does not match the horizon");
	}

	if (ComputePrunedTreeRoot(snapshot.OutputMmrSize, snapshot.Outputs, snapshot.OutputHashes)
		!= snapshot.OutputMmrRoot) {
		throw SegmentException(
			ESegmentError::Mismatch, "Rebuilt output tree root does not match the horizon");
	}

	if (ComputePrunedTreeRoot(snapshot.OutputMmrSize, snapshot.RangeProofs, snapshot.RangeProofHashes)
		!= download.GetHorizon().RangeProofRoot) {
		throw SegmentException(
			ESegmentError::Mismatch, "Rebuilt rangeproof tree root does not match the horizon");
	}

	// Outputs and rangeproofs are pruned with the same bitmap, so their leaf
	// index sets must be identical.
	if (snapshot.Outputs.size() != snapshot.RangeProofs.size()) {
		throw SegmentException(
			ESegmentError::Mismatch, "Output and rangeproof leaf counts differ");
	}

	for (size_t i = 0; i < snapshot.Outputs.size(); ++i) {
		if (snapshot.Outputs[i].first != snapshot.RangeProofs[i].first) {
			throw SegmentException(
				ESegmentError::Mismatch,
				"Output and rangeproof leaf indices differ at " + std::to_string(snapshot.Outputs[i].first));
		}
	}

	return snapshot;
}
