#pragma once

#include "PibdActivation.h"
#include "PibdSnapshot.h"

#include <Core/File/BitmapFile.h>
#include "../../PMMR/Common/HashFile.h"
#include "../../PMMR/Common/MMRHashUtil.h"
#include "../../PMMR/Common/MMRUtil.h"
#include "../../PMMR/Common/PruneList.h"
#include "../../PMMR/Models/SegmentException.h"

#include <Common/Util/FileUtil.h>
#include <Core/File/DataFile.h>
#include <Crypto/Models/Hash.h>
#include <Roaring.h>

#include <filesystem.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct PibdTreeWriteInfo
{
	uint64_t Size = 0;
	uint64_t StoredNodes = 0;
	uint64_t StoredLeaves = 0;
	uint64_t PrunedRoots = 0;
	Hash Root;
};

// Writes a PIBD snapshot into the on-disk PMMR format Grin++ uses
// (pmmr_hash.bin, pmmr_data.bin, pmmr_leafset.bin, pmmr_prun.bin), using the
// same HashFile/DataFile/BitmapFile/PruneList primitives as the node.
//
// Every tree is written into <txHashSetDir>/<tree>/ so the caller can point at a
// staging directory; activation is a separate, atomic step. Each tree is verified
// by re-loading the written files and recomputing the root, which must equal the
// horizon root.
class PibdStateWriter
{
public:
	static constexpr size_t OUTPUT_LEAF_SIZE = 34;
	static constexpr size_t RANGEPROOF_LEAF_SIZE = 683;
	static constexpr size_t KERNEL_LEAF_SIZE = 114;

	// Write all three trees, verified against the snapshot's horizon roots.
	static void Write(const PibdSnapshot& snapshot, const fs::path& txHashSetDir)
	{
		const PibdTreeWriteInfo outputs = WriteTree<OUTPUT_LEAF_SIZE>(
			txHashSetDir / "output", snapshot.OutputMmrSize, snapshot.Outputs,
			snapshot.OutputHashes, &snapshot.Bitmap, "output");
		const PibdTreeWriteInfo rangeProofs = WriteTree<RANGEPROOF_LEAF_SIZE>(
			txHashSetDir / "rangeproof", snapshot.OutputMmrSize, snapshot.RangeProofs,
			snapshot.RangeProofHashes, &snapshot.Bitmap, "rangeproof");
		const PibdTreeWriteInfo kernels = WriteTree<KERNEL_LEAF_SIZE>(
			txHashSetDir / "kernel", snapshot.KernelMmrSize, snapshot.Kernels,
			snapshot.KernelHashes, nullptr, "kernel");

		if (outputs.Root != snapshot.OutputMmrRoot) {
			throw SegmentException(
				ESegmentError::Mismatch, "Written output root does not match the carried output root");
		}

		if (rangeProofs.Root != snapshot.RangeProofRoot) {
			throw SegmentException(ESegmentError::Mismatch, "Written rangeproof root does not match the horizon");
		}

		if (kernels.Root != snapshot.KernelRoot) {
			throw SegmentException(ESegmentError::Mismatch, "Written kernel root does not match the horizon");
		}

		// Only now is the staging directory complete enough to be activated.
		PibdActivation::WriteCompleteMarker(txHashSetDir, snapshot.BlockHash, snapshot.Height);
	}

private:
	// DATA_SIZE is the fixed on-disk element size of the tree's data file.
	template<size_t DATA_SIZE, class T>
	static PibdTreeWriteInfo WriteTree(
		const fs::path& treeDir,
		const uint64_t mmrSize,
		const std::vector<std::pair<uint64_t, T>>& leavesByLeafIndex,
		const std::map<uint64_t, Hash>& storedHashes,
		const SegmentBitmap* unspentLeaves,
		const std::string& treeName)
	{
		if (mmrSize == 0) {
			throw SegmentException(
				ESegmentError::NonExistent, treeName + " tree has no size");
		}

		const uint64_t numLeaves = MMRUtil::GetNumLeaves(mmrSize);

		std::map<uint64_t, const T*> leafData;
		for (const std::pair<uint64_t, T>& leaf : leavesByLeafIndex) {
			if (leaf.first >= numLeaves) {
				throw SegmentException(
					ESegmentError::Mismatch,
					treeName + " leaf index " + std::to_string(leaf.first) + " is out of range");
			}

			if (!leafData.emplace(leaf.first, &leaf.second).second) {
				throw SegmentException(
					ESegmentError::Mismatch,
					treeName + " tree contains leaf index " + std::to_string(leaf.first) + " twice");
			}
		}

		// 1. Leaf hashes and which subtrees hold any data at all.
		std::vector<uint8_t> hasData((size_t)mmrSize, 0);
		std::vector<int64_t> parent((size_t)mmrSize, -1);
		std::vector<std::optional<Hash>> hashes((size_t)mmrSize);
		uint64_t leafIndex = 0;
		for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
			if (MMRUtil::IsLeaf(pos0)) {
				const auto leaf = leafData.find(leafIndex);
				if (leaf != leafData.end()) {
					Serializer serializer;
					leaf->second->Serialize(serializer);
					if (serializer.GetBytes().size() != DATA_SIZE) {
						throw SegmentException(
							ESegmentError::Mismatch,
							treeName + " leaf " + std::to_string(leafIndex) + " serializes to " +
								std::to_string(serializer.GetBytes().size()) + " bytes, expected " +
								std::to_string(DATA_SIZE));
					}

					hashes[(size_t)pos0] = MMRHashUtil::HashLeafWithIndex(serializer.GetBytes(), pos0);
					hasData[(size_t)pos0] = 1;
				}

				++leafIndex;
				continue;
			}

			const uint64_t height = MMRUtil::BintreePostorderHeight(pos0);
			const uint64_t leftChild = pos0 - (1ULL << height);
			const uint64_t rightChild = pos0 - 1;
			parent[(size_t)leftChild] = (int64_t)pos0;
			parent[(size_t)rightChild] = (int64_t)pos0;
			hasData[(size_t)pos0] = (hasData[(size_t)leftChild] || hasData[(size_t)rightChild]) ? 1 : 0;
		}

