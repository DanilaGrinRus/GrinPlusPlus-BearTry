#include "BlockDBImpl.h"
#include "RocksDB/RocksDBFactory.h"
#include "RocksDB/RocksDB.h"

#include <BlockChain/Chain.h>
#include <Core/Models/FullBlock.h>
#include <Core/Models/BlockSums.h>
#include <Core/Models/OutputLocation.h>
#include <Database/DatabaseException.h>
#include <Database/KernelIndexState.h>
#include <PMMR/Common/LeafIndex.h>
#include <Crypto/Hasher.h>
#include <cstdio>
#include <cstring>
#include <Common/Logger.h>
#include <Common/Util/HexUtil.h>
#include <Common/Util/StringUtil.h>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <utility>
#include <string>
#include <filesystem.h>

using namespace rocksdb;

namespace
{
	// Temporary task-11 diagnostics. Default off; no default-path behavior change.
	bool SpentDiagnosticsEnabled()
	{
		static const bool enabled = []() {
#if defined(_WIN32)
			char* pValue = nullptr;
			size_t valueLength = 0;
			if (_dupenv_s(&pValue, &valueLength, "GRINPP_SPENT_DIAG") != 0 ||
				pValue == nullptr) {
				return false;
			}

			const std::string value(pValue);
			std::free(pValue);
#else
			const char* raw = std::getenv("GRINPP_SPENT_DIAG");
			if (raw == nullptr) {
				return false;
			}
			const std::string value(raw);
#endif
			return value == "1" || value == "true" || value == "TRUE" || value == "yes";
		}();
		return enabled;
	}

	std::string ReadCurrentManifestName(const fs::path& dbPath)
	{
		std::ifstream currentFile(dbPath / "CURRENT");
		std::string name;
		std::getline(currentFile, name);
		if (!name.empty() && name.back() == '\r') {
			name.pop_back();
		}
		return name;
	}
}

BlockDB::BlockDB(const Config& config, std::unique_ptr<RocksDB>&& pRocksDB)
	: m_config(config), m_pRocksDB(std::move(pRocksDB)), m_blockHeadersCache(128)
{
}

BlockDB::~BlockDB()
{
	m_pRocksDB.reset();
}

std::shared_ptr<BlockDB> BlockDB::OpenDB(const Config& config)
{
	fs::path dbPath = config.GetDatabasePath() / "CHAIN/";

	ColumnFamilyDescriptor BLOCK_COLUMN = ColumnFamilyDescriptor("BLOCK", *ColumnFamilyOptions().OptimizeForPointLookup(1024));
	ColumnFamilyDescriptor HEADER_COLUMN = ColumnFamilyDescriptor("HEADER", *ColumnFamilyOptions().OptimizeForPointLookup(1024));
	ColumnFamilyDescriptor BLOCK_SUMS_COLUMN = ColumnFamilyDescriptor("BLOCK_SUMS", *ColumnFamilyOptions().OptimizeForPointLookup(1024));
	ColumnFamilyDescriptor OUTPUT_POS_COLUMN = ColumnFamilyDescriptor("OUTPUT_POS", *ColumnFamilyOptions().OptimizeForPointLookup(1024));
	ColumnFamilyDescriptor INPUT_BITMAP_COLUMN = ColumnFamilyDescriptor("INPUT_BITMAP", *ColumnFamilyOptions().OptimizeForPointLookup(1024));
	ColumnFamilyDescriptor SPENT_OUTPUTS_COLUMN = ColumnFamilyDescriptor("SPENT_OUTPUTS", *ColumnFamilyOptions().OptimizeForPointLookup(1024));

	std::vector<ColumnFamilyDescriptor> tableNames = { ColumnFamilyDescriptor(), BLOCK_COLUMN, HEADER_COLUMN, BLOCK_SUMS_COLUMN, OUTPUT_POS_COLUMN, INPUT_BITMAP_COLUMN, SPENT_OUTPUTS_COLUMN };
	std::unique_ptr<RocksDB> pRocksDB = RocksDBFactory::Open(dbPath, tableNames);

	return std::make_shared<BlockDB>(config, std::move(pRocksDB));
}

