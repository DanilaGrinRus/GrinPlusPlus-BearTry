#pragma once

#include <Core/Models/BlockHeader.h>
#include <Core/Models/SpentOutput.h>
#include <Core/Traits/Batchable.h>
#include <Database/KernelIndexState.h>
#include <unordered_map>
#include <memory>

// Forward Declarations
class BlockSums;
class FullBlock;
class Commitment;
class OutputLocation;
class Chain;

class IBlockDB : public Traits::IBatchable
{
public:
	using Ptr = std::shared_ptr<IBlockDB>;
	using CPtr = std::shared_ptr<const IBlockDB>;

	virtual ~IBlockDB() = default;

	/// <summary>
	/// Looks up the current serialization format version of the DB.
	/// </summary>
	/// <returns>The current DB format version.</returns>
	virtual uint8_t GetVersion() const = 0;

	/// <summary>
	/// Updates the DB serialization format version.
	/// </summary>
	/// <param name="version">The new version.</param>
	virtual void SetVersion(const uint8_t version) = 0;

	/// <summary>
	/// Updates FullBlock entries to the latest serialization format.
	/// </summary>
	virtual void MigrateBlocks() = 0;

	/// <summary>
	/// Removes all blocks beyond the horizon.
	/// </summary>
	/// <param name="pChain"></param>
	virtual void Compact(const std::shared_ptr<const Chain>& pChain) = 0;

	virtual BlockHeaderPtr GetBlockHeader(const Hash& hash) const = 0;

	virtual void AddBlockHeader(BlockHeaderPtr pBlockHeader) = 0;
	virtual void AddBlockHeaders(const std::vector<BlockHeaderPtr>& blockHeaders) = 0;

	virtual void AddBlock(const FullBlock& block) = 0;
	virtual std::unique_ptr<FullBlock> GetBlock(const Hash& hash) const = 0;
	virtual void ClearBlocks() = 0;

	virtual void AddBlockSums(const Hash& blockHash, const BlockSums& blockSums) = 0;
	virtual std::unique_ptr<BlockSums> GetBlockSums(const Hash& blockHash) const = 0;
	virtual void ClearBlockSums() = 0;

	virtual void AddOutputPosition(const Commitment& outputCommitment, const OutputLocation& location) = 0;
	virtual std::unique_ptr<OutputLocation> GetOutputPosition(const Commitment& outputCommitment) const = 0;
	virtual void RemoveOutputPositions(const std::vector<Commitment>& outputCommitments) = 0;
	virtual void ClearOutputPositions() = 0;

	virtual void AddSpentPositions(const Hash& blockHash, const std::vector<SpentOutput>& outputPositions) = 0;
	virtual std::unordered_map<Commitment, OutputLocation> GetSpentPositions(const Hash& blockHash) const = 0;
	virtual void ClearSpentPositions() = 0;

	// Persistent kernel excess -> PMMR position index.
	virtual std::unique_ptr<KernelIndexState> GetKernelIndexState() const = 0;
	virtual void SetKernelIndexState(const KernelIndexState& state) = 0;
	virtual void AddKernelPosition(uint64_t generation, const Commitment& kernelExcess, const OutputLocation& location) = 0;
	virtual std::unique_ptr<OutputLocation> GetKernelPosition(uint64_t generation, const Commitment& kernelExcess) const = 0;
	// Removes one exact kernel position (excess + location). Excess-only removal
	// is deliberately not exposed: with duplicate excesses it could delete a
	// surviving earlier occurrence.
	virtual void RemoveKernelPosition(uint64_t generation, const Commitment& kernelExcess, const OutputLocation& location) = 0;
	virtual void ClearKernelPositions(uint64_t generation) = 0;
	virtual void ClearAllKernelPositions() = 0;
	// Removes every primary/secondary index entry at or above firstLeafIndex and
	// returns {removedCount, XOR of the removed entry hashes}. Used by a verified
	// reorg rewind; the default refuses (0 removed) so an alternate implementation
	// cannot silently keep a stale index -- callers fall back to a full rebuild.
	virtual std::pair<uint64_t, Hash> RemoveKernelPositionsFrom(uint64_t generation, uint64_t firstLeafIndex)
	{
		(void)generation;
		(void)firstLeafIndex;
		return { 0, Hash() };
	}
	virtual uint64_t GetKernelPositionCount(uint64_t generation) const = 0;
	virtual Hash GetKernelPositionChecksum(uint64_t generation) const = 0;
	// Default preserves compatibility with alternate database implementations.
	virtual std::pair<uint64_t, Hash> GetKernelPositionDigest(uint64_t generation) const
	{
		return { GetKernelPositionCount(generation), GetKernelPositionChecksum(generation) };
	}
};