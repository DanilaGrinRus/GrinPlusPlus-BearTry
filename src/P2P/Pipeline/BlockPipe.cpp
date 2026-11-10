#include "BlockPipe.h"
#include "../Messages/BlockMessage.h"
#include "../Messages/TransactionKernelMessage.h"
#include "../ConnectionManager.h"

#include <Common/Util/ThreadUtil.h>
#include <Common/Logger.h>
#include <BlockChain/BlockChain.h>
#include <Core/Models/BlockHeader.h>

BlockPipe::BlockPipe(const Config& config, const IBlockChain::Ptr& pBlockChain)
	: m_config(config), m_pBlockChain(pBlockChain), m_terminate(false)
{
}

BlockPipe::~BlockPipe()
{
	{
		std::lock_guard<std::mutex> lock(m_rawMutex);
		m_terminate = true;
	}
	m_rawCv.notify_all();

	ThreadUtil::Join(m_blockThread);
	ThreadUtil::Join(m_processThread);

	std::lock_guard<std::mutex> lock(m_rawMutex);
	m_rawBlocks.clear();
	m_rawBytes = 0;
}

std::shared_ptr<BlockPipe> BlockPipe::Create(const Config& config, const IBlockChain::Ptr& pBlockChain)
{
	std::shared_ptr<BlockPipe> pBlockPipe = std::shared_ptr<BlockPipe>(new BlockPipe(config, pBlockChain));
	pBlockPipe->m_blockThread = std::thread(Thread_ProcessNewBlocks, std::ref(*pBlockPipe.get()));
	pBlockPipe->m_processThread = std::thread(Thread_PostProcessBlocks, std::ref(*pBlockPipe.get()));

	return pBlockPipe;
}