		// A spent leaf that is the sibling of an unspent leaf is a boundary
		// hash-only leaf: the segment carries its hash but no data. Persist it at
		// its unshifted position so serving can prove the parent. Leaves inside a
		// fully pruned subtree remain represented only by that subtree root.
		for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
			if (!MMRUtil::IsLeaf(pos0) || hashes[(size_t)pos0].has_value()) {
				continue;
			}

			const int64_t parentPos = parent[(size_t)pos0];
			if (parentPos < 0 || hasData[(size_t)parentPos] == 0) {
				continue;
			}

			const auto stored = storedHashes.find(pos0);
			if (stored != storedHashes.end()) {
				hashes[(size_t)pos0] = stored->second;
			}
		}
		if (leafData.empty()) {
			throw SegmentException(
				ESegmentError::MissingHash,
				treeName + " tree has no leaf data and cannot be written");
		}

		// 2. Roots of maximal fully-pruned subtrees: no data here, data above.
		std::vector<uint8_t> prunedRoot((size_t)mmrSize, 0);
		for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
			const int64_t parentPos = parent[(size_t)pos0];
			if (hasData[(size_t)pos0] == 0 && (parentPos < 0 || hasData[(size_t)parentPos] == 1)) {
				if (MMRUtil::IsLeaf(pos0)) {
					throw SegmentException(ESegmentError::MissingLeaf,
						treeName + " cannot store a hash-only leaf at position " + std::to_string(pos0));
				}
				// A fully pruned peak is a valid carried subtree root.
				prunedRoot[(size_t)pos0] = 1;
			}
		}

		// 3. Internal hashes: computed from the children, or taken from a segment
		// when the child is a pruned subtree root.
		for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
			if (MMRUtil::IsLeaf(pos0)) {
				continue;
			}

			const uint64_t height = MMRUtil::BintreePostorderHeight(pos0);
			const uint64_t leftChild = pos0 - (1ULL << height);
			const uint64_t rightChild = pos0 - 1;

			if (prunedRoot[(size_t)pos0] != 0) {
				const auto stored = storedHashes.find(pos0);
				if (stored == storedHashes.end()) {
					throw SegmentException(
						ESegmentError::MissingHash,
						treeName + " tree is missing the pruned-subtree hash at position " +
							std::to_string(pos0));
				}

				hashes[(size_t)pos0] = stored->second;
				continue;
			}

			if (hasData[(size_t)pos0] == 0) {
				continue;   // inside a pruned subtree, nothing is stored
			}

			const Hash leftHash = ResolveChild(leftChild, hashes, storedHashes, treeName);
			const Hash rightHash = ResolveChild(rightChild, hashes, storedHashes, treeName);
			hashes[(size_t)pos0] = MMRHashUtil::HashParentWithIndex(leftHash, rightHash, pos0);
		}

		// 4. Write the files in the shifted layout used by PruneableMMR.
		fs::create_directories(treeDir);

		const fs::path hashPath = treeDir / "pmmr_hash.bin";
		const fs::path dataPath = treeDir / "pmmr_data.bin";
		const fs::path leafSetPath = treeDir / "pmmr_leafset.bin";
		const fs::path prunePath = treeDir / "pmmr_prun.bin";

		// The leaf set marks unspent leaves (the horizon bitmap), not every leaf we
		// received: a spent leaf whose sibling is unspent is sent for verification
		// but stays spent in the state.
		if (unspentLeaves != nullptr) {
			for (uint64_t unspentLeafIndex = 0; unspentLeafIndex < numLeaves; ++unspentLeafIndex) {
				if (!unspentLeaves->Contains(unspentLeafIndex)) {
					continue;
				}

				if (leafData.find(unspentLeafIndex) == leafData.end()) {
					throw SegmentException(
						ESegmentError::MissingLeaf,
						treeName + " tree is missing unspent leaf " + std::to_string(unspentLeafIndex));
				}
			}
		}

		std::shared_ptr<HashFile> pHashFile = HashFile::Load(hashPath);
		Roaring leafSetRoaring;
		uint64_t storedNodes = 0;
		uint64_t writtenLeafIndex = 0;
		for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
			if (hashes[(size_t)pos0].has_value()) {
				pHashFile->AddData(hashes[(size_t)pos0].value());
				++storedNodes;
			}

			if (MMRUtil::IsLeaf(pos0)) {
				if (hasData[(size_t)pos0] != 0 &&
					(unspentLeaves == nullptr || unspentLeaves->Contains(writtenLeafIndex))) {
					leafSetRoaring.add((uint32_t)(pos0 + 1));
				}

				++writtenLeafIndex;
			}
		}

		pHashFile->Commit();

		std::shared_ptr<DataFile<DATA_SIZE>> pDataFile = DataFile<DATA_SIZE>::Load(dataPath);
		uint64_t storedLeaves = 0;
		for (uint64_t index = 0; index < numLeaves; ++index) {
			const auto leaf = leafData.find(index);
			if (leaf == leafData.end()) {
				continue;
			}

			Serializer serializer;
			leaf->second->Serialize(serializer);
			pDataFile->AddData(serializer.GetBytes());
			++storedLeaves;
		}

		pDataFile->Commit();

		BitmapFile::Create(leafSetPath, leafSetRoaring);
		WritePruneList(prunePath, prunedRoot, mmrSize);

		// 5. Verify by re-loading exactly what a node would read.
		std::shared_ptr<HashFile> pVerifyHash = HashFile::Load(hashPath);
		std::shared_ptr<const PruneList> pVerifyPrune = PruneList::Load(prunePath);
		const Hash root = MMRHashUtil::Root(pVerifyHash, mmrSize, pVerifyPrune);

		PibdTreeWriteInfo info;
		info.Size = mmrSize;
		info.StoredNodes = storedNodes;
		info.StoredLeaves = storedLeaves;
		info.PrunedRoots = (uint64_t)std::count(prunedRoot.cbegin(), prunedRoot.cend(), (uint8_t)1);
		info.Root = root;

		if (pVerifyHash->GetSize() != storedNodes) {
			throw SegmentException(
				ESegmentError::Mismatch,
				treeName + " hash file has " + std::to_string(pVerifyHash->GetSize()) +
					" nodes, wrote " + std::to_string(storedNodes));
		}

		if (pDataFile->GetSize() != storedLeaves) {
			throw SegmentException(
				ESegmentError::Mismatch,
				treeName + " data file has " + std::to_string(pDataFile->GetSize()) +
					" leaves, wrote " + std::to_string(storedLeaves));
		}

		return info;
	}

	static Hash ResolveChild(
		const uint64_t childPos,
		const std::vector<std::optional<Hash>>& hashes,
		const std::map<uint64_t, Hash>& storedHashes,
		const std::string& treeName)
	{
		if (hashes[(size_t)childPos].has_value()) {
			return hashes[(size_t)childPos].value();
		}

		const auto stored = storedHashes.find(childPos);
		if (stored == storedHashes.end()) {
			throw SegmentException(
				ESegmentError::MissingHash,
				treeName + " tree is missing the pruned-subtree hash at position " +
					std::to_string(childPos));
		}

		return stored->second;
	}

	static void WritePruneList(
		const fs::path& prunePath,
		const std::vector<uint8_t>& prunedRoot,
		const uint64_t mmrSize)
	{
		Roaring prunedRoots;
		for (uint64_t pos0 = 0; pos0 < mmrSize; ++pos0) {
			if (prunedRoot[(size_t)pos0] != 0) {
				prunedRoots.add((uint32_t)(pos0 + 1));
			}
		}

		const size_t numBytes = prunedRoots.getSizeInBytes();
		if (numBytes == 0) {
			return;   // no pruning: PruneList::Load treats a missing file as empty
		}

		std::vector<uint8_t> bytes(numBytes);
		const size_t written = prunedRoots.write((char*)bytes.data());
		if (written != numBytes) {
			throw SegmentException(
				ESegmentError::Mismatch, "Failed to serialize the prune list");
		}

		FileUtil::SafeWriteToFile(prunePath, bytes);
	}
};