void BlockDB::Commit()
{
	m_pRocksDB->Commit();

	for (auto pHeader : m_uncommitted)
	{
		m_blockHeadersCache.Put(pHeader->GetHash(), pHeader);
	}

	m_uncommitted.clear();
}

void BlockDB::Rollback() noexcept
{
	m_uncommitted.clear();
	m_pRocksDB->Rollback();
}

class DBVersion : public Traits::ISerializable
{
public:
	DBVersion(const uint8_t version) : m_version(version) { }

	uint8_t Get() const noexcept { return m_version; }

	void Serialize(Serializer& serializer) const final
	{
		serializer.Append<uint8_t>(m_version);
	}

	static DBVersion Deserialize(ByteBuffer& byteBuffer)
	{
		return DBVersion(byteBuffer.ReadU8());
	}

private:
	uint8_t m_version;
};

uint8_t BlockDB::GetVersion() const
{
	auto pVersion = m_pRocksDB->Get<DBVersion>("default", "VERSION");
	if (pVersion != nullptr) {
		return pVersion->Get();
	}

	return 0;
}

void BlockDB::SetVersion(const uint8_t version)
{
	m_pRocksDB->Put("default", DBEntry("VERSION", DBVersion(version)));
}

bool BlockDB::CreateCheckpoint(const fs::path& checkpointPath) const
{
	return m_pRocksDB->CreateCheckpoint(checkpointPath);
}

void BlockDB::MigrateBlocks()
{
	//std::vector<FullBlock> blocks;

	//auto iter = m_pRocksDB->GetIterator("BLOCK");
	//for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
	//	rocksdb::Slice key = iter->key();
	//	try {
	//		auto pBlock = m_pRocksDB->Get<FullBlock>("BLOCK", key, EProtocolVersion::V1);
	//		blocks.push_back(std::move(*pBlock));
	//	}
	//	catch (std::exception& e) {
	//		LOG_DEBUG_F("Failed to migrate block {}. Error: {}", key.data(), e.what());
	//	}
	//}

	//for (const FullBlock& block : blocks)
	//{
	//	const Hash& hash = block.GetHash();
	//	rocksdb::Slice key((const char*)hash.data(), hash.size());
	//	m_pRocksDB->Put("BLOCK", DBEntry(key, block));
	//}
}

void BlockDB::Compact(const std::shared_ptr<const Chain>& pChain)
{
	if (pChain == nullptr) throw DATABASE_EXCEPTION("Cannot compact blocks without a confirmed chain");
	std::vector<std::string> blocks_to_remove;

	const uint64_t horizon = Consensus::GetHorizonHeight(pChain->GetHeight());
	auto iter = m_pRocksDB->GetIterator("BLOCK");
	for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
		rocksdb::Slice key = iter->key();
		try {
			auto pBlock = m_pRocksDB->Get<FullBlock>("BLOCK", key);
			if (pBlock == nullptr) {
				throw DATABASE_EXCEPTION("Block disappeared during compaction scan");
			}
			if (pBlock->GetHeight() < horizon) {
				blocks_to_remove.push_back(key.ToString());
			}
		}
		catch (const std::exception& e) {
			// A failed read cannot establish the block height. Abort before any
			// deletions instead of treating an unreadable block as expired.
			LOG_ERROR_F("Aborting compaction: cannot read block {}: {}", key.ToString(true), e.what());
			throw DATABASE_EXCEPTION("Block compaction scan failed; no blocks deleted");
		}
	}
	if (!iter->status().ok()) {
		LOG_ERROR_F("Aborting compaction: block iterator failed: {}", iter->status().ToString());
		throw DATABASE_EXCEPTION("Block compaction iteration failed; no blocks deleted");
	}

	m_pRocksDB->DeleteBatch("BLOCK", blocks_to_remove);
}

BlockHeaderPtr BlockDB::GetBlockHeader(const Hash& hash) const
{
	if (m_blockHeadersCache.Cached(hash))
	{
		return m_blockHeadersCache.Get(hash);
	}

	rocksdb::Slice key((const char*)hash.data(), hash.size());
	auto pBlockHeader = m_pRocksDB->Get<BlockHeader>("HEADER", key);
	if (pBlockHeader != nullptr)
	{
		return std::shared_ptr<BlockHeader>(std::move(pBlockHeader));
	}

	return nullptr;
}

