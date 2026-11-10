#include "HeaderStateCheckpoint.h"

#include <BlockChain/Chain.h>
#include <Consensus.h>
#include <Core/Config.h>
#include <Core/Global.h>
#include <Core/Models/BlockHeader.h>
#include <Core/Models/FullBlock.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Util/JsonUtil.h>
#include <Common/Util/HexUtil.h>
#include <Crypto/Hasher.h>
#include <Database/BlockDb.h>
#include "../PMMR/Common/MMRHashUtil.h"
#include <PMMR/Common/LeafIndex.h>
#include <PMMR/HeaderMMR.h>
#include <Common/Logger.h>
#include <Core/File/FileSync.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _MSC_VER
#pragma warning(disable: 4996) // getenv in the prototype-only environment handling
#endif

namespace
{
	constexpr uint64_t DEFAULT_SEGMENT_SIZE = 10000;
	constexpr uint32_t MAX_SEGMENTS = 1000000;

	Hash HashFromRaw(const uint8_t* data)
	{
		std::vector<unsigned char> bytes(data, data + 32);
		return Hash(std::move(bytes));
	}

	uint64_t GetEnvU64(const char* name, const uint64_t fallback)
	{
		const char* value = std::getenv(name);
		if (value == nullptr || *value == '\0') {
			return fallback;
		}

		try {
			return static_cast<uint64_t>(std::stoull(value));
		} catch (...) {
			return fallback;
		}
	}

	bool GetEnvBool(const char* name)
	{
		const char* value = std::getenv(name);
		if (value == nullptr) {
			return false;
		}

		const std::string text(value);
		return text == "1" || text == "true" || text == "TRUE" || text == "yes";
	}

	void AppendU8(std::vector<uint8_t>& out, const uint8_t value)
	{
		out.push_back(value);
	}

	void AppendU32(std::vector<uint8_t>& out, const uint32_t value)
	{
		for (int i = 0; i < 4; ++i) {
			out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
		}
	}

	void AppendU64(std::vector<uint8_t>& out, const uint64_t value)
	{
		for (int i = 0; i < 8; ++i) {
			out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
		}
	}

	void AppendHash(std::vector<uint8_t>& out, const Hash& hash)
	{
		const auto& data = hash.GetData();
		out.insert(out.end(), data.begin(), data.end());
	}

	void AppendBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes)
	{
		AppendU32(out, static_cast<uint32_t>(bytes.size()));
		out.insert(out.end(), bytes.begin(), bytes.end());
	}

	class CheckpointReader
	{
	public:
		CheckpointReader(const std::vector<uint8_t>& data)
			: m_data(data), m_offset(0) { }

		bool ReadU8(uint8_t& value)
		{
			if (!Has(1)) return false;
			value = m_data[m_offset++];
			return true;
		}

		bool ReadU32(uint32_t& value)
		{
			if (!Has(4)) return false;
			value = 0;
			for (int i = 0; i < 4; ++i) {
				value |= static_cast<uint32_t>(m_data[m_offset++]) << (8 * i);
			}
			return true;
		}

		bool ReadU64(uint64_t& value)
		{
			if (!Has(8)) return false;
			value = 0;
			for (int i = 0; i < 8; ++i) {
				value |= static_cast<uint64_t>(m_data[m_offset++]) << (8 * i);
			}
			return true;
		}

		bool ReadHash(Hash& hash)
		{
			if (!Has(32)) return false;
			std::vector<unsigned char> bytes(m_data.begin() + m_offset, m_data.begin() + m_offset + 32);
			m_offset += 32;
			hash = Hash(std::move(bytes));
			return true;
		}

		bool ReadBytes(std::vector<uint8_t>& bytes)
		{
			uint32_t size = 0;
			if (!ReadU32(size) || !Has(size)) return false;
			bytes.assign(m_data.begin() + m_offset, m_data.begin() + m_offset + size);
			m_offset += size;
			return true;
		}

		size_t Remaining() const
		{
			return m_data.size() - m_offset;
		}

	private:
		bool Has(const size_t size) const
		{
			return m_offset + size <= m_data.size();
		}

		const std::vector<uint8_t>& m_data;
		size_t m_offset;
	};

	bool ReadFile(const fs::path& path, std::vector<uint8_t>& out, std::string& error)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.is_open()) {
			error = "cannot open " + path.u8string();
			return false;
		}

		const std::streamoff size = file.tellg();
		if (size < 0) {
			error = "cannot determine size of " + path.u8string();
			return false;
		}

		file.seekg(0, std::ios::beg);
		out.resize(static_cast<size_t>(size));
		if (!out.empty() && !file.read(reinterpret_cast<char*>(out.data()), size)) {
			error = "cannot read " + path.u8string();
			return false;
		}

		return true;
	}

	uint64_t FileSize(const fs::path& path, std::string& error)
	{
		std::error_code ec;
		const uint64_t size = static_cast<uint64_t>(fs::file_size(path, ec));
		if (ec) {
			error = "cannot stat " + path.u8string();
			return 0;
		}

		return size;
	}

	Hash HashLeaf(const BlockHeader& header, const uint64_t position)
	{
		Serializer serializer;
		header.GetProofOfWork().SerializeCycle(serializer);
		return MMRHashUtil::HashLeafWithIndex(serializer.GetBytes(), position);
	}

	bool HashAt(const std::vector<uint8_t>& mmr, const uint64_t position, Hash& out)
	{
		const uint64_t offset = position * 32;
		if (offset + 32 > mmr.size()) {
			return false;
		}

		std::vector<unsigned char> bytes(mmr.begin() + offset, mmr.begin() + offset + 32);
		out = Hash(std::move(bytes));
		return true;
	}

	Hash ComputeRootFromPeaks(const std::vector<Hash>& peaks, const uint64_t size)
	{
		if (size == 0) {
			return ZERO_HASH;
		}

		Hash hash = ZERO_HASH;
		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			if (*iter != ZERO_HASH) {
				if (hash == ZERO_HASH) {
					hash = *iter;
				} else {
					hash = MMRHashUtil::HashParentWithIndex(*iter, hash, size);
				}
			}
		}
		return hash;
	}

	Hash FlipHash(const Hash& hash)
	{
		std::vector<unsigned char> bytes = hash.GetData();
		if (!bytes.empty()) {
			bytes[0] = static_cast<unsigned char>(bytes[0] ^ 0xFF);
		}
		return Hash(std::move(bytes));
	}
}

namespace HeaderCheckpoint
{
	EMode ModeFromEnv()
	{
		const char* value = std::getenv("GRINPP_HEADER_CKPT");
		if (value == nullptr || *value == '\0' || std::string(value) == "off") {
			return EMode::Off;
		}

		const std::string mode(value);
		if (mode == "full") return EMode::Full;
		if (mode == "hash") return EMode::HashOnly;
		if (mode == "checkpoint") return EMode::Write;
		if (mode == "fast") return EMode::Fast;

		LOG_WARNING_F("Unknown GRINPP_HEADER_CKPT mode '{}'; checkpoint prototype disabled.", mode);
		return EMode::Off;
	}

	bool ForceFullFromEnv()
	{
		return GetEnvBool("GRINPP_HEADER_CKPT_FORCE_FULL");
	}

	bool BootstrapFromEnv()
	{
		return GetEnvBool("GRINPP_HEADER_CKPT_BOOTSTRAP");
	}

	std::string TrustAnchorFromEnv()
	{
		const char* value = std::getenv("GRINPP_HEADER_CKPT_TRUST_ANCHOR");
		return value == nullptr ? std::string() : std::string(value);
	}

#ifdef _WIN32
	static uint32_t CountMaskBits(DWORD_PTR mask)
	{
		uint32_t count = 0;
		while (mask != 0)
		{
			count += static_cast<uint32_t>(mask & 1);
			mask >>= 1;
		}
		return count;
	}
#endif

	static uint32_t ProcessAvailableCores()
	{
#ifdef _WIN32
		DWORD_PTR processMask = 0;
		DWORD_PTR systemMask = 0;
		if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
		{
			const uint32_t count = CountMaskBits(processMask);
			if (count > 0) return count;
		}
#endif
		const unsigned hw = std::thread::hardware_concurrency();
		return hw == 0 ? 1 : hw;
	}

	static uint64_t AvailableMemoryMb()
	{
#ifdef _WIN32
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (GlobalMemoryStatusEx(&status))
		{
			return static_cast<uint64_t>(status.ullAvailPhys / (1024ULL * 1024ULL));
		}
#endif
		return 0;
	}

	bool ExtensionEntryCountAllowed(uint64_t storedEntries, uint64_t currentEntries) noexcept
	{
		return currentEntries >= storedEntries && currentEntries - storedEntries <= 10000;
	}

	bool ExtensionMmrNodeMatches(
		uint64_t expectedPosition,
		uint64_t actualPosition,
		const Hash& expected,
		const Hash& actual
	) noexcept
	{
		return expectedPosition == actualPosition && expected == actual;
	}

