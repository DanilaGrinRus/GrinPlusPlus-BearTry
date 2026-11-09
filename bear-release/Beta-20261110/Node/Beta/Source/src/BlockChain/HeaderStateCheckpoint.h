#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <filesystem.h>
#include <Crypto/Models/Hash.h>

// Prototype checkpoint for the header/header-MMR startup verification.
//
// The checkpoint is a cache of the result of the existing G-23 full header
// validation. It is only written after a successful full validation and is
// only used when the current logical state is an exact match for the state the
// checkpoint was built from. Anything else falls back to the existing full
// validation.
//
// This is deliberately a separate experimental component: the release path is
// unchanged unless GRINPP_HEADER_CKPT is set.

class Config;
class IBlockDB;
class IHeaderMMR;
class Chain;

struct HeaderCheckpointMetrics
{
	uint64_t DurationMs = 0;
	uint64_t CpuMs = 0;
	uint64_t BytesRead = 0;
	uint64_t CandidateEntries = 0;
	uint64_t MmrNodes = 0;
	uint64_t SegmentSize = 0;
	uint64_t DbReadMs = 0;
	uint64_t SerializeMs = 0;
	uint64_t HashMs = 0;
	uint64_t FileReadMs = 0;
	uint32_t Threads = 1;
	std::string Mode;
};

struct HeaderCheckpointData
{
	static constexpr uint32_t CURRENT_VERSION = 2;

	// 0 = not trusted, 1 = externally supplied anchor, 2 = explicit local
	// bootstrap, 3 = derived from a trusted checkpoint after validated headers.
	static constexpr uint8_t TRUST_NONE = 0;
	static constexpr uint8_t TRUST_EXTERNAL = 1;
	static constexpr uint8_t TRUST_BOOTSTRAP = 2;
	static constexpr uint8_t TRUST_DERIVED = 3;

	uint32_t Version = CURRENT_VERSION;
	uint8_t Trust = TRUST_NONE;
	uint8_t TrustSource = 0;
	uint64_t Sequence = 0;
	uint64_t CandidateEntries = 0;
	uint64_t ConfirmedEntries = 0;
	uint64_t MmrLeaves = 0;
	uint64_t MmrNodes = 0;
	uint64_t MmrFileSize = 0;
	uint64_t CandidateFileSize = 0;
	uint64_t ConfirmedFileSize = 0;
	uint8_t DatabaseVersion = 0;
	uint64_t SegmentSize = 0;

	std::vector<uint8_t> MagicBytes;
	Hash GenesisHash;
	Hash CandidateTip;
	Hash ConfirmedTip;
	Hash MmrRoot;
	Hash Aggregate;

	std::vector<Hash> HeaderSegments;
	std::vector<Hash> MmrSegments;
	std::vector<Hash> ConfirmedSegments;
};

// Operator-published anchor manifest. This is provenance, not a signature:
// Signature is deliberately rejected until real signature verification exists.
struct HeaderCheckpointAnchor
{
	std::string Network;
	std::string Source;
	Hash GenesisHash;
	uint64_t CandidateEntries = 0;
	uint64_t ConfirmedEntries = 0;
	uint64_t MmrNodes = 0;
	Hash CandidateTip;
	Hash Aggregate;
};

namespace HeaderCheckpoint
{
	bool ParseAnchorManifest(
		const fs::path& path,
		HeaderCheckpointAnchor& out,
		std::string& error
	);

	bool AnchorMatches(
		const HeaderCheckpointAnchor& anchor,
		const HeaderCheckpointData& data,
		const std::string& expectedNetwork,
		std::string& error
	);

	enum class EMode
	{
		Off,
		Full,
		HashOnly,
		Write,
		Fast
	};

	EMode ModeFromEnv();
	bool ForceFullFromEnv();
	bool BootstrapFromEnv();
	std::string TrustAnchorFromEnv();

	// Thread policy for the parallel header-hash fast path.
	// 0 / unset / "auto" means auto; 1..32 is a manual cap.
	uint32_t ChooseThreadCount(uint32_t availableCores, uint64_t availableMemoryMb);
	uint32_t ResolveThreadCount();

	// Testable predicates used by bounded extension validation.
	bool ExtensionEntryCountAllowed(uint64_t storedEntries, uint64_t currentEntries) noexcept;
	bool ExtensionMmrNodeMatches(
		uint64_t expectedPosition,
		uint64_t actualPosition,
		const Hash& expected,
		const Hash& actual
	) noexcept;

	// Full local verification used for the ordinary startup path and before a
	// trusted checkpoint is (re)written. This is stronger than the historical
	// G-23: it also verifies every internal header-MMR node, the header fields
	// that are locally verifiable, and the confirmed-chain prefix.
	bool VerifyFull(
		const Config& config,
		const IBlockDB& blockDB,
		const IHeaderMMR& headerMMR,
		const Chain& candidateChain,
		const Chain& confirmedChain,
		HeaderCheckpointMetrics& metrics,
		std::string& error
	);

	// Rewrites a trusted checkpoint after the chain has advanced. It only
	// accepts an append-only transition whose old tip is still present; reorgs
	// and rewinds are rejected here and handled by full startup validation.
	bool Refresh(
		const Config& config,
		const IBlockDB& blockDB,
		const IHeaderMMR& headerMMR,
		const Chain& candidateChain,
		const Chain& confirmedChain,
		HeaderCheckpointMetrics& metrics,
		std::string& error,
		bool verifyMmrInternal = false
	);

	// Test-only deliberate DB field corruption. GRINPP_HEADER_CKPT_MUTATE is
	// "height:field" and is ignored unless set.
	bool MutateHeaderForTest(
		const Chain& candidateChain,
		IBlockDB& blockDB,
		std::string& error
	);

	// Fills only the cheap metadata fields (counts, tips, file sizes, version).
	bool ReadMetadata(
		const Config& config,
		const IBlockDB& blockDB,
		const IHeaderMMR& headerMMR,
		const Chain& candidateChain,
		const Chain& confirmedChain,
		HeaderCheckpointData& out,
		std::string& error
	);

	// Reads all covered data, optionally performs the same semantic checks as
	// the G-23 startup loop, optionally verifies the internal header MMR nodes,
	// and computes the aggregate and segment hashes.
	bool Compute(
		const Config& config,
		const IBlockDB& blockDB,
		const IHeaderMMR& headerMMR,
		const Chain& candidateChain,
		const Chain& confirmedChain,
		bool validateHeaders,
		bool verifyMmrInternal,
		HeaderCheckpointData& out,
		HeaderCheckpointMetrics& metrics,
		std::string& error
	);

	bool Read(const fs::path& path, HeaderCheckpointData& out, std::string& error);
	bool Write(const fs::path& path, const HeaderCheckpointData& data, std::string& error);

	// Cheap metadata comparison (no hash/segment data).
	bool MetadataEquals(const HeaderCheckpointData& expected, const HeaderCheckpointData& actual);

	// Metadata and all segment/aggregate values, ignoring Sequence.
	bool StateEquals(const HeaderCheckpointData& expected, const HeaderCheckpointData& actual);

	std::string DescribeMismatch(const HeaderCheckpointData& expected, const HeaderCheckpointData& actual);
	std::string Describe(const HeaderCheckpointData& data);
	fs::path Path(const Config& config);
}