void BlockDB::AddBlockHeader(BlockHeaderPtr pBlockHeader)
{
	LOG_TRACE_F("Adding header {}", *pBlockHeader);

	const Hash& hash = pBlockHeader->GetHash();
	rocksdb::Slice key((const char*)hash.data(), hash.size());
	m_pRocksDB->Put("HEADER", DBEntry<BlockHeader>(key, *pBlockHeader));

	if (m_pRocksDB->IsTransactional())
	{
		m_uncommitted.push_back(pBlockHeader);
	}
	else
	{
		m_blockHeadersCache.Put(pBlockHeader->GetHash(), pBlockHeader);
	}
}

void BlockDB::AddBlockHeaders(const std::vector<BlockHeaderPtr>& blockHeaders)
{
	LOG_TRACE_F("Adding {} headers.", blockHeaders.size());

	std::vector<DBEntry<BlockHeader>> entries;
	entries.reserve(blockHeaders.size());

	for (auto pBlockHeader : blockHeaders)
	{
		const Hash& hash = pBlockHeader->GetHash();
		rocksdb::Slice key((const char*)hash.data(), hash.size());
		entries.push_back({ key, *pBlockHeader });
	}

	m_pRocksDB->Put("HEADER", entries);

	LOG_TRACE("Finished adding headers.");
}

void BlockDB::AddBlock(const FullBlock& block)
{
	LOG_TRACE_F("Adding block {}", block);

	const Hash& hash = block.GetHash();
	rocksdb::Slice key((const char*)hash.data(), hash.size());
	m_pRocksDB->Put("BLOCK", DBEntry<FullBlock>(key, block));
}

std::unique_ptr<FullBlock> BlockDB::GetBlock(const Hash& hash) const
{
	rocksdb::Slice key((const char*)hash.data(), hash.size());
	return m_pRocksDB->Get<FullBlock>("BLOCK", key);
}

void BlockDB::ClearBlocks()
{
	LOG_WARNING("Deleting all blocks.");

	m_pRocksDB->DeleteAll("BLOCK");
}

void BlockDB::AddBlockSums(const Hash& blockHash, const BlockSums& blockSums)
{
	LOG_TRACE_F("Adding BlockSums for block {}", blockHash);

	rocksdb::Slice key((const char*)blockHash.data(), blockHash.size());
	m_pRocksDB->Put("BLOCK_SUMS", DBEntry<BlockSums>(key, blockSums));
}

std::unique_ptr<BlockSums> BlockDB::GetBlockSums(const Hash& blockHash) const
{
	rocksdb::Slice key((const char*)blockHash.data(), blockHash.size());
	return m_pRocksDB->Get<BlockSums>("BLOCK_SUMS", key);
}

void BlockDB::ClearBlockSums()
{
	LOG_WARNING("Deleting all block sums.");

	m_pRocksDB->DeleteAll("BLOCK_SUMS");
}

void BlockDB::AddOutputPosition(const Commitment& outputCommitment, const OutputLocation& location)
{
	rocksdb::Slice key((const char*)outputCommitment.data(), outputCommitment.size());

	m_pRocksDB->Put("OUTPUT_POS", DBEntry<OutputLocation>(key, location));
}

std::unique_ptr<OutputLocation> BlockDB::GetOutputPosition(const Commitment& outputCommitment) const
{
	rocksdb::Slice key((const char*)outputCommitment.data(), outputCommitment.size());
	return m_pRocksDB->Get<OutputLocation>("OUTPUT_POS", key);
}

void BlockDB::RemoveOutputPositions(const std::vector<Commitment>& outputCommitments)
{
	std::vector<std::string> keys;
	std::transform(
		outputCommitments.begin(), outputCommitments.end(),
		std::back_inserter(keys),
		[](const Commitment& commit) { return std::string((const char*)commit.data(), commit.size()); }
	);

	m_pRocksDB->Delete("OUTPUT_POS", keys);
}