void BlockPipe::Thread_ProcessNewBlocks(BlockPipe& pipeline)
{
	LoggerAPI::SetThreadName("BLOCK_PREPROCESS_PIPE");
	LOG_TRACE("BEGIN");

	while (!pipeline.m_terminate && Global::IsRunning())
	{
		RawBlockEntry rawEntry;
		bool hasRaw = false;

		{
			std::unique_lock<std::mutex> lock(pipeline.m_rawMutex);
			pipeline.m_rawCv.wait_for(lock, std::chrono::milliseconds(10), [&pipeline]() {
				return pipeline.m_terminate || !pipeline.m_rawBlocks.empty();
			});

			if (pipeline.m_terminate) {
				break;
			}

			if (!pipeline.m_rawBlocks.empty()) {
				// Bodies are applied at the CONFIRMED tip. The CANDIDATE header
				// chain is usually ahead during catch-up; using it would make every
				// body look stale and silently drop it.
				const uint64_t confirmedHeight =
					pipeline.m_pBlockChain->GetHeight(EChainType::CONFIRMED);
				const uint64_t confirmedDifficulty =
					pipeline.m_pBlockChain->GetTotalDifficulty(EChainType::CONFIRMED);
				const BlockHeaderPtr pConfirmedTip =
					pipeline.m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
				const uint64_t nextHeight = confirmedHeight + 1;

				// Discard bodies that cannot become a chain mutation. A body at the
				// confirmed height is kept only if it is a stronger fork candidate.
				for (auto it = pipeline.m_rawBlocks.begin();
					it != pipeline.m_rawBlocks.end(); ) {
					const bool stale = it->first < confirmedHeight ||
						(it->first == confirmedHeight &&
							it->second.m_totalDifficulty <= confirmedDifficulty);
					if (stale) {
						pipeline.m_rawBytes -= it->second.m_bytes.size();
						it = pipeline.m_rawBlocks.erase(it);
					} else {
						++it;
					}
				}

				// Stronger same-height alternative: reorg to it if its parent is
				// the current confirmed tip's parent and its total difficulty is
				// strictly higher.
				if (pConfirmedTip != nullptr) {
					auto best = pipeline.m_rawBlocks.end();
					const auto range = pipeline.m_rawBlocks.equal_range(confirmedHeight);
					for (auto it = range.first; it != range.second; ++it) {
						if (it->second.m_totalDifficulty > confirmedDifficulty &&
							it->second.m_previousHash == pConfirmedTip->GetPreviousHash() &&
							(best == pipeline.m_rawBlocks.end() ||
								it->second.m_totalDifficulty > best->second.m_totalDifficulty)) {
							best = it;
						}
					}
					if (best != pipeline.m_rawBlocks.end()) {
						rawEntry = std::move(best->second);
						pipeline.m_rawBytes -= rawEntry.m_bytes.size();
						pipeline.m_rawBlocks.erase(best);
						hasRaw = true;
					}
				}

				// Pick the strongest branch at the next confirmed height whose
				// previous hash matches the confirmed tip.
				if (!hasRaw) {
					auto best = pipeline.m_rawBlocks.end();
					const auto range = pipeline.m_rawBlocks.equal_range(nextHeight);
					for (auto it = range.first; it != range.second; ++it) {
						if (pConfirmedTip == nullptr ||
							it->second.m_previousHash == pConfirmedTip->GetHash()) {
							if (best == pipeline.m_rawBlocks.end() ||
								it->second.m_totalDifficulty > best->second.m_totalDifficulty) {
								best = it;
							}
						}
					}
					if (best != pipeline.m_rawBlocks.end()) {
						rawEntry = std::move(best->second);
						pipeline.m_rawBytes -= rawEntry.m_bytes.size();
						pipeline.m_rawBlocks.erase(best);
						hasRaw = true;
					}
				}

				// Bound the hold time for bodies that never get a parent.
				const auto now = std::chrono::steady_clock::now();
				for (auto it = pipeline.m_rawBlocks.begin(); it != pipeline.m_rawBlocks.end(); ) {
					if (now - it->second.m_enqueued > std::chrono::seconds(60)) {
						LOG_WARNING_F("Deferred block height {} timed out waiting for parent/branch; dropping for resync.",
							it->first);
						pipeline.m_rawBytes -= it->second.m_bytes.size();
						it = pipeline.m_rawBlocks.erase(it);
					} else {
						++it;
					}
				}
			}
		}

		if (hasRaw) {
			ProcessRawBlock(pipeline, std::move(rawEntry));
			continue;
		}

		// Non-catch-up path (and test doubles): already-deserialized blocks.
		// AddBlock takes the state write lock and commits parent state before the
		// lock is released. Keep the batch in receive order so a child cannot
		// acquire the lock before its parent.
		std::vector<BlockEntry> blocksToProcess = pipeline.m_blocksToProcess.copy_front(8);
		if (!blocksToProcess.empty())
		{
			for (const BlockEntry& blockEntry : blocksToProcess)
			{
				ProcessNewBlock(pipeline, blockEntry);
			}

			pipeline.m_blocksToProcess.pop_front(blocksToProcess.size());
		}
		else
		{
			ThreadUtil::SleepFor(std::chrono::milliseconds(5));
		}
	}

	LOG_TRACE("END");
}

void BlockPipe::ProcessRawBlock(BlockPipe& pipeline, RawBlockEntry&& blockEntry)
{
	try
	{
		ByteBuffer body(blockEntry.m_bytes, blockEntry.m_version);
		const BlockMessage blockMessage = BlockMessage::Deserialize(body);
		ProcessNewBlock(pipeline, BlockEntry(blockEntry.m_peer, blockMessage.GetBlock()));
	}
	catch (const std::exception& e)
	{
		// Deserialization should only run after the parent is committed. A
		// failure here is local ordering/state, not proof that the peer is bad.
		LOG_ERROR_F("Deferred block deserialization failed at height {}: {}; no ban.",
			blockEntry.m_height, e.what());
	}
}

void BlockPipe::ProcessNewBlock(BlockPipe& pipeline, const BlockEntry& blockEntry)
{
	try
	{
		const EBlockChainStatus status = pipeline.m_pBlockChain->AddBlock(blockEntry.m_block);
		if (status == EBlockChainStatus::INVALID)
		{
			blockEntry.m_peer->Ban(EBanReason::BadBlock);
		}
		else if (status == EBlockChainStatus::LOCAL_REWIND_DATA_MISSING)
		{
			// The peer's block was not classified as invalid. Do not ban; the
			// operator/sync layer needs local state recovery (restore bodies or
			// resync the state), not a peer ban.
			LOG_WARNING_F(
				"Block {} needs local rewind data that is unavailable; peer {} is not banned. "
				"Local state recovery/resync is required before retrying.",
				blockEntry.m_block.GetHash(),
				blockEntry.m_peer->GetIPAddress().Format());
		}
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Exception ({}) caught while attempting to add block {}; peer validity is unknown, no ban.", e.what(), blockEntry.m_block);
	}
}

