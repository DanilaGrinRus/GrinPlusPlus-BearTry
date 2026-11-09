#pragma once

#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Commitment.h>
#include <cstdint>
#include <map>

// Pure NRD (NoRecentDuplicate) rules, kept separate so they can be tested
// against grin's relative-height cases without constructing a full block.
namespace NrdRules
{
	inline bool IsRelativeHeightSatisfied(
		const uint64_t previousHeight,
		const uint64_t currentHeight,
		const uint64_t relativeHeight,
		const uint64_t cutoffHeight)
	{
		// An entry older than the recent two-week window is not considered.
		if (previousHeight < cutoffHeight) {
			return true;
		}
		if (currentHeight < previousHeight) {
			return false;
		}
		return currentHeight - previousHeight >= relativeHeight;
	}

	inline bool RecordBlockKernel(
		std::map<Commitment, uint64_t>& blockKernels,
		const Commitment& excess,
		const uint64_t height)
	{
		const auto inserted = blockKernels.emplace(excess, height);
		return inserted.second;
	}
}