void BlockDB::ClearOutputPositions()
{
	LOG_WARNING("Deleting all output positions.");

	m_pRocksDB->DeleteAll("OUTPUT_POS");
}

void BlockDB::AddSpentPositions(const Hash& blockHash, const std::vector<SpentOutput>& outputPositions)
{
	assert(outputPositions.size() < (size_t)UINT16_MAX);

	rocksdb::Slice key((const char*)blockHash.data(), blockHash.size());
	m_pRocksDB->Put("SPENT_OUTPUTS", DBEntry<SpentOutputs>(key, std::make_unique<SpentOutputs>(outputPositions)));
}

std::unordered_map<Commitment, OutputLocation> BlockDB::GetSpentPositions(const Hash& blockHash) const
{
	rocksdb::Slice key((const char*)blockHash.data(), blockHash.size());

	const bool diagnosticsEnabled = SpentDiagnosticsEnabled();
	std::optional<std::string> rawValue;
	if (diagnosticsEnabled)
	{
		rawValue = m_pRocksDB->GetRawValue("SPENT_OUTPUTS", key);
	}

	auto pSpent = m_pRocksDB->Get<SpentOutputs>("SPENT_OUTPUTS", key);
	if (diagnosticsEnabled)
	{
		const fs::path dbPath = m_config.GetDatabasePath() / "CHAIN";
		const std::string manifestName = ReadCurrentManifestName(dbPath);
		std::error_code sizeError;
		const uintmax_t manifestBytes = manifestName.empty()
			? 0
			: fs::file_size(dbPath / manifestName, sizeError);

		std::string rawHex;
		if (rawValue.has_value())
		{
			const std::vector<unsigned char> rawBytes(rawValue->begin(), rawValue->end());
			rawHex = HexUtil::ConvertToHex(rawBytes);
			if (rawHex.size() > 512) {
				rawHex = rawHex.substr(0, 512) + "...";
			}
		}

		const std::vector<unsigned char> keyBytes(blockHash.data(), blockHash.data() + blockHash.size());
		LOG_WARNING_F(
			"SPENT_DIAG GetSpentPositions db={} CURRENT={} MANIFEST={} manifestBytes={} CF=SPENT_OUTPUTS key={} rawPresent={} rawBytes={} rawHex={} mapEntries={}",
			dbPath.string(),
			manifestName,
			manifestName,
			manifestBytes,
			HexUtil::ConvertToHex(keyBytes),
			rawValue.has_value(),
			rawValue.has_value() ? rawValue->size() : 0,
			rawHex,
			pSpent != nullptr ? pSpent->BuildMap().size() : 0);
	}

	if (pSpent == nullptr)
	{
		LOG_ERROR_F("Failed to retrieve spent positions for block ({})", blockHash);
		throw DATABASE_EXCEPTION("Failed to retrieve spent positions");
	}

	return pSpent->BuildMap();
}

void BlockDB::ClearSpentPositions()
{
	LOG_WARNING("Deleting all spent positions.");

	m_pRocksDB->DeleteAll("SPENT_OUTPUTS");
}

namespace
{
	// Schema v2: kernel positions are keyed by their unique PMMR position, with a
	// secondary excess -> position mapping used by get_kernel. The old
	// KERNELPOS/ KERNELIDX/STATE keys are intentionally ignored (lazy rebuild);
	// deleting the legacy prefix is deferred to a migration step.
	constexpr char KERNEL_POS_PREFIX[] = "KERNELPOS2/";
	constexpr char KERNEL_POS_STATE_KEY[] = "KERNELIDX2/STATE";
	constexpr char KERNEL_POS_TABLE[] = "default";

	std::string GenerationPrefix(const uint64_t generation)
	{
		char generationHex[17] = {};
		std::snprintf(generationHex, sizeof(generationHex), "%016llx", static_cast<unsigned long long>(generation));
		std::string prefix(KERNEL_POS_PREFIX);
		prefix.append(generationHex);
		prefix.push_back('/');
		return prefix;
	}

