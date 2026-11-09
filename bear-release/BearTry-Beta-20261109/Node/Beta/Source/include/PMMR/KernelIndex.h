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
// previous complete generation active. On startup, a verified ancestor may be
// extended together with its state record in the same BlockDB transaction.
// Normal block append and reorg rewind update the index in the same transaction
// as the block/txhashset change; Ensure() remains the verified startup fallback.
namespace KernelIndex
{
	using HeaderLookup = std::function<BlockHeaderPtr(uint64_t height)>;

	// Verifies the index against tip/txhashset. Rebuilds into a new generation
	// when the state is absent, stale, incomplete, or its checksum/count does
	// not match; a nearby verified ancestor is extended in one transaction.
	// Caller must hold an exclusive BlockDB batch transaction. Returns false
	// without publishing a partial index.
	bool Ensure(
		const std::shared_ptr<IBlockDB>& pBlockDB,
		const std::shared_ptr<ITxHashSet>& pTxHashSet,
		const BlockHeaderPtr& pTip,
		const HeaderLookup& getHeaderByHeight);

	// Append the kernels of one block whose parent is the current indexed tip.
	// Writes entries and the new state in the caller's BlockDB transaction; on
	// any mismatch it writes nothing and returns false (the caller may then run
	// Ensure() as a verified fallback). No digest scan is done per block.
	bool AppendBlock(
		const std::shared_ptr<IBlockDB>& pBlockDB,
		const std::shared_ptr<ITxHashSet>& pTxHashSet,
		const BlockHeaderPtr& pPreviousHeader,
		const BlockHeaderPtr& pHeader);

	// Rewind a verified index from the pre-reorg tip to pAncestor. Verifies the
	// header link chain, removes the exact entries above the ancestor and updates
	// the state/checksum in the caller's transaction. On failure it either writes
	// nothing or invalidates the state in the same transaction; the caller must
	// run Ensure() before trusting the index again.
	bool RewindToAncestor(
		const std::shared_ptr<IBlockDB>& pBlockDB,
		const BlockHeaderPtr& pAncestor,
		const BlockHeaderPtr& pExpectedTip,
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
