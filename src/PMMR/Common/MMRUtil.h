// Derived from grin 5.5.2 (Rust), Copyright The Grin Developers.
// Licensed under the Apache License, Version 2.0. Modified for the Grin++ C++ fork.
// See THIRD_PARTY_NOTICES.md and LICENSES/Apache-2.0.txt for details.
#pragma once

#include <PMMR/Common/Index.h>
#include <PMMR/Common/LeafIndex.h>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

class MMRUtil
{
public:
	static std::vector<uint64_t> GetPeakIndices(const uint64_t size);

	//
	// PIBD / segment helpers, ported 1:1 from grin v5.5.2
	// core/src/core/pmmr/pmmr.rs (peak_map_height and friends).
	//

	// Peak bitmap and height of the next node for an MMR of the given size.
	static std::pair<uint64_t, uint64_t> PeakMapHeight(uint64_t size);

	// 0-based PMMR position of the 0-based leaf index.
	static uint64_t InsertionToPmmrIndex(const uint64_t leafIndex0);

	// Number of leaves in an MMR of the given size.
	static uint64_t GetNumLeaves(const uint64_t size);

	// Height of a node in the full binary tree, from its postorder position.
	static uint64_t BintreePostorderHeight(const uint64_t pos0);

	static bool IsLeaf(const uint64_t pos0);

	// Is the node at this position the left sibling of its parent?
	static bool IsLeftSibling(const uint64_t pos0);

	// (parent, sibling) positions from pos0 up to the peak of the tree, staying
	// inside an MMR of the given size.
	static std::vector<std::pair<uint64_t, uint64_t>> FamilyBranch(const uint64_t pos0, const uint64_t size);

	static uint64_t BintreeLeftmost(const uint64_t pos0);
	static uint64_t BintreeRightmost(const uint64_t pos0);

	// 0-based leaf index of a 0-based PMMR position, if the position is a leaf
	// (inverse of InsertionToPmmrIndex).
	static std::optional<uint64_t> PmmrLeafToInsertionIndex(const uint64_t pos0);
};
