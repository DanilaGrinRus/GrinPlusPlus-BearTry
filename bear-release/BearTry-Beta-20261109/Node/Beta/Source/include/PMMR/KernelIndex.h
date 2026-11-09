#pragma once

#include <Core/Models/BlockHeader.h>
#include <Core/Models/OutputLocation.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/Hash.h>
#include <Database/KernelIndexState.h>
#include <memory>
#include <functional>

class IBlockDB;
class ITxHashSet;

// Persistent kernel excess -> PMMR position index used by get_kernel (API-P1-02).
//
// The index is chain-bound: a state record stores the confirmed tip hash,
// height, kernel root, kernel count, entry count and a commutative checksum of
// every entry. Entries live in generations; a rebuild writes a new generation
// and publishes it by one state write. A crash before publication leaves the
// previous complete generation active. ApplyBlock/Rewind update entries and the
// state record inside the same ChainState/BlockDB batch.
namespace KernelIndex
{
	using HeaderLookup = std::function<BlockHeaderPtr(uint64_t height)>;

	// Verifies the index against tip/txhashset. Rebuilds into a new generation
	// when the state is absent, stale, incomplete, or its checksum/count does
	// not match. Returns false without publishing a partial index.
	bool Ensure(
		const std::shared_ptr<IBlockDB>& pBlockDB,
		const std::shared_ptr<ITxHashSet>& pTxHashSet,
		const BlockHeaderPtr& pTip,
		const HeaderLookup& getHeaderByHeight);

	// Entry checksum used by both the builder and runtime updates.
	inline Hash ComputeEntryHash(const Commitment& excess, const OutputLocation& location)
	{
		return KernelIndexEntryHash(excess, location);
	}

	inline Hash XorEntryHash(Hash accumulator, const Commitment& excess, const OutputLocation& location)
	{
		return KernelIndexXorHash(accumulator, excess, location);
	}
}