	// Big-endian encoding keeps RocksDB iterator order == numeric position order.
	std::string EncodePosition(const uint64_t position)
	{
		std::string encoded(8, '\0');
		for (size_t i = 0; i < 8; ++i) {
			encoded[i] = static_cast<char>((position >> (8 * (7 - i))) & 0xFF);
		}
		return encoded;
	}

	std::string PrimaryPrefix(const uint64_t generation)
	{
		return GenerationPrefix(generation) + "P/";
	}

	std::string SecondaryPrefix(const uint64_t generation, const Commitment& kernelExcess)
	{
		std::string prefix = GenerationPrefix(generation) + "E/";
		prefix.append(reinterpret_cast<const char*>(kernelExcess.data()), kernelExcess.size());
		return prefix;
	}

	std::string PrimaryKey(
		const uint64_t generation,
		const Commitment& kernelExcess,
		const OutputLocation& location)
	{
		std::string key = PrimaryPrefix(generation);
		key.append(EncodePosition(location.GetLeafIndex().GetPosition()));
		key.append(reinterpret_cast<const char*>(kernelExcess.data()), kernelExcess.size());
		return key;
	}

	std::string SecondaryKey(
		const uint64_t generation,
		const Commitment& kernelExcess,
		const OutputLocation& location)
	{
		std::string key = SecondaryPrefix(generation, kernelExcess);
		key.append(EncodePosition(location.GetLeafIndex().GetPosition()));
		return key;
	}

	std::string SecondaryKey(
		const uint64_t generation,
		const Commitment& kernelExcess,
		const uint64_t position)
	{
		std::string key = SecondaryPrefix(generation, kernelExcess);
		key.append(EncodePosition(position));
		return key;
	}

	bool IsPrimaryKey(const std::string& key, const std::string& prefix)
	{
		return key.size() == prefix.size() + 8 + 33 &&
			key.compare(0, prefix.size(), prefix) == 0;
	}
}

std::unique_ptr<KernelIndexState> BlockDB::GetKernelIndexState() const
{
	return m_pRocksDB->Get<KernelIndexState>(KERNEL_POS_TABLE, rocksdb::Slice(KERNEL_POS_STATE_KEY));
}

void BlockDB::SetKernelIndexState(const KernelIndexState& state)
{
	m_pRocksDB->Put(
		KERNEL_POS_TABLE,
		DBEntry<KernelIndexState>(rocksdb::Slice(KERNEL_POS_STATE_KEY), state));
}

void BlockDB::AddKernelPosition(const uint64_t generation, const Commitment& kernelExcess, const OutputLocation& location)
{
	// One primary entry per kernel leaf (duplicates by excess stay distinct), plus
	// a secondary entry for excess -> position lookup. Keep the key strings alive
	// through the Put call (rocksdb::Slice is a view).
	const std::string primaryKey = PrimaryKey(generation, kernelExcess, location);
	const std::string secondaryKey = SecondaryKey(generation, kernelExcess, location);
	m_pRocksDB->Put(
		KERNEL_POS_TABLE,
		DBEntry<OutputLocation>(rocksdb::Slice(primaryKey), location));
	m_pRocksDB->Put(
		KERNEL_POS_TABLE,
		DBEntry<OutputLocation>(rocksdb::Slice(secondaryKey), location));
}

std::unique_ptr<OutputLocation> BlockDB::GetKernelPosition(const uint64_t generation, const Commitment& kernelExcess) const
{
	// The secondary keys are ordered by position; the last one is the latest
	// occurrence (Rust find_kernel returns the latest match in range).
	const std::string prefix = SecondaryPrefix(generation, kernelExcess);
	std::string lastKey;
	auto pIterator = m_pRocksDB->GetIterator(KERNEL_POS_TABLE);
	for (pIterator->Seek(prefix); pIterator->Valid(); pIterator->Next()) {
		const rocksdb::Slice key = pIterator->key();
		if (key.size() != prefix.size() + 8 || key.ToString().rfind(prefix, 0) != 0) {
			break;
		}

		lastKey = key.ToString();
	}

	if (lastKey.empty()) {
		return nullptr;
	}

	return m_pRocksDB->Get<OutputLocation>(KERNEL_POS_TABLE, rocksdb::Slice(lastKey));
}

