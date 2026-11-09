#pragma once

#include <Database/BlockDb.h>
#include <Database/KernelIndexState.h>
#include <shared_mutex>
#include <Core/Config.h>
#include <caches/Cache.h>
#include <mutex>
#include <set>

// Forward Declarations
class RocksDB;

class BlockDB : public IBlockDB
{
public:
	BlockDB(const Config& config, std::unique_ptr<RocksDB>&& pRocksDB);
	virtual ~BlockDB();

	static std::shared_ptr<BlockDB> OpenDB(const Config& config);

	void Commit() final;
	void Rollback() noexcept final;
	void OnInitWrite(const bool batch) final;
	void OnEndWrite() final;

	uint8_t GetVersion() const final;
	void SetVersion(const uint8_t version) final;

	// Consistent point-in-time checkpoint of the open chain RocksDB.
	bool CreateCheckpoint(const fs::path& checkpointPath) const;

	void MigrateBlocks() final;
	void Compact(const std::shared_ptr<const Chain>& pChain) final;

	BlockHeaderPtr GetBlockHeader(const Hash& hash) const final;

	void AddBlockHeader(BlockHeaderPtr pBlockHeader) final;
	void AddBlockHeaders(const std::vector<BlockHeaderPtr>& blockHeaders) final;

	void AddBlock(const FullBlock& block) final;
	std::unique_ptr<FullBlock> GetBlock(const Hash& hash) const final;
	void ClearBlocks() final;

	void AddBlockSums(const Hash& blockHash, const BlockSums& blockSums) final;
	std::unique_ptr<BlockSums> GetBlockSums(const Hash& blockHash) const final;
	void ClearBlockSums() final;

	void AddOutputPosition(const Commitment& outputCommitment, const OutputLocation& location) final;
	std::unique_ptr<OutputLocation> GetOutputPosition(const Commitment& outputCommitment) const final;
	void RemoveOutputPositions(const std::vector<Commitment>& outputCommitments) final;
	void ClearOutputPositions() final;

	void AddSpentPositions(const Hash& blockHash, const std::vector<SpentOutput>& outputPostions) final;
	std::unordered_map<Commitment, OutputLocation> GetSpentPositions(const Hash& blockHash) const final;
	void ClearSpentPositions() final;

	std::unique_ptr<KernelIndexState> GetKernelIndexState() const final;
	void SetKernelIndexState(const KernelIndexState& state) final;
	void AddKernelPosition(uint64_t generation, const Commitment& kernelExcess, const OutputLocation& location) final;
	std::unique_ptr<OutputLocation> GetKernelPosition(uint64_t generation, const Commitment& kernelExcess) const final;
	void RemoveKernelPosition(uint64_t generation, const Commitment& kernelExcess, const OutputLocation& location) final;
	std::pair<uint64_t, Hash> RemoveKernelPositionsFrom(uint64_t generation, uint64_t firstLeafIndex) final;
	void ClearKernelPositions(uint64_t generation) final;
	void ClearAllKernelPositions() final;
	uint64_t GetKernelPositionCount(uint64_t generation) const final;
	Hash GetKernelPositionChecksum(uint64_t generation) const final;
	std::pair<uint64_t, Hash> GetKernelPositionDigest(uint64_t generation) const final;

private:
	const Config& m_config;
	std::unique_ptr<RocksDB> m_pRocksDB;
	mutable std::shared_mutex m_cacheMutex;
	FIFOCache<Hash, BlockHeaderPtr> m_blockHeadersCache;

	std::vector<BlockHeaderPtr> m_uncommitted;
};