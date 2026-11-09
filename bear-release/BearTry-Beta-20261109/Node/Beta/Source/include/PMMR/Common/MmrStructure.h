#pragma once

#include <cstdint>

// Structural validation of a PMMR node file. A valid MMR with L leaves has
// exactly N = 2L - popcount(L) nodes, so many raw file sizes are impossible
// (for example 2, 5, 6, 9, 12). A truncated or partially written append-only
// file can land on such a size; loading it without a check trusts a node count
// that cannot describe any MMR. The functions below are pure and unit-tested.
namespace MmrStructure
{
	inline uint32_t PopCount(uint64_t value) noexcept
	{
		uint32_t count = 0;
		while (value != 0) {
			value &= (value - 1);
			++count;
		}

		return count;
	}

	inline uint64_t NodesForLeaves(const uint64_t leaves) noexcept
	{
		if (leaves == 0) {
			return 0;
		}

		return 2 * leaves - PopCount(leaves);
	}

	inline bool IsValidNodeCount(const uint64_t nodes) noexcept
	{
		if (nodes == 0) {
			return true;
		}

		// NodesForLeaves is strictly increasing for leaves >= 1; binary search
		// the unique candidate instead of trusting an unbounded inversion.
		// For L leaves the node count is 2L - popcount(L), so L is slightly
		// larger than nodes/2 (up to the popcount). nodes is a safe upper bound.
		uint64_t low = 1;
		uint64_t high = nodes;
		while (low <= high) {
			const uint64_t mid = low + (high - low) / 2;
			const uint64_t candidate = NodesForLeaves(mid);
			if (candidate == nodes) {
				return true;
			}

			if (candidate < nodes) {
				low = mid + 1;
			} else {
				if (mid == 0) {
					break;
				}
				high = mid - 1;
			}
		}

		return false;
	}

	// Largest number of nodes that describes a valid MMR not exceeding nodes.
	inline uint64_t LargestValidNodeCount(const uint64_t nodes) noexcept
	{
		uint64_t low = 1;
		uint64_t high = nodes;
		uint64_t best = 0;
		while (low <= high) {
			const uint64_t mid = low + (high - low) / 2;
			const uint64_t candidate = NodesForLeaves(mid);
			if (candidate <= nodes) {
				best = candidate;
				low = mid + 1;
			} else {
				if (mid == 0) {
					break;
				}
				high = mid - 1;
			}
		}

		return best;
	}

	// Number of leaves for a valid node count (0 when it is not valid).
	inline uint64_t LeavesForNodes(const uint64_t nodes) noexcept
	{
		if (!IsValidNodeCount(nodes) || nodes == 0) {
			return 0;
		}

		uint64_t low = 1;
		uint64_t high = nodes;
		while (low <= high) {
			const uint64_t mid = low + (high - low) / 2;
			const uint64_t candidate = NodesForLeaves(mid);
			if (candidate == nodes) {
				return mid;
			}

			if (candidate < nodes) {
				low = mid + 1;
			} else {
				if (mid == 0) {
					break;
				}
				high = mid - 1;
			}
		}

		return 0;
	}
}