void BlockDB::RemoveKernelPosition(
	const uint64_t generation,
	const Commitment& kernelExcess,
	const OutputLocation& location)
{
	// Exact-position removal only; another occurrence of the same excess (earlier
	// or later) must survive a partial reorg.
	const std::string primaryKey = PrimaryKey(generation, kernelExcess, location);
	const std::string secondaryKey = SecondaryKey(generation, kernelExcess, location);
	m_pRocksDB->Delete(KERNEL_POS_TABLE, rocksdb::Slice(primaryKey));
	m_pRocksDB->Delete(KERNEL_POS_TABLE, rocksdb::Slice(secondaryKey));
}

void BlockDB::ClearKernelPositions(const uint64_t generation)
{
	LOG_WARNING_F("Deleting kernel positions for generation {}.", generation);

	const std::string prefix = GenerationPrefix(generation);
	auto pIterator = m_pRocksDB->GetIterator(KERNEL_POS_TABLE);
	std::vector<std::string> keys;
	for (pIterator->Seek(prefix); pIterator->Valid(); pIterator->Next()) {
		const rocksdb::Slice key = pIterator->key();
		if (key.size() < prefix.size() || key.ToString().rfind(prefix, 0) != 0) {
			break;
		}

		keys.emplace_back(key.data(), key.size());
	}

	if (!keys.empty()) {
		m_pRocksDB->Delete(KERNEL_POS_TABLE, keys);
	}
}

std::pair<uint64_t, Hash> BlockDB::RemoveKernelPositionsFrom(
	const uint64_t generation,
	const uint64_t firstLeafIndex)
{
	// Keys of one generation are ordered by the 8-byte big-endian MMR position,
	// which increases monotonically with the kernel leaf index. Seek directly to
	// the first leaf that must be removed.
	const std::string primaryPrefix = PrimaryPrefix(generation);
	const std::string seekKey = primaryPrefix + EncodePosition(LeafIndex::At(firstLeafIndex).GetPosition());

	std::vector<std::string> keys;
	Hash removedChecksum;
	uint64_t removedCount = 0;
	auto pIterator = m_pRocksDB->GetIterator(KERNEL_POS_TABLE);
	for (pIterator->Seek(seekKey); pIterator->Valid(); pIterator->Next()) {
		const rocksdb::Slice key = pIterator->key();
		const std::string keyString = key.ToString();
		if (keyString.rfind(primaryPrefix, 0) != 0) {
			break;
		}
		if (!IsPrimaryKey(keyString, primaryPrefix)) {
			continue;
		}

		// key layout: primaryPrefix | position (8, big-endian) | excess (33)
		uint64_t position = 0;
		for (size_t i = 0; i < 8; ++i) {
			position = (position << 8) | static_cast<uint8_t>(keyString[primaryPrefix.size() + i]);
		}

		const std::vector<uint8_t> excessBytes(
			keyString.begin() + static_cast<std::ptrdiff_t>(primaryPrefix.size() + 8),
			keyString.end());
		const Commitment excess{ CBigInteger<33>(excessBytes) };

		const rocksdb::Slice value = pIterator->value();
		std::vector<uint8_t> data(value.data(), value.data() + value.size());
		ByteBuffer buffer(std::move(data));
		const OutputLocation location = OutputLocation::Deserialize(buffer);

		removedChecksum = KernelIndexXorHash(removedChecksum, excess, location);
		keys.emplace_back(keyString);
		keys.emplace_back(SecondaryKey(generation, excess, position));
		++removedCount;
	}

	if (!pIterator->status().ok()) {
		throw DATABASE_EXCEPTION("Kernel index rewind iterator failed");
	}

	if (!keys.empty()) {
		m_pRocksDB->Delete(KERNEL_POS_TABLE, keys);
	}

	return { removedCount, removedChecksum };
}

