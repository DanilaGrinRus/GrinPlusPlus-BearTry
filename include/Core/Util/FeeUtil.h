#pragma once

#include <Consensus.h>
#include <limits>
#include <stdexcept>

class FeeUtil
{
public:
	static uint64_t CalculateFee(const uint64_t feeBase, const int64_t numInputs, const int64_t numOutputs, const int64_t numKernels)
	{
		const uint64_t weight = Consensus::CalculateWeightV5(numInputs, numOutputs, numKernels);
		if (weight && feeBase > (std::numeric_limits<uint64_t>::max)() / weight) {
			throw std::overflow_error("Fee exceeds uint64 range");
		}
		return feeBase * weight;
	}
};