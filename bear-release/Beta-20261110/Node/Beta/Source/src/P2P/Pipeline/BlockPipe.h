#pragma once

#include <Crypto/Models/Hash.h>
#include <P2P/Peer.h>
#include <Core/Models/FullBlock.h>
#include <BlockChain/BlockChain.h>
#include <Common/ConcurrentQueue.h>
#include <Core/Serialization/ByteBuffer.h>
#include <condition_variable>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <cstdint>
#include <atomic>
#include <thread>

// Forward Declarations
class Config;
class TxHashSetArchiveMessage;
class Transaction;

class BlockPipe
{
public:
	static std::shared_ptr<BlockPipe> Create(
		const Config& config,
		const IBlockChain::Ptr& pBlockChain
	);
	~BlockPipe();

	bool AddBlockToProcess(PeerPtr pPeer, const FullBlock& block);
	bool AddBlockToProcess(PeerPtr pPeer, ByteBuffer blockBody);
	bool IsProcessingBlock(const Hash& hash) const;

private:
	BlockPipe(const Config& config, const IBlockChain::Ptr& pBlockChain);

	const Config& m_config;
	IBlockChain::Ptr m_pBlockChain;

	struct BlockEntry
	{
		BlockEntry(PeerPtr pPeer, const FullBlock& fullBlock)
			: m_peer(pPeer), m_block(fullBlock)
		{

		}

		PeerPtr m_peer;
		FullBlock m_block;
	};

	// A block received during initial catch-up is kept serialized until its
	// parent has been committed. V3 TransactionInput::Deserialize resolves the
	// output features through Global::GetCoinView(), so deserializing a child
	// before its parent is not recoverable; the raw bytes must be buffered.
	struct RawBlockEntry
	{
		PeerPtr m_peer;
		std::vector<uint8_t> m_bytes;
		EProtocolVersion m_version{ EProtocolVersion::V1 };
		uint64_t m_height{ 0 };
		uint64_t m_totalDifficulty{ 0 };
		Hash m_hash;
		Hash m_previousHash;
		std::chrono::steady_clock::time_point m_enqueued{ std::chrono::steady_clock::now() };
	};

	static constexpr size_t MAX_PENDING_RAW_BLOCKS = 512;
	static constexpr size_t MAX_RAW_BLOCK_BYTES = 16 * 1024 * 1024;
	static constexpr size_t MAX_PENDING_RAW_BYTES = 64 * 1024 * 1024;

	// Pre-Process New Blocks
	static void Thread_ProcessNewBlocks(BlockPipe& pipeline);
	static void ProcessNewBlock(BlockPipe& pipeline, const BlockEntry& blockEntry);
	static void ProcessRawBlock(BlockPipe& pipeline, RawBlockEntry&& blockEntry);
	std::thread m_blockThread;
	ConcurrentQueue<BlockEntry> m_blocksToProcess;

	mutable std::mutex m_rawMutex;
	std::condition_variable m_rawCv;
	std::multimap<uint64_t, RawBlockEntry> m_rawBlocks;
	size_t m_rawBytes{ 0 };

	// Process Next Block
	std::thread m_processThread;
	static void Thread_PostProcessBlocks(BlockPipe& pipeline);

	std::atomic_bool m_terminate;
};