void BlockPipe::Thread_PostProcessBlocks(BlockPipe& pipeline)
{
	LoggerAPI::SetThreadName("BLOCK_POSTPROCESS_PIPE");
	LOG_TRACE("BEGIN");

	while (!pipeline.m_terminate && Global::IsRunning())
	{
		if (!pipeline.m_pBlockChain->ProcessNextOrphanBlock())
		{
			ThreadUtil::SleepFor(std::chrono::milliseconds(5));
		}
	}

	LOG_TRACE("END");
}

bool BlockPipe::AddBlockToProcess(PeerPtr pPeer, const FullBlock& block)
{
	if (m_blocksToProcess.size() >= MAX_PENDING_RAW_BLOCKS) {
		LOG_WARNING_F("Block queue is full ({}); dropping block {}.",
			MAX_PENDING_RAW_BLOCKS, block.GetHeight());
		return false;
	}

	std::function<bool(const BlockEntry&, const BlockEntry&)> comparator = [](const BlockEntry& blockEntry1, const BlockEntry& blockEntry2)
	{
		return blockEntry1.m_block.GetHash() == blockEntry2.m_block.GetHash();
	};

	return m_blocksToProcess.push_back_unique(BlockEntry(pPeer, block), comparator);
}

bool BlockPipe::AddBlockToProcess(PeerPtr pPeer, ByteBuffer blockBody)
{
	const size_t remaining = blockBody.GetRemainingSize();
	if (remaining == 0 || remaining > MAX_RAW_BLOCK_BYTES) {
		LOG_WARNING_F("Rejected serialized block with invalid size {}.", remaining);
		return false;
	}

	std::vector<uint8_t> bytes = blockBody.ReadRemainingBytes();
	const EProtocolVersion version = blockBody.GetProtocolVersion();

	RawBlockEntry entry;
	entry.m_peer = pPeer;
	entry.m_version = version;
	entry.m_bytes = std::move(bytes);
	try {
		ByteBuffer headerBuffer(entry.m_bytes, version);
		const BlockHeader header = BlockHeader::Deserialize(headerBuffer);
		entry.m_height = header.GetHeight();
		entry.m_totalDifficulty = header.GetTotalDifficulty();
		entry.m_hash = header.GetHash();
		entry.m_previousHash = header.GetPreviousHash();
	} catch (const std::exception& e) {
		LOG_WARNING_F("Rejected malformed serialized block: {}; no ban.", e.what());
		return false;
	}

	std::lock_guard<std::mutex> lock(m_rawMutex);
	if (m_terminate) {
		return false;
	}
	if (m_rawBlocks.size() >= MAX_PENDING_RAW_BLOCKS ||
		m_rawBytes + entry.m_bytes.size() > MAX_PENDING_RAW_BYTES) {
		LOG_WARNING_F("Deferred block buffer is full ({} blocks, {} bytes); dropping height {}.",
			m_rawBlocks.size(), m_rawBytes, entry.m_height);
		return false;
	}

	// A height may legitimately have multiple branches during fork/reorg. Keep
	// them distinct; the worker picks the one whose parent is the confirmed tip.
	const auto range = m_rawBlocks.equal_range(entry.m_height);
	for (auto it = range.first; it != range.second; ++it) {
		if (it->second.m_hash == entry.m_hash) {
			return true;
		}
	}

	m_rawBytes += entry.m_bytes.size();
	m_rawBlocks.emplace(entry.m_height, std::move(entry));
	m_rawCv.notify_one();
	return true;
}

bool BlockPipe::IsProcessingBlock(const Hash& hash) const
{
	{
		std::lock_guard<std::mutex> lock(m_rawMutex);
		for (const auto& item : m_rawBlocks) {
			if (item.second.m_hash == hash) {
				return true;
			}
		}
	}

	std::function<bool(const BlockEntry&, const Hash&)> comparator = [](const BlockEntry& blockEntry, const Hash& hash)
	{
		return blockEntry.m_block.GetHash() == hash;
	};

	return m_blocksToProcess.contains<Hash>(hash, comparator) || m_pBlockChain->HasOrphan(hash);
}

