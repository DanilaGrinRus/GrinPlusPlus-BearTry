#pragma once

#include <cstdint>

// Upper bound for the legacy txhashset.zip transfer. Mainnet archives are well
// below this; a malicious or broken peer must not be able to make the node
// reserve/write an arbitrary amount of disk before any validation happens.
namespace TxHashSetLimits
{
	static constexpr uint64_t MAX_ZIPPED_SIZE = 8ULL * 1024 * 1024 * 1024; // 8 GiB

	inline bool IsPlausibleSize(const uint64_t zippedSize) noexcept
	{
		return zippedSize > 0 && zippedSize <= MAX_ZIPPED_SIZE;
	}
}