	uint32_t ChooseThreadCount(uint32_t availableCores, uint64_t availableMemoryMb)
	{
		uint32_t cores = (std::max)((uint32_t)1, availableCores);
		// Leave one core for the node/P2P/UI when the machine has more than two.
		uint32_t byCores = cores <= 2 ? cores : cores - 1;
		byCores = (std::min)(byCores, (uint32_t)8);

		uint32_t byMemory = 8;
		if (availableMemoryMb > 0)
		{
			// Keep a 1 GiB reserve for the node and allow ~512 MiB per worker.
			const uint64_t usable = availableMemoryMb > 1024 ? availableMemoryMb - 1024 : 0;
			byMemory = static_cast<uint32_t>((std::min)((uint64_t)8, usable / 512));
			byMemory = (std::max)((uint32_t)1, byMemory);
		}

		return (std::max)((uint32_t)1, (std::min)(byCores, byMemory));
	}

	uint32_t ResolveThreadCount()
	{
		const char* value = std::getenv("GRINPP_HEADER_CKPT_THREADS");
		if (value == nullptr || *value == '\0' ||
			std::string(value) == "auto" || std::string(value) == "0")
		{
			return ChooseThreadCount(ProcessAvailableCores(), AvailableMemoryMb());
		}

		try
		{
			const unsigned long requested = std::stoul(value);
			if (requested >= 1)
			{
				return (uint32_t)(std::min)((unsigned long)32, requested);
			}
		}
		catch (...)
		{
		}
		return ChooseThreadCount(ProcessAvailableCores(), AvailableMemoryMb());
	}


	static bool ParseHashField(const Json::Value& root, const char* key, Hash& out, std::string& error)
	{
		const Json::Value value = root.get(key, Json::nullValue);
		if (!value.isString())
		{
			error = std::string("anchor manifest field '") + key + "' is missing or not a string";
			return false;
		}
		try
		{
			const std::vector<unsigned char> bytes = HexUtil::FromHex(value.asString());
			if (bytes.size() != 32)
			{
				error = std::string("anchor manifest field '") + key + "' must be 32 bytes hex";
				return false;
			}
			out = Hash(std::vector<unsigned char>(bytes));
			return true;
		}
		catch (const std::exception&)
		{
			error = std::string("anchor manifest field '") + key + "' is not valid hex";
			return false;
		}
	}

	bool ParseAnchorManifest(
		const fs::path& path,
		HeaderCheckpointAnchor& out,
		std::string& error)
	{
		out = HeaderCheckpointAnchor{};
		std::vector<uint8_t> bytes;
		if (!ReadFile(path, bytes, error))
		{
			return false;
		}

		Json::Value root;
		try
		{
			root = JsonUtil::Parse(bytes);
		}
		catch (const std::exception& e)
		{
			error = std::string("cannot parse anchor manifest: ") + e.what();
			return false;
		}
		if (!root.isObject())
		{
			error = "anchor manifest must be a JSON object";
			return false;
		}

		// A signature field is not a proof here: this build does not verify it.
		// Accepting it silently would turn an unverified blob into apparent trust.
		if (root.isMember("signature"))
		{
			error = "anchor manifest signature is present, but signature verification is not implemented";
			return false;
		}

		const Json::Value network = root.get("network", Json::nullValue);
		if (!network.isString() || network.asString().empty())
		{
			error = "anchor manifest network is missing";
			return false;
		}
		out.Network = network.asString();

		const Json::Value source = root.get("source", Json::nullValue);
		if (!source.isString() || source.asString().empty())
		{
			error = "anchor manifest source/provenance is missing";
			return false;
		}
		out.Source = source.asString();

		if (!ParseHashField(root, "genesis", out.GenesisHash, error)) return false;
		if (!ParseHashField(root, "tip", out.CandidateTip, error)) return false;
		if (!ParseHashField(root, "aggregate", out.Aggregate, error)) return false;

		const Json::Value candidate = root.get("candidate_entries", Json::nullValue);
		const Json::Value confirmed = root.get("confirmed_entries", Json::nullValue);
		const Json::Value mmrNodes = root.get("mmr_nodes", Json::nullValue);
		if (!candidate.isUInt64() || !confirmed.isUInt64() || !mmrNodes.isUInt64())
		{
			error = "anchor manifest counts must be unsigned integers";
			return false;
		}
		out.CandidateEntries = candidate.asUInt64();
		out.ConfirmedEntries = confirmed.asUInt64();
		out.MmrNodes = mmrNodes.asUInt64();

		const Json::Value height = root.get("candidate_height", Json::nullValue);
		if (!height.isNull())
		{
			if (!height.isUInt64() || height.asUInt64() + 1 != out.CandidateEntries)
			{
				error = "anchor manifest candidate_height does not match candidate_entries";
				return false;
			}
		}

		if (out.CandidateEntries == 0 || out.ConfirmedEntries > out.CandidateEntries)
		{
			error = "anchor manifest counts are inconsistent";
			return false;
		}

		return true;
	}

	bool AnchorMatches(
		const HeaderCheckpointAnchor& anchor,
		const HeaderCheckpointData& data,
		const std::string& expectedNetwork,
		std::string& error)
	{
		if (anchor.Network != expectedNetwork)
		{
			error = "anchor network mismatch: manifest=" + anchor.Network + " node=" + expectedNetwork;
			return false;
		}
		if (anchor.GenesisHash != data.GenesisHash)
		{
			error = "anchor genesis mismatch";
			return false;
		}
		if (anchor.CandidateEntries != data.CandidateEntries ||
			anchor.ConfirmedEntries != data.ConfirmedEntries ||
			anchor.MmrNodes != data.MmrNodes)
		{
			error = "anchor counts mismatch";
			return false;
		}
		if (anchor.CandidateTip != data.CandidateTip)
		{
			error = "anchor candidate tip mismatch";
			return false;
		}
		if (anchor.Aggregate != data.Aggregate)
		{
			error = "anchor aggregate mismatch";
			return false;
		}
		return true;
	}

	fs::path Path(const Config& config)
	{
		return config.GetChainPath() / "header.ckpt";
	}

	static uint64_t SegmentSizeFromEnv()
	{
		const uint64_t size = GetEnvU64("GRINPP_HEADER_CKPT_SEGMENT", DEFAULT_SEGMENT_SIZE);
		return (std::max)((uint64_t)100, (std::min)(size, (uint64_t)1000000));
	}

	bool ReadMetadata(
		const Config& config,
		const IBlockDB& blockDB,
		const IHeaderMMR& headerMMR,
		const Chain& candidateChain,
		const Chain& confirmedChain,
		HeaderCheckpointData& out,
		std::string& error)
	{
		out = HeaderCheckpointData{};
		out.CandidateEntries = candidateChain.GetHeight() + 1;
		out.ConfirmedEntries = confirmedChain.GetHeight() + 1;
		out.MmrLeaves = headerMMR.GetLeafCount();
		out.MmrNodes = headerMMR.GetNodeCount();
		out.CandidateFileSize = FileSize(config.GetChainPath() / "candidate.chain", error);
		if (!error.empty()) return false;
		out.ConfirmedFileSize = FileSize(config.GetChainPath() / "confirmed.chain", error);
		if (!error.empty()) return false;
		out.MmrFileSize = FileSize(config.GetChainPath() / "header_mmr.bin", error);
		if (!error.empty()) return false;
		out.DatabaseVersion = blockDB.GetVersion();
		out.SegmentSize = SegmentSizeFromEnv();
		out.MagicBytes = config.GetMagicBytes();
		out.GenesisHash = Global::GetGenesisBlock().GetHash();
		out.CandidateTip = candidateChain.GetTipHash();
		out.ConfirmedTip = confirmedChain.GetTipHash();
		return true;
	}

