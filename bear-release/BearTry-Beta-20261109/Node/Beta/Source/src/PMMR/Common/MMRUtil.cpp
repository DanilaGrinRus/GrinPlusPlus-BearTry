#include "MMRUtil.h"

#include <Common/Util/BitUtil.h>

//
// Calculates the postorder traversal index of all peaks in an MMR with the given size (# of nodes).
// Returns empty when the size does not represent a complete MMR (ie. siblings exist, but no parent).
//
std::vector<uint64_t> MMRUtil::GetPeakIndices(const uint64_t size)
{
	std::vector<uint64_t> peakIndices;
	if (size > 0) {
		uint64_t peakSize = BitUtil::FillOnesToRight(size);
		uint64_t numLeft = size;
		uint64_t sumPrevPeaks = 0;
		while (peakSize != 0) {
			if (numLeft >= peakSize) {
				peakIndices.push_back(sumPrevPeaks + peakSize - 1);
				sumPrevPeaks += peakSize;
				numLeft -= peakSize;
			}

			peakSize >>= 1;
		}

		if (numLeft > 0) {
			return std::vector<uint64_t>();
		}
	}

	return peakIndices;
}

std::pair<uint64_t, uint64_t> MMRUtil::PeakMapHeight(uint64_t size)
{
	if (size == 0) {
		return { 0, 0 };
	}

	uint64_t peakSize = BitUtil::FillOnesToRight(size);
	uint64_t peakMap = 0;
	while (peakSize != 0) {
		peakMap <<= 1;
		if (size >= peakSize) {
			size -= peakSize;
			peakMap |= 1;
		}

		peakSize >>= 1;
	}

	return { peakMap, size };
}

uint64_t MMRUtil::InsertionToPmmrIndex(const uint64_t leafIndex0)
{
	return 2 * leafIndex0 - (uint64_t)BitUtil::CountBitsSet(leafIndex0);
}

uint64_t MMRUtil::GetNumLeaves(const uint64_t size)
{
	const std::pair<uint64_t, uint64_t> peakMapHeight = PeakMapHeight(size);
	if (peakMapHeight.second == 0) {
		return peakMapHeight.first;
	}

	return peakMapHeight.first + 1;
}

uint64_t MMRUtil::BintreePostorderHeight(const uint64_t pos0)
{
	return PeakMapHeight(pos0).second;
}

bool MMRUtil::IsLeaf(const uint64_t pos0)
{
	return BintreePostorderHeight(pos0) == 0;
}

bool MMRUtil::IsLeftSibling(const uint64_t pos0)
{
	const std::pair<uint64_t, uint64_t> peakMapHeight = PeakMapHeight(pos0);
	const uint64_t peak = 1ULL << peakMapHeight.second;
	return (peakMapHeight.first & peak) == 0;
}

std::vector<std::pair<uint64_t, uint64_t>> MMRUtil::FamilyBranch(const uint64_t pos0, const uint64_t size)
{
	std::vector<std::pair<uint64_t, uint64_t>> branch;
	const std::pair<uint64_t, uint64_t> peakMapHeight = PeakMapHeight(pos0);
	uint64_t peak = 1ULL << peakMapHeight.second;
	uint64_t current = pos0;
	while (current + 1 < size) {
		uint64_t sibling = 0;
		if ((peakMapHeight.first & peak) != 0) {
			++current;
			sibling = current - 2 * peak;
		} else {
			current += 2 * peak;
			sibling = current - 1;
		}

		if (current >= size) {
			break;
		}

		branch.emplace_back(current, sibling);
		peak <<= 1;
	}

	return branch;
}

uint64_t MMRUtil::BintreeLeftmost(const uint64_t pos0)
{
	const uint64_t height = BintreePostorderHeight(pos0);
	return pos0 + 2 - (2ULL << height);
}

uint64_t MMRUtil::BintreeRightmost(const uint64_t pos0)
{
	return pos0 - BintreePostorderHeight(pos0);
}

std::optional<uint64_t> MMRUtil::PmmrLeafToInsertionIndex(const uint64_t pos0)
{
	const std::pair<uint64_t, uint64_t> peakMapHeight = PeakMapHeight(pos0);
	if (peakMapHeight.second != 0) {
		return std::nullopt;
	}

	return peakMapHeight.first;
}