void BlockDB::ClearAllKernelPositions()
{
	LOG_WARNING("Deleting all kernel positions.");

	auto pIterator = m_pRocksDB->GetIterator(KERNEL_POS_TABLE);
	std::vector<std::string> keys;
	for (pIterator->Seek(KERNEL_POS_PREFIX); pIterator->Valid(); pIterator->Next()) {
		const rocksdb::Slice key = pIterator->key();
		if (key.size() < sizeof(KERNEL_POS_PREFIX) - 1 ||
			key.ToString().rfind(KERNEL_POS_PREFIX, 0) != 0) {
			break;
		}

		keys.emplace_back(key.data(), key.size());
	}

	if (!keys.empty()) {
		m_pRocksDB->Delete(KERNEL_POS_TABLE, keys);
	}

	m_pRocksDB->Delete(KERNEL_POS_TABLE, rocksdb::Slice(KERNEL_POS_STATE_KEY));
}

uint64_t BlockDB::GetKernelPositionCount(const uint64_t generation) const
{
	const std::string prefix = PrimaryPrefix(generation);
	uint64_t count = 0;
	auto pIterator = m_pRocksDB->GetIterator(KERNEL_POS_TABLE);
	for (pIterator->Seek(prefix); pIterator->Valid(); pIterator->Next()) {
		const rocksdb::Slice key = pIterator->key();
		if (key.size() < prefix.size() || key.ToString().rfind(prefix, 0) != 0) {
			break;
		}

		if (IsPrimaryKey(key.ToString(), prefix)) {
			count++;
		}
	}

	return count;
}

Hash BlockDB::GetKernelPositionChecksum(const uint64_t generation) const
{
	const std::string prefix = PrimaryPrefix(generation);
	Hash checksum;
	auto pIterator = m_pRocksDB->GetIterator(KERNEL_POS_TABLE);
	for (pIterator->Seek(prefix); pIterator->Valid(); pIterator->Next()) {
		const rocksdb::Slice key = pIterator->key();
		const std::string keyString = key.ToString();
		if (keyString.rfind(prefix, 0) != 0) {
			break;
		}

		if (!IsPrimaryKey(keyString, prefix)) {
			continue;
		}

		const auto pLocation = m_pRocksDB->Get<OutputLocation>(KERNEL_POS_TABLE, key);
		if (pLocation == nullptr) {
			// A missing value is corruption, not an empty index.
			return Hash(std::vector<uint8_t>(32, 0xFF));
		}

		const std::vector<uint8_t> excessBytes(
			keyString.begin() + static_cast<std::ptrdiff_t>(prefix.size() + 8),
			keyString.end());
		const Commitment excess{CBigInteger<33>(excessBytes)};
		checksum = KernelIndexXorHash(checksum, excess, *pLocation);
	}

	return checksum;
}

std::pair<uint64_t, Hash> BlockDB::GetKernelPositionDigest(const uint64_t generation) const
{
	const std::string prefix = PrimaryPrefix(generation);
	uint64_t count = 0;
	Hash checksum;
	auto pIterator = m_pRocksDB->GetIterator(KERNEL_POS_TABLE);
	for (pIterator->Seek(prefix); pIterator->Valid(); pIterator->Next()) {
		const std::string key = pIterator->key().ToString();
		if (key.rfind(prefix, 0) != 0) {
			break;
		}
		if (!IsPrimaryKey(key, prefix)) {
			continue;
		}
		const rocksdb::Slice value = pIterator->value();
		std::vector<uint8_t> data(value.data(), value.data() + value.size());
		ByteBuffer buffer(std::move(data));
		const OutputLocation location = OutputLocation::Deserialize(buffer);
		const std::vector<uint8_t> excessBytes(
			key.begin() + static_cast<std::ptrdiff_t>(prefix.size() + 8), key.end());
		const Commitment excess{CBigInteger<33>(excessBytes)};
		checksum = KernelIndexXorHash(checksum, excess, location);
		++count;
	}
	if (!pIterator->status().ok()) {
		throw DATABASE_EXCEPTION("Kernel index digest iterator failed");
	}
	return { count, checksum };
}

void BlockDB::OnInitWrite(const bool batch)
{
	m_pRocksDB->OnInitWrite(batch);
}

void BlockDB::OnEndWrite()
{
	m_pRocksDB->OnEndWrite();
}