	bool Compute(
		const Config& config,
		const IBlockDB& blockDB,
		const IHeaderMMR& headerMMR,
		const Chain& candidateChain,
		const Chain& confirmedChain,
		const bool validateHeaders,
		const bool verifyMmrInternal,
		HeaderCheckpointData& out,
		HeaderCheckpointMetrics& metrics,
		std::string& error)
	{
		const auto start = std::chrono::steady_clock::now();
		const std::clock_t cpuStart = std::clock();
		const uint64_t segmentSize = SegmentSizeFromEnv();

		out = HeaderCheckpointData{};
		metrics = HeaderCheckpointMetrics{};
		metrics.SegmentSize = segmentSize;

		out.CandidateEntries = candidateChain.GetHeight() + 1;
		out.ConfirmedEntries = confirmedChain.GetHeight() + 1;
		out.MmrLeaves = headerMMR.GetLeafCount();
		out.MmrNodes = headerMMR.GetNodeCount();
		out.DatabaseVersion = blockDB.GetVersion();
		out.SegmentSize = segmentSize;
		out.MagicBytes = config.GetMagicBytes();
		out.GenesisHash = Global::GetGenesisBlock().GetHash();
		out.CandidateTip = candidateChain.GetTipHash();
		out.ConfirmedTip = confirmedChain.GetTipHash();

		out.CandidateFileSize = FileSize(config.GetChainPath() / "candidate.chain", error);
		if (!error.empty()) return false;
		out.ConfirmedFileSize = FileSize(config.GetChainPath() / "confirmed.chain", error);
		if (!error.empty()) return false;
		out.MmrFileSize = FileSize(config.GetChainPath() / "header_mmr.bin", error);
		if (!error.empty()) return false;

		metrics.CandidateEntries = out.CandidateEntries;
		metrics.MmrNodes = out.MmrNodes;
		metrics.BytesRead += 32 * out.CandidateEntries;
		metrics.BytesRead += 32 * out.ConfirmedEntries;
		metrics.BytesRead += out.MmrFileSize;

		if (out.CandidateFileSize != out.CandidateEntries * 32) {
			error = "candidate.chain size does not match the chain height";
			return false;
		}
		if (out.ConfirmedFileSize != out.ConfirmedEntries * 32) {
			error = "confirmed.chain size does not match the confirmed height";
			return false;
		}
		if (out.MmrFileSize != out.MmrNodes * 32) {
			error = "header_mmr.bin size does not match the MMR node count";
			return false;
		}

		if (validateHeaders && (out.MmrLeaves != out.CandidateEntries)) {
			error = "header MMR leaf count does not match the candidate chain";
			return false;
		}
		if (validateHeaders && (out.ConfirmedEntries > out.CandidateEntries)) {
			error = "confirmed chain is ahead of the candidate chain";
			return false;
		}

		std::vector<uint8_t> mmr;
		if (!ReadFile(config.GetChainPath() / "header_mmr.bin", mmr, error)) {
			return false;
		}
		if (mmr.size() != out.MmrFileSize) {
			error = "header_mmr.bin changed while reading";
			return false;
		}

		if (out.CandidateEntries == 0) {
			out.MmrRoot = Hash();
		} else {
			out.MmrRoot = headerMMR.Root(out.CandidateEntries - 1);
		}

		std::ifstream candidateFile(config.GetChainPath() / "candidate.chain", std::ios::binary);
		std::ifstream confirmedFile(config.GetChainPath() / "confirmed.chain", std::ios::binary);
		if (!candidateFile.is_open() || !confirmedFile.is_open()) {
			error = "cannot open chain index data files";
			return false;
		}

		const uint64_t configuredThreads = ResolveThreadCount();
		// Extension verifies all MMR nodes; cap its parallel preparation at four.
		const uint64_t threadCount = verifyMmrInternal ? (AvailableMemoryMb() == 0 ? uint64_t{1} : (std::min)(configuredThreads, uint64_t{4})) : configuredThreads;
		const bool parallelHeaders =
			!validateHeaders && threadCount > 1 && out.CandidateEntries > 0;
		if (parallelHeaders) {
			std::vector<uint8_t> candidateBytes;
			if (!ReadFile(config.GetChainPath() / "candidate.chain", candidateBytes, error)) {
				return false;
			}
			if (candidateBytes.size() != out.CandidateFileSize) {
				error = "candidate.chain changed while reading";
				return false;
			}

			const uint64_t segmentCount = (out.CandidateEntries + segmentSize - 1) / segmentSize;
			out.HeaderSegments.assign(static_cast<size_t>(segmentCount), Hash());
			std::vector<Hash> expectedLeaves;
			if (verifyMmrInternal) { try { expectedLeaves.resize(static_cast<size_t>(out.CandidateEntries)); } catch (const std::exception&) { error = "cannot allocate header MMR verification buffers"; return false; } }

			std::atomic<uint64_t> nextSegment{ 0 };
			std::atomic<uint64_t> headerBytesRead{ 0 };
			std::atomic<uint64_t> dbNs{ 0 };
			std::atomic<uint64_t> serializeNs{ 0 };
			std::atomic<uint64_t> hashNs{ 0 };
			std::atomic<bool> failed{ false };
			std::mutex errorMutex;
			std::string workerError;

			auto worker = [&]()
			{
				try {
				while (!failed.load()) {
					const uint64_t segmentIndex = nextSegment.fetch_add(1);
					if (segmentIndex >= segmentCount) {
						return;
					}

					const uint64_t start = segmentIndex * segmentSize;
					const uint64_t end = (std::min)(start + segmentSize, out.CandidateEntries);
					std::vector<uint8_t> segment;
					segment.reserve(static_cast<size_t>(end - start) * 220);

					for (uint64_t height = start; height < end; ++height) {
						const Hash chainHash = HashFromRaw(candidateBytes.data() + height * 32);
						const auto dbStart = std::chrono::steady_clock::now();
						const BlockHeaderPtr pHeader = blockDB.GetBlockHeader(chainHash);
						dbNs.fetch_add(static_cast<uint64_t>(
							std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now() - dbStart).count()));
						if (pHeader == nullptr) {
							std::lock_guard<std::mutex> lock(errorMutex);
							if (!failed.exchange(true)) {
								workerError = "block header is missing at height " + std::to_string(height);
							}
							return;
						}

						if (verifyMmrInternal) {
							const auto pIndex = candidateChain.GetByHeight(height);
							if (pIndex == nullptr || pIndex->GetHash() != chainHash) {
								std::lock_guard<std::mutex> lock(errorMutex);
								if (!failed.exchange(true)) workerError = "candidate chain entry mismatch at height " + std::to_string(height);
								return;
							}
							expectedLeaves[static_cast<size_t>(height)] = HashLeaf(*pHeader, LeafIndex::At(height).GetPosition());
						}
						Serializer serializer;
						const auto serializeStart = std::chrono::steady_clock::now();
						pHeader->Serialize(serializer);
						const std::vector<uint8_t> headerBytes = serializer.GetBytes();
						serializeNs.fetch_add(static_cast<uint64_t>(
							std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now() - serializeStart).count()));
						segment.insert(segment.end(), candidateBytes.begin() + height * 32, candidateBytes.begin() + height * 32 + 32);
						AppendU32(segment, static_cast<uint32_t>(headerBytes.size()));
						segment.insert(segment.end(), headerBytes.begin(), headerBytes.end());
						headerBytesRead.fetch_add(4 + headerBytes.size());
					}

					const auto hashStart = std::chrono::steady_clock::now();
					out.HeaderSegments[static_cast<size_t>(segmentIndex)] = Hasher::SHA256(segment);
					if (segmentIndex == 0 && std::getenv("GRINPP_DIAG_REFRESH_MARKER") != nullptr) { LOG_INFO("event=diagnostic_hash_inside first_segment_complete=1"); }
					hashNs.fetch_add(static_cast<uint64_t>(
						std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::steady_clock::now() - hashStart).count()));
				}
				} catch (const std::exception& e) {
					std::lock_guard<std::mutex> lock(errorMutex);
					if (!failed.exchange(true)) workerError = std::string("header worker failed: ") + e.what();
				} catch (...) {
					std::lock_guard<std::mutex> lock(errorMutex);
					if (!failed.exchange(true)) workerError = "header worker failed";
				}
			};

			std::vector<std::thread> workers;
			workers.reserve(static_cast<size_t>(threadCount));
			try {
				for (uint64_t i = 0; i < threadCount; ++i) workers.emplace_back(worker);
			} catch (...) {
				failed.store(true);
				for (std::thread& thread : workers) if (thread.joinable()) thread.join();
				error = "could not start header workers";
				return false;
			}
			for (std::thread& thread : workers) {
				thread.join();
			}

			if (failed.load()) {
				error = workerError;
				return false;
			}
			// Check every MMR leaf and parent in its original order. Parallel workers
			// only prepare expected leaves and segment digests; no checks are skipped.
			if (verifyMmrInternal) {
				std::vector<Hash> peaks;
				std::vector<uint64_t> peakHeights;
				uint64_t computedNodes = 0;
				for (uint64_t height = 0; height < out.CandidateEntries; ++height) {
					const uint64_t position = LeafIndex::At(height).GetPosition();
					Hash node = expectedLeaves[static_cast<size_t>(height)];
					Hash storedLeaf;
					if (!HashAt(mmr, position, storedLeaf) ||
						!ExtensionMmrNodeMatches(computedNodes, position, node, storedLeaf)) {
						error = "header MMR leaf mismatch at height " + std::to_string(height);
						return false;
					}
					++computedNodes;
					uint64_t nodePosition = position;
					uint64_t nodeHeight = 0;
					while (!peakHeights.empty() && peakHeights.back() == nodeHeight) {
						const Hash left = peaks.back(); peaks.pop_back(); peakHeights.pop_back();
						const uint64_t parentPosition = nodePosition + 1;
						const Hash parent = MMRHashUtil::HashParentWithIndex(left, node, parentPosition);
						Hash storedParent;
						if (!HashAt(mmr, parentPosition, storedParent) ||
							!ExtensionMmrNodeMatches(computedNodes, parentPosition, parent, storedParent)) {
							error = "header MMR internal node mismatch at position " + std::to_string(parentPosition);
							return false;
						}
						++computedNodes;
						node = parent; nodePosition = parentPosition; ++nodeHeight;
					}
					peaks.push_back(node); peakHeights.push_back(nodeHeight);
				}
				if (computedNodes != out.MmrNodes) {
					error = "header MMR internal node count mismatch";
					return false;
				}
			}
			metrics.BytesRead += headerBytesRead.load();
			metrics.DbReadMs = dbNs.load() / 1000000;
			metrics.SerializeMs = serializeNs.load() / 1000000;
			metrics.HashMs = hashNs.load() / 1000000;
			metrics.Threads = static_cast<uint32_t>(threadCount);
		}

		std::vector<uint8_t> confirmedSegment;
		confirmedSegment.reserve(static_cast<size_t>(segmentSize) * 32);

		if (!parallelHeaders) {
		std::vector<uint8_t> headerSegment;
		headerSegment.reserve(static_cast<size_t>(segmentSize) * 220);

		std::vector<Hash> peaks;
		std::vector<uint64_t> peakHeights;
		peaks.reserve(64);
		peakHeights.reserve(64);
		uint64_t computedNodes = 0;
		uint64_t dbNs = 0;
		uint64_t serializeNs = 0;
		uint64_t hashNs = 0;

		Hash previousHash;
		for (uint64_t height = 0; height < out.CandidateEntries; ++height) {
			std::array<uint8_t, 32> chainBytes{};
			candidateFile.read(reinterpret_cast<char*>(chainBytes.data()), chainBytes.size());
			if (candidateFile.gcount() != static_cast<std::streamsize>(chainBytes.size())) {
				error = "candidate.chain is truncated at height " + std::to_string(height);
				return false;
			}

			std::vector<unsigned char> rawHash(chainBytes.begin(), chainBytes.end());
			const Hash chainHash(std::move(rawHash));

			const auto pIndex = candidateChain.GetByHeight(height);
			if (pIndex == nullptr) {
				error = "candidate chain is missing an entry at height " + std::to_string(height);
				return false;
			}
			if (pIndex->GetHash() != chainHash) {
				error = "candidate.chain entry does not match the in-memory chain at height " + std::to_string(height);
				return false;
			}

			const auto dbStart = std::chrono::steady_clock::now();
			const auto pHeader = blockDB.GetBlockHeader(chainHash);
			dbNs += static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - dbStart).count());
			if (pHeader == nullptr) {
				error = "block header is missing at height " + std::to_string(height);
				return false;
			}

			if (validateHeaders) {
				if (pHeader->GetHeight() != height || pHeader->GetHash() != chainHash) {
					error = "block header/hash mismatch at height " + std::to_string(height);
					return false;
				}
				if (height > 0 && pHeader->GetPreviousHash() != previousHash) {
					error = "candidate chain link mismatch at height " + std::to_string(height);
					return false;
				}
			}

			const uint64_t position = LeafIndex::At(height).GetPosition();
			if (validateHeaders || verifyMmrInternal) {
				const Hash expectedLeaf = HashLeaf(*pHeader, position);
				Hash storedLeaf;
				if (!HashAt(mmr, position, storedLeaf)) {
					error = "header MMR leaf is outside the stored file at height " + std::to_string(height);
					return false;
				}
				if (storedLeaf != expectedLeaf) {
					error = "header MMR hash mismatch at height " + std::to_string(height);
					return false;
				}

				if (verifyMmrInternal) {
					if (position != computedNodes) {
						error = "header MMR node sequence mismatch at height " + std::to_string(height);
						return false;
					}
					++computedNodes;

					Hash node = expectedLeaf;
					uint64_t nodePosition = position;
					uint64_t nodeHeight = 0;
					while (!peakHeights.empty() && peakHeights.back() == nodeHeight) {
						const Hash left = peaks.back();
						peaks.pop_back();
						peakHeights.pop_back();

						const uint64_t parentPosition = nodePosition + 1;
						const Hash parent = MMRHashUtil::HashParentWithIndex(left, node, parentPosition);
						Hash storedParent;
						if (!HashAt(mmr, parentPosition, storedParent) || storedParent != parent) {
							error = "header MMR internal node mismatch at position " + std::to_string(parentPosition);
							return false;
						}
						if (parentPosition != computedNodes) {
							error = "header MMR parent node sequence mismatch";
							return false;
						}
						++computedNodes;

						node = parent;
						nodePosition = parentPosition;
						++nodeHeight;
					}

					peaks.push_back(node);
					peakHeights.push_back(nodeHeight);
				}
			}

			Serializer serializer;
			const auto serializeStart = std::chrono::steady_clock::now();
			pHeader->Serialize(serializer);
			const std::vector<uint8_t> headerBytes = serializer.GetBytes();
			serializeNs += static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - serializeStart).count());
			headerSegment.insert(headerSegment.end(), chainBytes.begin(), chainBytes.end());
			AppendU32(headerSegment, static_cast<uint32_t>(headerBytes.size()));
			headerSegment.insert(headerSegment.end(), headerBytes.begin(), headerBytes.end());
			metrics.BytesRead += 4 + headerBytes.size();

			if ((height + 1) % segmentSize == 0 || (height + 1) == out.CandidateEntries) {
				const auto hashStart = std::chrono::steady_clock::now();
				out.HeaderSegments.push_back(Hasher::SHA256(headerSegment));
				if (height + 1 == segmentSize && std::getenv("GRINPP_DIAG_REFRESH_MARKER") != nullptr) { LOG_INFO("event=diagnostic_hash_inside first_segment_complete=1"); }
				hashNs += static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - hashStart).count());
				headerSegment.clear();
			}

			previousHash = chainHash;
		}

		if (candidateFile.peek() != std::char_traits<char>::eof()) {
			error = "candidate.chain contains trailing bytes";
			return false;
		}

		if (verifyMmrInternal && computedNodes != out.MmrNodes) {
			error = "header MMR node count mismatch after recomputation";
			return false;
		}
		metrics.DbReadMs = dbNs / 1000000;
		metrics.SerializeMs = serializeNs / 1000000;
		metrics.HashMs = hashNs / 1000000;
		metrics.Threads = 1;
		}

		for (uint64_t height = 0; height < out.ConfirmedEntries; ++height) {
			std::array<uint8_t, 32> chainBytes{};
			confirmedFile.read(reinterpret_cast<char*>(chainBytes.data()), chainBytes.size());
			if (confirmedFile.gcount() != static_cast<std::streamsize>(chainBytes.size())) {
				error = "confirmed.chain is truncated at height " + std::to_string(height);
				return false;
			}

			std::vector<unsigned char> rawHash(chainBytes.begin(), chainBytes.end());
			const Hash chainHash(std::move(rawHash));

			const auto pIndex = confirmedChain.GetByHeight(height);
			if (pIndex == nullptr || pIndex->GetHash() != chainHash) {
				error = "confirmed.chain entry does not match the in-memory chain at height " + std::to_string(height);
				return false;
			}

			if (validateHeaders && height < out.CandidateEntries) {
				const auto pCandidate = candidateChain.GetByHeight(height);
				if (pCandidate == nullptr || pCandidate->GetHash() != chainHash) {
					error = "confirmed chain is not a prefix of the candidate chain at height " + std::to_string(height);
					return false;
				}
			}

			confirmedSegment.insert(confirmedSegment.end(), chainBytes.begin(), chainBytes.end());
			if ((height + 1) % segmentSize == 0 || (height + 1) == out.ConfirmedEntries) {
				out.ConfirmedSegments.push_back(Hasher::SHA256(confirmedSegment));
				confirmedSegment.clear();
			}
		}

		if (confirmedFile.peek() != std::char_traits<char>::eof()) {
			error = "confirmed.chain contains trailing bytes";
			return false;
		}

		for (uint64_t offset = 0; offset < out.MmrNodes; offset += segmentSize) {
			const uint64_t count = (std::min)(segmentSize, out.MmrNodes - offset);
			out.MmrSegments.push_back(Hasher::SHA256(mmr.data() + offset * 32, static_cast<size_t>(count * 32)));
		}

		std::vector<uint8_t> aggregate;
		AppendU32(aggregate, HeaderCheckpointData::CURRENT_VERSION);
		AppendU64(aggregate, out.CandidateEntries);
		AppendU64(aggregate, out.ConfirmedEntries);
		AppendU64(aggregate, out.MmrLeaves);
		AppendU64(aggregate, out.MmrNodes);
		AppendU64(aggregate, out.MmrFileSize);
		AppendU64(aggregate, out.CandidateFileSize);
		AppendU64(aggregate, out.ConfirmedFileSize);
		AppendU8(aggregate, out.DatabaseVersion);
		AppendU64(aggregate, out.SegmentSize);
		AppendBytes(aggregate, out.MagicBytes);
		AppendHash(aggregate, out.GenesisHash);
		AppendHash(aggregate, out.CandidateTip);
		AppendHash(aggregate, out.ConfirmedTip);
		AppendHash(aggregate, out.MmrRoot);
		AppendU32(aggregate, static_cast<uint32_t>(out.HeaderSegments.size()));
		for (const Hash& hash : out.HeaderSegments) AppendHash(aggregate, hash);
		AppendU32(aggregate, static_cast<uint32_t>(out.MmrSegments.size()));
		for (const Hash& hash : out.MmrSegments) AppendHash(aggregate, hash);
		AppendU32(aggregate, static_cast<uint32_t>(out.ConfirmedSegments.size()));
		for (const Hash& hash : out.ConfirmedSegments) AppendHash(aggregate, hash);

		out.Aggregate = Hasher::SHA256(aggregate);
		metrics.BytesRead += aggregate.size();

		const auto end = std::chrono::steady_clock::now();
		const std::clock_t cpuEnd = std::clock();
		metrics.DurationMs = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
		metrics.CpuMs = static_cast<uint64_t>(
			(cpuEnd - cpuStart) * 1000 / CLOCKS_PER_SEC);
		return true;
	}

	bool VerifyFull(
		const Config& config,
		const IBlockDB& blockDB,
		const IHeaderMMR& headerMMR,
		const Chain& candidateChain,
		const Chain& confirmedChain,
		HeaderCheckpointMetrics& metrics,
		std::string& error)
	{
		const auto start = std::chrono::steady_clock::now();
		metrics = HeaderCheckpointMetrics{};

		const uint64_t candidateEntries = candidateChain.GetHeight() + 1;
		const uint64_t confirmedEntries = confirmedChain.GetHeight() + 1;
		const uint64_t mmrLeaves = headerMMR.GetLeafCount();
		const uint64_t mmrNodes = headerMMR.GetNodeCount();

		metrics.CandidateEntries = candidateEntries;
		metrics.MmrNodes = mmrNodes;

		const uint64_t candidateFileSize = FileSize(config.GetChainPath() / "candidate.chain", error);
		if (!error.empty()) return false;
		const uint64_t confirmedFileSize = FileSize(config.GetChainPath() / "confirmed.chain", error);
		if (!error.empty()) return false;
		const uint64_t mmrFileSize = FileSize(config.GetChainPath() / "header_mmr.bin", error);
		if (!error.empty()) return false;

		if (candidateFileSize != candidateEntries * 32) {
			error = "candidate.chain size does not match the chain height";
			return false;
		}
		if (confirmedFileSize != confirmedEntries * 32) {
			error = "confirmed.chain size does not match the confirmed height";
			return false;
		}
		if (mmrFileSize != mmrNodes * 32) {
			error = "header_mmr.bin size does not match the MMR node count";
			return false;
		}
		if (mmrLeaves != candidateEntries) {
			error = "header MMR leaf count does not match the candidate chain";
			return false;
		}
		if (confirmedEntries > candidateEntries) {
			error = "confirmed chain is ahead of the candidate chain";
			return false;
		}

		metrics.BytesRead += 32 * candidateEntries;
		metrics.BytesRead += 32 * confirmedEntries;
		metrics.BytesRead += mmrFileSize;

		std::vector<uint8_t> mmr;
		if (!ReadFile(config.GetChainPath() / "header_mmr.bin", mmr, error)) {
			return false;
		}
		if (mmr.size() != mmrFileSize) {
			error = "header_mmr.bin changed while reading";
			return false;
		}

		std::ifstream candidateFile(config.GetChainPath() / "candidate.chain", std::ios::binary);
		std::ifstream confirmedFile(config.GetChainPath() / "confirmed.chain", std::ios::binary);
		if (!candidateFile.is_open() || !confirmedFile.is_open()) {
			error = "cannot open chain index data files";
			return false;
		}

		std::vector<Hash> peaks;
		std::vector<uint64_t> peakHeights;
		peaks.reserve(64);
		peakHeights.reserve(64);
		uint64_t computedNodes = 0;
		Hash previousHash;
		BlockHeaderPtr previousHeader;

		for (uint64_t height = 0; height < candidateEntries; ++height) {
			std::array<uint8_t, 32> chainBytes{};
			candidateFile.read(reinterpret_cast<char*>(chainBytes.data()), chainBytes.size());
			if (candidateFile.gcount() != static_cast<std::streamsize>(chainBytes.size())) {
				error = "candidate.chain is truncated at height " + std::to_string(height);
				return false;
			}

			const Hash chainHash = HashFromRaw(chainBytes.data());
			const auto pIndex = candidateChain.GetByHeight(height);
			if (pIndex == nullptr || pIndex->GetHash() != chainHash) {
				error = "candidate.chain entry does not match the in-memory chain at height " + std::to_string(height);
				return false;
			}

			const BlockHeaderPtr pHeader = blockDB.GetBlockHeader(chainHash);
			if (pHeader == nullptr) {
				error = "block header is missing at height " + std::to_string(height);
				return false;
			}
			if (pHeader->GetHeight() != height || pHeader->GetHash() != chainHash) {
				error = "block header/hash mismatch at height " + std::to_string(height);
				return false;
			}
			if (height > 0 && pHeader->GetPreviousHash() != previousHash) {
				error = "candidate chain link mismatch at height " + std::to_string(height);
				return false;
			}

			if (pHeader->GetVersion() != Consensus::GetHeaderVersion(height)) {
				error = "header version mismatch at height " + std::to_string(height);
				return false;
			}
			if (pHeader->GetTimestamp() > Consensus::GetMaxBlockTime(std::chrono::system_clock::now())) {
				error = "header timestamp is too far in the future at height " + std::to_string(height);
				return false;
			}
			if (height > 0) {
				if (previousHeader == nullptr) {
					error = "previous header is unavailable at height " + std::to_string(height);
					return false;
				}
				if (pHeader->GetTimestamp() <= previousHeader->GetTimestamp()) {
					error = "header timestamp is not after its predecessor at height " + std::to_string(height);
					return false;
				}
				if (pHeader->GetTotalDifficulty() <= previousHeader->GetTotalDifficulty()) {
					error = "header total difficulty did not increase at height " + std::to_string(height);
					return false;
				}

				const Hash expectedPreviousRoot = ComputeRootFromPeaks(peaks, LeafIndex::At(height).GetPosition());
				if (pHeader->GetPreviousRoot() != expectedPreviousRoot) {
					error = "header previous MMR root mismatch at height " + std::to_string(height);
					return false;
				}
			}

			const uint64_t position = LeafIndex::At(height).GetPosition();
			const Hash expectedLeaf = HashLeaf(*pHeader, position);
			Hash storedLeaf;
			if (!HashAt(mmr, position, storedLeaf) || storedLeaf != expectedLeaf) {
				error = "header MMR hash mismatch at height " + std::to_string(height);
				return false;
			}
			if (position != computedNodes) {
				error = "header MMR node sequence mismatch at height " + std::to_string(height);
				return false;
			}
			++computedNodes;

			Hash node = expectedLeaf;
			uint64_t nodePosition = position;
			uint64_t nodeHeight = 0;
			while (!peakHeights.empty() && peakHeights.back() == nodeHeight) {
				const Hash left = peaks.back();
				peaks.pop_back();
				peakHeights.pop_back();

				const uint64_t parentPosition = nodePosition + 1;
				const Hash parent = MMRHashUtil::HashParentWithIndex(left, node, parentPosition);
				Hash storedParent;
				if (!HashAt(mmr, parentPosition, storedParent) || storedParent != parent) {
					error = "header MMR internal node mismatch at position " + std::to_string(parentPosition);
					return false;
				}
				if (parentPosition != computedNodes) {
					error = "header MMR parent node sequence mismatch";
					return false;
				}
				++computedNodes;

				node = parent;
				nodePosition = parentPosition;
				++nodeHeight;
			}
			peaks.push_back(node);
			peakHeights.push_back(nodeHeight);

			previousHash = chainHash;
			previousHeader = pHeader;
		}

		if (candidateFile.peek() != std::char_traits<char>::eof()) {
			error = "candidate.chain contains trailing bytes";
			return false;
		}
		if (computedNodes != mmrNodes) {
			error = "header MMR node count mismatch after recomputation";
			return false;
		}

		for (uint64_t height = 0; height < confirmedEntries; ++height) {
			std::array<uint8_t, 32> chainBytes{};
			confirmedFile.read(reinterpret_cast<char*>(chainBytes.data()), chainBytes.size());
			if (confirmedFile.gcount() != static_cast<std::streamsize>(chainBytes.size())) {
				error = "confirmed.chain is truncated at height " + std::to_string(height);
				return false;
			}

			const Hash chainHash = HashFromRaw(chainBytes.data());
			const auto pIndex = confirmedChain.GetByHeight(height);
			if (pIndex == nullptr || pIndex->GetHash() != chainHash) {
				error = "confirmed.chain entry does not match the in-memory chain at height " + std::to_string(height);
				return false;
			}

			const auto pCandidate = candidateChain.GetByHeight(height);
			if (pCandidate == nullptr || pCandidate->GetHash() != chainHash) {
				error = "confirmed chain is not a prefix of the candidate chain at height " + std::to_string(height);
				return false;
			}
		}
		if (confirmedFile.peek() != std::char_traits<char>::eof()) {
			error = "confirmed.chain contains trailing bytes";
			return false;
		}

		const auto end = std::chrono::steady_clock::now();
		metrics.DurationMs = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
		return true;
	}

	bool Refresh(
		const Config& config,
		const IBlockDB& blockDB,
		const IHeaderMMR& headerMMR,
		const Chain& candidateChain,
		const Chain& confirmedChain,
		HeaderCheckpointMetrics& metrics,
		std::string& error,
		const bool verifyMmrInternal)
	{
		HeaderCheckpointData stored;
		if (!Read(Path(config), stored, error)) {
			return false;
		}
		if (stored.Trust == HeaderCheckpointData::TRUST_NONE) {
			error = "checkpoint is not trusted";
			return false;
		}

		HeaderCheckpointData before;
		if (!ReadMetadata(config, blockDB, headerMMR, candidateChain, confirmedChain, before, error)) {
			return false;
		}
		if (before.MagicBytes != stored.MagicBytes ||
			before.GenesisHash != stored.GenesisHash ||
			before.DatabaseVersion != stored.DatabaseVersion ||
			before.SegmentSize != stored.SegmentSize ||
			before.MmrLeaves != before.CandidateEntries) {
			error = "checkpoint metadata no longer matches the profile";
			return false;
		}
		if (before.CandidateEntries < stored.CandidateEntries ||
			before.ConfirmedEntries < stored.ConfirmedEntries) {
			error = "chain rewound or reorged; full validation required";
			return false;
		}
		if (before.CandidateEntries == stored.CandidateEntries &&
			before.ConfirmedEntries == stored.ConfirmedEntries &&
			before.MmrNodes == stored.MmrNodes &&
			before.MmrFileSize == stored.MmrFileSize &&
			before.CandidateFileSize == stored.CandidateFileSize &&
			before.ConfirmedFileSize == stored.ConfirmedFileSize) {
			error = "checkpoint is already current";
			return false;
		}

		const auto oldTip = candidateChain.GetByHeight(stored.CandidateEntries - 1);
		if (oldTip == nullptr || oldTip->GetHash() != stored.CandidateTip) {
			error = "previous checkpoint tip is not on the current candidate chain";
			return false;
		}
		if (stored.ConfirmedEntries > 0) {
			const auto oldConfirmed = confirmedChain.GetByHeight(stored.ConfirmedEntries - 1);
			if (oldConfirmed == nullptr || oldConfirmed->GetHash() != stored.ConfirmedTip) {
				error = "previous checkpoint confirmed tip is not on the current confirmed chain";
				return false;
			}
		}

		// Bounded append-only extension. The old checkpoint is trusted and its
		// old prefix is checked below by segment hashes. Validate the new header
		// chain and MMR leaves before accepting it; anything unusual falls back
		// to the existing full validation.
		const uint64_t newCandidateEntries = before.CandidateEntries;
		if (!ExtensionEntryCountAllowed(stored.CandidateEntries, newCandidateEntries)) {
			error = "checkpoint is too far behind for bounded extension";
			return false;
		}
		if (stored.CandidateEntries == 0 ||
			headerMMR.Root(stored.CandidateEntries - 1) != stored.MmrRoot) {
			error = "previous checkpoint MMR root does not match the current prefix";
			return false;
		}

		std::vector<uint8_t> extensionMmr;
		if (!ReadFile(config.GetChainPath() / "header_mmr.bin", extensionMmr, error) ||
			extensionMmr.size() != before.MmrFileSize) {
			error = "header_mmr.bin is unavailable or changed during extension";
			return false;
		}

		Hash previousHash = stored.CandidateTip;
		const BlockHeaderPtr pOldHeader = blockDB.GetBlockHeader(stored.CandidateTip);
		if (pOldHeader == nullptr) {
			error = "previous checkpoint header is missing";
			return false;
		}
		BlockHeaderPtr previousHeader = pOldHeader;
		for (uint64_t height = stored.CandidateEntries; height < newCandidateEntries; ++height) {
			const auto pIndex = candidateChain.GetByHeight(height);
			if (pIndex == nullptr) {
				error = "extension candidate header index is missing at height " + std::to_string(height);
				return false;
			}
			const Hash chainHash = pIndex->GetHash();
			const BlockHeaderPtr pHeader = blockDB.GetBlockHeader(chainHash);
			if (pHeader == nullptr ||
				pHeader->GetHeight() != height ||
				pHeader->GetHash() != chainHash ||
				pHeader->GetPreviousHash() != previousHash) {
				error = "extension header/hash/link mismatch at height " + std::to_string(height);
				return false;
			}
			if (pHeader->GetVersion() != Consensus::GetHeaderVersion(height) ||
				pHeader->GetTimestamp() > Consensus::GetMaxBlockTime(std::chrono::system_clock::now()) ||
				pHeader->GetTimestamp() <= previousHeader->GetTimestamp() ||
				pHeader->GetTotalDifficulty() <= previousHeader->GetTotalDifficulty()) {
				error = "extension header fields are not monotonic at height " + std::to_string(height);
				return false;
			}

			const uint64_t position = LeafIndex::At(height).GetPosition();
			Hash storedLeaf;
			if (!HashAt(extensionMmr, position, storedLeaf) ||
				storedLeaf != HashLeaf(*pHeader, position)) {
				error = "extension header MMR leaf mismatch at height " + std::to_string(height);
				return false;
			}

			previousHash = chainHash;
			previousHeader = pHeader;
		}

		for (uint64_t height = stored.ConfirmedEntries; height < before.ConfirmedEntries; ++height) {
			const auto pConfirmed = confirmedChain.GetByHeight(height);
			const auto pCandidate = candidateChain.GetByHeight(height);
			if (pConfirmed == nullptr || pCandidate == nullptr ||
				pConfirmed->GetHash() != pCandidate->GetHash()) {
				error = "extension confirmed chain is not a candidate-chain prefix at height " + std::to_string(height);
				return false;
			}
		}

		HeaderCheckpointData generated;
		if (!Compute(config, blockDB, headerMMR, candidateChain, confirmedChain, false, verifyMmrInternal, generated, metrics, error)) {
			return false;
		}

		auto headerSegmentHash = [&](const uint64_t startHeight, const uint64_t count, Hash& out) -> bool
		{
			std::ifstream file(config.GetChainPath() / "candidate.chain", std::ios::binary);
			if (!file.is_open()) {
				error = "cannot open candidate.chain";
				return false;
			}
			file.seekg(static_cast<std::streamoff>(startHeight * 32));
			std::vector<uint8_t> segment;
			segment.reserve(static_cast<size_t>(count) * 220);
			for (uint64_t i = 0; i < count; ++i) {
				std::array<uint8_t, 32> raw{};
				file.read(reinterpret_cast<char*>(raw.data()), raw.size());
				if (file.gcount() != static_cast<std::streamsize>(raw.size())) {
					error = "candidate.chain is truncated while refreshing checkpoint";
					return false;
				}
				const Hash hash = HashFromRaw(raw.data());
				const BlockHeaderPtr pHeader = blockDB.GetBlockHeader(hash);
				if (pHeader == nullptr) {
					error = "block header is missing while refreshing checkpoint";
					return false;
				}
				Serializer serializer;
				pHeader->Serialize(serializer);
				const auto bytes = serializer.GetBytes();
				segment.insert(segment.end(), raw.begin(), raw.end());
				AppendU32(segment, static_cast<uint32_t>(bytes.size()));
				segment.insert(segment.end(), bytes.begin(), bytes.end());
			}
			out = Hasher::SHA256(segment);
			return true;
		};

		auto mmrSegmentHash = [&](const uint64_t startNode, const uint64_t count, Hash& out) -> bool
		{
			std::ifstream file(config.GetChainPath() / "header_mmr.bin", std::ios::binary);
			if (!file.is_open()) {
				error = "cannot open header_mmr.bin";
				return false;
			}
			file.seekg(static_cast<std::streamoff>(startNode * 32));
			std::vector<uint8_t> bytes(static_cast<size_t>(count) * 32);
			file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (file.gcount() != static_cast<std::streamsize>(bytes.size())) {
				error = "header_mmr.bin is truncated while refreshing checkpoint";
				return false;
			}
			out = Hasher::SHA256(bytes);
			return true;
		};

		auto confirmedSegmentHash = [&](const uint64_t startHeight, const uint64_t count, Hash& out) -> bool
		{
			std::ifstream file(config.GetChainPath() / "confirmed.chain", std::ios::binary);
			if (!file.is_open()) {
				error = "cannot open confirmed.chain";
				return false;
			}
			file.seekg(static_cast<std::streamoff>(startHeight * 32));
			std::vector<uint8_t> bytes(static_cast<size_t>(count) * 32);
			file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (file.gcount() != static_cast<std::streamsize>(bytes.size())) {
				error = "confirmed.chain is truncated while refreshing checkpoint";
				return false;
			}
			out = Hasher::SHA256(bytes);
			return true;
		};

		const auto verifyOldPrefix = [&](const std::vector<Hash>& oldHashes,
			const std::vector<Hash>& newHashes,
			const uint64_t oldItems,
			const uint64_t segmentSize,
			auto&& lastSegmentHash) -> bool
		{
			if (oldHashes.size() > newHashes.size()) {
				error = "checkpoint segment list shrank";
				return false;
			}
			for (size_t i = 0; i + 1 < oldHashes.size(); ++i) {
				if (oldHashes[i] != newHashes[i]) {
					error = "checkpoint prefix mismatch at segment " + std::to_string(i);
					return false;
				}
			}
			if (!oldHashes.empty()) {
				const size_t last = oldHashes.size() - 1;
				if (oldItems % segmentSize == 0) {
					if (oldHashes[last] != newHashes[last]) {
						error = "checkpoint final segment mismatch";
						return false;
					}
				} else {
					Hash oldLast;
					if (!lastSegmentHash(last, oldItems % segmentSize, oldLast)) {
						return false;
					}
					if (oldLast != oldHashes[last]) {
						error = "checkpoint final partial segment mismatch";
						return false;
					}
				}
			}
			return true;
		};

		if (!verifyOldPrefix(
				stored.HeaderSegments,
				generated.HeaderSegments,
				stored.CandidateEntries,
				stored.SegmentSize,
				[&](const size_t index, const uint64_t count, Hash& hash) {
					return headerSegmentHash(index * stored.SegmentSize, count, hash);
				})) {
			return false;
		}
		if (!verifyOldPrefix(
				stored.MmrSegments,
				generated.MmrSegments,
				stored.MmrNodes,
				stored.SegmentSize,
				[&](const size_t index, const uint64_t count, Hash& hash) {
					return mmrSegmentHash(index * stored.SegmentSize, count, hash);
				})) {
			return false;
		}
		if (!verifyOldPrefix(
				stored.ConfirmedSegments,
				generated.ConfirmedSegments,
				stored.ConfirmedEntries,
				stored.SegmentSize,
				[&](const size_t index, const uint64_t count, Hash& hash) {
					return confirmedSegmentHash(index * stored.SegmentSize, count, hash);
				})) {
			return false;
		}

		generated.Sequence = stored.Sequence + 1;
		generated.Trust = stored.Trust;
		generated.TrustSource = stored.Trust == HeaderCheckpointData::TRUST_BOOTSTRAP
			? HeaderCheckpointData::TRUST_DERIVED
			: stored.TrustSource;

		if (!Write(Path(config), generated, error)) {
			return false;
		}
		return true;
	}

	bool MutateHeaderForTest(
		const Chain& candidateChain,
		IBlockDB& blockDB,
		std::string& error)
	{
		const char* env = std::getenv("GRINPP_HEADER_CKPT_MUTATE");
		if (env == nullptr || *env == '\0') {
			return true;
		}

		const std::string spec(env);
		const size_t colon = spec.find(':');
		if (colon == std::string::npos) {
			error = "GRINPP_HEADER_CKPT_MUTATE must be height:field";
			return false;
		}

		uint64_t height = 0;
		try {
			height = std::stoull(spec.substr(0, colon));
		} catch (...) {
			error = "invalid mutation height";
			return false;
		}
		const std::string field = spec.substr(colon + 1);

		const auto pIndex = candidateChain.GetByHeight(height);
		if (pIndex == nullptr) {
			error = "mutation height is not on the candidate chain";
			return false;
		}
		const BlockHeaderPtr pHeader = blockDB.GetBlockHeader(pIndex->GetHash());
		if (pHeader == nullptr) {
			error = "mutation header is missing";
			return false;
		}

		uint16_t version = pHeader->GetVersion();
		int64_t timestamp = pHeader->GetTimestamp();
		Hash previousRoot = Hash(pHeader->GetPreviousRoot());
		Hash outputRoot = Hash(pHeader->GetOutputRoot());
		Hash rangeProofRoot = Hash(pHeader->GetRangeProofRoot());
		Hash kernelRoot = Hash(pHeader->GetKernelRoot());
		Hash offset = Hash(pHeader->GetTotalKernelOffset().GetBytes());
		uint64_t totalDifficulty = pHeader->GetTotalDifficulty();
		uint32_t scalingDifficulty = pHeader->GetScalingDifficulty();
		uint64_t nonce = pHeader->GetNonce();

		if (field == "version") {
			version = static_cast<uint16_t>(Consensus::GetHeaderVersion(height) + 1);
		} else if (field == "timestamp") {
			if (height == 0) {
				timestamp = 0;
			} else {
				const auto pPrevIndex = candidateChain.GetByHeight(height - 1);
				const auto pPrev = pPrevIndex == nullptr ? nullptr : blockDB.GetBlockHeader(pPrevIndex->GetHash());
				timestamp = pPrev == nullptr ? timestamp - 1 : pPrev->GetTimestamp();
			}
		} else if (field == "previous_root") {
			previousRoot = FlipHash(previousRoot);
		} else if (field == "output_root") {
			outputRoot = FlipHash(outputRoot);
		} else if (field == "range_proof_root") {
			rangeProofRoot = FlipHash(rangeProofRoot);
		} else if (field == "kernel_root") {
			kernelRoot = FlipHash(kernelRoot);
		} else if (field == "offset") {
			offset = FlipHash(offset);
		} else if (field == "total_difficulty") {
			totalDifficulty += 1;
		} else if (field == "scaling") {
			scalingDifficulty += 1;
		} else if (field == "nonce") {
			nonce += 1;
		} else {
			error = "unsupported mutation field: " + field;
			return false;
		}

		ProofOfWork proofOfWork = pHeader->GetProofOfWork();
		BlockHeader mutated(
			version,
			pHeader->GetHeight(),
			timestamp,
			Hash(pHeader->GetPreviousHash()),
			std::move(previousRoot),
			std::move(outputRoot),
			std::move(rangeProofRoot),
			std::move(kernelRoot),
			BlindingFactor(std::move(offset)),
			pHeader->GetOutputMMRSize(),
			pHeader->GetKernelMMRSize(),
			totalDifficulty,
			scalingDifficulty,
			nonce,
			std::move(proofOfWork));

		blockDB.AddBlockHeader(std::make_shared<BlockHeader>(std::move(mutated)));
		LOG_WARNING_F("event=header_field_mutation height={} field={}", height, field);
		return true;
	}

	bool Write(const fs::path& path, const HeaderCheckpointData& data, std::string& error)
	{
		static const uint8_t MAGIC[8] = { 'G', 'R', 'I', 'N', 'P', 'P', 'H', 'C' };

		std::vector<uint8_t> body;
		body.insert(body.end(), MAGIC, MAGIC + sizeof(MAGIC));
		AppendU32(body, data.Version);
		AppendU8(body, data.Trust);
		AppendU8(body, data.TrustSource);
		AppendU64(body, data.Sequence);
		AppendU64(body, data.CandidateEntries);
		AppendU64(body, data.ConfirmedEntries);
		AppendU64(body, data.MmrLeaves);
		AppendU64(body, data.MmrNodes);
		AppendU64(body, data.MmrFileSize);
		AppendU64(body, data.CandidateFileSize);
		AppendU64(body, data.ConfirmedFileSize);
		AppendU8(body, data.DatabaseVersion);
		AppendU64(body, data.SegmentSize);
		AppendBytes(body, data.MagicBytes);
		AppendHash(body, data.GenesisHash);
		AppendHash(body, data.CandidateTip);
		AppendHash(body, data.ConfirmedTip);
		AppendHash(body, data.MmrRoot);
		AppendHash(body, data.Aggregate);
		AppendU32(body, static_cast<uint32_t>(data.HeaderSegments.size()));
		for (const Hash& hash : data.HeaderSegments) AppendHash(body, hash);
		AppendU32(body, static_cast<uint32_t>(data.MmrSegments.size()));
		for (const Hash& hash : data.MmrSegments) AppendHash(body, hash);
		AppendU32(body, static_cast<uint32_t>(data.ConfirmedSegments.size()));
		for (const Hash& hash : data.ConfirmedSegments) AppendHash(body, hash);
		AppendHash(body, Hasher::SHA256(body));

		fs::path temp = path;
		temp += ".tmp";
		{
			std::ofstream file(temp, std::ios::binary | std::ios::trunc);
			if (!file.is_open()) {
				error = "cannot open checkpoint temp file";
				return false;
			}
			file.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
			file.flush();
			if (!file.good()) {
				error = "cannot write checkpoint temp file";
				file.close();
				std::error_code ec;
				fs::remove(temp, ec);
				return false;
			}
		}

		if (!FileSync::Flush(temp)) {
			error = "cannot flush checkpoint temp file";
			std::error_code ec;
			fs::remove(temp, ec);
			return false;
		}

#ifdef _WIN32
		if (!MoveFileExW(
				temp.wstring().c_str(),
				path.wstring().c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			error = "cannot replace checkpoint file";
			std::error_code ec;
			fs::remove(temp, ec);
			return false;
		}
#else
		std::error_code ec;
		fs::rename(temp, path, ec);
		if (ec) {
			error = "cannot replace checkpoint file: " + ec.message();
			fs::remove(temp, ec);
			return false;
		}
#endif

		return true;
	}

	bool Read(const fs::path& path, HeaderCheckpointData& out, std::string& error)
	{
		std::vector<uint8_t> bytes;
		if (!ReadFile(path, bytes, error)) {
			return false;
		}

		static const uint8_t MAGIC[8] = { 'G', 'R', 'I', 'N', 'P', 'P', 'H', 'C' };
		if (bytes.size() < sizeof(MAGIC) + 32) {
			error = "checkpoint file is too short";
			return false;
		}
		if (std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0) {
			error = "bad checkpoint magic";
			return false;
		}

		const size_t bodySize = bytes.size() - 32;
		const Hash storedChecksum = Hasher::SHA256(bytes.data(), bodySize);
		Hash actualChecksum;
		{
			std::vector<unsigned char> checksumBytes(bytes.begin() + bodySize, bytes.end());
			actualChecksum = Hash(std::move(checksumBytes));
		}
		if (storedChecksum != actualChecksum) {
			error = "checkpoint checksum mismatch";
			return false;
		}

		CheckpointReader reader(bytes);
		uint8_t magic[8]{};
		for (uint8_t& byte : magic) {
			if (!reader.ReadU8(byte)) { error = "truncated checkpoint"; return false; }
		}
		if (std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0) {
			error = "bad checkpoint magic";
			return false;
		}

		out = HeaderCheckpointData{};
		uint32_t version = 0;
		if (!reader.ReadU32(version)) { error = "truncated checkpoint"; return false; }
		out.Version = version;
		if (out.Version != HeaderCheckpointData::CURRENT_VERSION) {
			error = "unsupported checkpoint version";
			return false;
		}

		if (!reader.ReadU8(out.Trust) ||
			!reader.ReadU8(out.TrustSource) ||
			!reader.ReadU64(out.Sequence) ||
			!reader.ReadU64(out.CandidateEntries) ||
			!reader.ReadU64(out.ConfirmedEntries) ||
			!reader.ReadU64(out.MmrLeaves) ||
			!reader.ReadU64(out.MmrNodes) ||
			!reader.ReadU64(out.MmrFileSize) ||
			!reader.ReadU64(out.CandidateFileSize) ||
			!reader.ReadU64(out.ConfirmedFileSize) ||
			!reader.ReadU8(out.DatabaseVersion) ||
			!reader.ReadU64(out.SegmentSize) ||
			!reader.ReadBytes(out.MagicBytes) ||
			!reader.ReadHash(out.GenesisHash) ||
			!reader.ReadHash(out.CandidateTip) ||
			!reader.ReadHash(out.ConfirmedTip) ||
			!reader.ReadHash(out.MmrRoot) ||
			!reader.ReadHash(out.Aggregate)) {
			error = "truncated checkpoint";
			return false;
		}

		auto readSegments = [&](std::vector<Hash>& segments) -> bool
		{
			uint32_t count = 0;
			if (!reader.ReadU32(count) || count > MAX_SEGMENTS) {
				return false;
			}
			segments.resize(count);
			for (Hash& hash : segments) {
				if (!reader.ReadHash(hash)) {
					return false;
				}
			}
			return true;
		};

		if (!readSegments(out.HeaderSegments) ||
			!readSegments(out.MmrSegments) ||
			!readSegments(out.ConfirmedSegments)) {
			error = "truncated checkpoint segment list";
			return false;
		}

		if (reader.Remaining() != 32) {
			error = "checkpoint has trailing data";
			return false;
		}

		if (out.Trust > HeaderCheckpointData::TRUST_DERIVED ||
			out.TrustSource > HeaderCheckpointData::TRUST_DERIVED) {
			error = "checkpoint has an invalid trust field";
			return false;
		}

		if (out.SegmentSize == 0 || out.SegmentSize > 1000000) {
			error = "invalid checkpoint segment size";
			return false;
		}
		if (out.MmrFileSize != out.MmrNodes * 32 ||
			out.CandidateFileSize != out.CandidateEntries * 32 ||
			out.ConfirmedFileSize != out.ConfirmedEntries * 32) {
			error = "checkpoint file-size fields are inconsistent";
			return false;
		}

		const uint64_t headerSegments = (out.CandidateEntries + out.SegmentSize - 1) / out.SegmentSize;
		const uint64_t mmrSegments = (out.MmrNodes + out.SegmentSize - 1) / out.SegmentSize;
		const uint64_t confirmedSegments = (out.ConfirmedEntries + out.SegmentSize - 1) / out.SegmentSize;
		if (out.HeaderSegments.size() != headerSegments ||
			out.MmrSegments.size() != mmrSegments ||
			out.ConfirmedSegments.size() != confirmedSegments) {
			error = "checkpoint segment counts are inconsistent";
			return false;
		}

		return true;
	}

	bool MetadataEquals(const HeaderCheckpointData& expected, const HeaderCheckpointData& actual)
	{
		return expected.CandidateEntries == actual.CandidateEntries
			&& expected.ConfirmedEntries == actual.ConfirmedEntries
			&& expected.MmrLeaves == actual.MmrLeaves
			&& expected.MmrNodes == actual.MmrNodes
			&& expected.MmrFileSize == actual.MmrFileSize
			&& expected.CandidateFileSize == actual.CandidateFileSize
			&& expected.ConfirmedFileSize == actual.ConfirmedFileSize
			&& expected.DatabaseVersion == actual.DatabaseVersion
			&& expected.SegmentSize == actual.SegmentSize
			&& expected.MagicBytes == actual.MagicBytes
			&& expected.GenesisHash == actual.GenesisHash
			&& expected.CandidateTip == actual.CandidateTip
			&& expected.ConfirmedTip == actual.ConfirmedTip;
	}

	bool StateEquals(const HeaderCheckpointData& expected, const HeaderCheckpointData& actual)
	{
		return expected.CandidateEntries == actual.CandidateEntries
			&& expected.ConfirmedEntries == actual.ConfirmedEntries
			&& expected.MmrLeaves == actual.MmrLeaves
			&& expected.MmrNodes == actual.MmrNodes
			&& expected.MmrFileSize == actual.MmrFileSize
			&& expected.CandidateFileSize == actual.CandidateFileSize
			&& expected.ConfirmedFileSize == actual.ConfirmedFileSize
			&& expected.DatabaseVersion == actual.DatabaseVersion
			&& expected.SegmentSize == actual.SegmentSize
			&& expected.MagicBytes == actual.MagicBytes
			&& expected.GenesisHash == actual.GenesisHash
			&& expected.CandidateTip == actual.CandidateTip
			&& expected.ConfirmedTip == actual.ConfirmedTip
			&& expected.MmrRoot == actual.MmrRoot
			&& expected.Aggregate == actual.Aggregate
			&& expected.HeaderSegments == actual.HeaderSegments
			&& expected.MmrSegments == actual.MmrSegments
			&& expected.ConfirmedSegments == actual.ConfirmedSegments;
	}

	std::string DescribeMismatch(const HeaderCheckpointData& expected, const HeaderCheckpointData& actual)
	{
		std::ostringstream out;
		out << "expected candidate=" << expected.CandidateEntries
			<< " confirmed=" << expected.ConfirmedEntries
			<< " leaves=" << expected.MmrLeaves
			<< " nodes=" << expected.MmrNodes
			<< " tip=" << expected.CandidateTip.ToHex()
			<< "; actual candidate=" << actual.CandidateEntries
			<< " confirmed=" << actual.ConfirmedEntries
			<< " leaves=" << actual.MmrLeaves
			<< " nodes=" << actual.MmrNodes
			<< " tip=" << actual.CandidateTip.ToHex();

		const size_t headerCount = (std::min)(expected.HeaderSegments.size(), actual.HeaderSegments.size());
		for (size_t i = 0; i < headerCount; ++i) {
			if (expected.HeaderSegments[i] != actual.HeaderSegments[i]) {
				out << "; first mismatching header segment=" << i
					<< " heights=" << (i * expected.SegmentSize) << ".."
					<< (std::min)((i + 1) * expected.SegmentSize, expected.CandidateEntries) - 1;
				break;
			}
		}

		const size_t mmrCount = (std::min)(expected.MmrSegments.size(), actual.MmrSegments.size());
		for (size_t i = 0; i < mmrCount; ++i) {
			if (expected.MmrSegments[i] != actual.MmrSegments[i]) {
				out << "; first mismatching MMR segment=" << i;
				break;
			}
		}

		return out.str();
	}

	std::string Describe(const HeaderCheckpointData& data)
	{
		std::ostringstream out;
		out << "sequence=" << data.Sequence
			<< " trust=" << (uint32_t)data.Trust
			<< " trust_source=" << (uint32_t)data.TrustSource
			<< " candidate=" << data.CandidateEntries
			<< " confirmed=" << data.ConfirmedEntries
			<< " mmr_leaves=" << data.MmrLeaves
			<< " mmr_nodes=" << data.MmrNodes
			<< " segment=" << data.SegmentSize
			<< " tip=" << data.CandidateTip.ToHex()
			<< " aggregate=" << data.Aggregate.ToHex();
		return out.str();
	}
}

