#include <catch.hpp>

#include <BlockChain/HeaderStateCheckpoint.h>

#include <cstdlib>
#include <filesystem.h>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <cstdlib>
#endif

namespace
{
	fs::path TempHeaderCheckpointPath()
	{
		static uint32_t counter = 0;
		const fs::path dir = fs::temp_directory_path() /
			("grinpp-header-checkpoint-" + std::to_string(++counter));
		fs::create_directories(dir);
		return dir / "header.ckpt";
	}

	HeaderCheckpointData MakeCheckpoint()
	{
		HeaderCheckpointData data;
		data.Sequence = 42;
		data.Trust = HeaderCheckpointData::TRUST_EXTERNAL;
		data.TrustSource = HeaderCheckpointData::TRUST_EXTERNAL;
		data.CandidateEntries = 2;
		data.ConfirmedEntries = 1;
		data.MmrLeaves = 2;
		data.MmrNodes = 3;
		data.MmrFileSize = 3 * 32;
		data.CandidateFileSize = 2 * 32;
		data.ConfirmedFileSize = 1 * 32;
		data.DatabaseVersion = 3;
		data.SegmentSize = 1000;
		data.MagicBytes = { 83, 59 };
		data.GenesisHash = Hash::ValueOf(1);
		data.CandidateTip = Hash::ValueOf(2);
		data.ConfirmedTip = Hash::ValueOf(3);
		data.MmrRoot = Hash::ValueOf(4);
		data.HeaderSegments = { Hash::ValueOf(5) };
		data.MmrSegments = { Hash::ValueOf(6) };
		data.ConfirmedSegments = { Hash::ValueOf(7) };
		data.Aggregate = Hash::ValueOf(8);
		return data;
	}

	void SetEnv(const char* name, const char* value)
	{
#ifdef _WIN32
		_putenv_s(name, value);
#else
		setenv(name, value, 1);
#endif
	}
}

TEST_CASE("HeaderCheckpoint::RoundTripAndTrustFields")
{
	const fs::path path = TempHeaderCheckpointPath();
	const HeaderCheckpointData written = MakeCheckpoint();
	std::string error;
	REQUIRE(HeaderCheckpoint::Write(path, written, error));
	REQUIRE(error.empty());

	HeaderCheckpointData read;
	REQUIRE(HeaderCheckpoint::Read(path, read, error));
	REQUIRE(error.empty());
	REQUIRE(read.Version == HeaderCheckpointData::CURRENT_VERSION);
	REQUIRE(read.Trust == HeaderCheckpointData::TRUST_EXTERNAL);
	REQUIRE(read.TrustSource == HeaderCheckpointData::TRUST_EXTERNAL);
	REQUIRE(read.CandidateEntries == 2);
	REQUIRE(read.ConfirmedEntries == 1);
	REQUIRE(read.MmrNodes == 3);
	REQUIRE(read.Aggregate == written.Aggregate);
	REQUIRE(HeaderCheckpoint::StateEquals(written, read));
}

TEST_CASE("HeaderCheckpoint::RejectsCorruptionAndMissingFile")
{
	const fs::path path = TempHeaderCheckpointPath();
	std::string error;
	REQUIRE(HeaderCheckpoint::Write(path, MakeCheckpoint(), error));

	{
		std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
		REQUIRE(file.is_open());
		file.seekp(10);
		const char byte = 0x5A;
		file.write(&byte, 1);
	}
	HeaderCheckpointData read;
	REQUIRE_FALSE(HeaderCheckpoint::Read(path, read, error));
	REQUIRE_FALSE(error.empty());

	const fs::path missing = path.parent_path() / "missing.ckpt";
	REQUIRE_FALSE(HeaderCheckpoint::Read(missing, read, error));
}

TEST_CASE("HeaderCheckpoint::RejectsInvalidTrustFields")
{
	const fs::path path = TempHeaderCheckpointPath();
	HeaderCheckpointData data = MakeCheckpoint();
	data.Trust = 9;
	std::string error;
	REQUIRE(HeaderCheckpoint::Write(path, data, error));

	HeaderCheckpointData read;
	REQUIRE_FALSE(HeaderCheckpoint::Read(path, read, error));
	REQUIRE(error.find("trust") != std::string::npos);
}

TEST_CASE("HeaderCheckpoint::AnchorManifest")
{
	const fs::path path = TempHeaderCheckpointPath();
	const HeaderCheckpointData data = MakeCheckpoint();
	const std::string json =
		"{\"network\":\"FLOONET\","
		"\"source\":\"test fixture\","
		"\"genesis\":\"" + data.GenesisHash.ToHex() + "\","
		"\"candidate_entries\":" + std::to_string(data.CandidateEntries) + ","
		"\"candidate_height\":" + std::to_string(data.CandidateEntries - 1) + ","
		"\"confirmed_entries\":" + std::to_string(data.ConfirmedEntries) + ","
		"\"mmr_nodes\":" + std::to_string(data.MmrNodes) + ","
		"\"tip\":\"" + data.CandidateTip.ToHex() + "\","
		"\"aggregate\":\"" + data.Aggregate.ToHex() + "\"}";
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		REQUIRE(file.is_open());
		file << json;
	}

	HeaderCheckpointAnchor anchor;
	std::string error;
	REQUIRE(HeaderCheckpoint::ParseAnchorManifest(path, anchor, error));
	REQUIRE(anchor.Network == "FLOONET");
	REQUIRE(anchor.Source == "test fixture");
	REQUIRE(HeaderCheckpoint::AnchorMatches(anchor, data, "FLOONET", error));
	REQUIRE_FALSE(HeaderCheckpoint::AnchorMatches(anchor, data, "MAINNET", error));
	REQUIRE(error.find("network") != std::string::npos);

	HeaderCheckpointData other = data;
	other.CandidateTip = Hash::ValueOf(9);
	REQUIRE_FALSE(HeaderCheckpoint::AnchorMatches(anchor, other, "FLOONET", error));
	REQUIRE(error.find("tip") != std::string::npos);

	// An unverified signature must not be accepted as if it were verified.
	std::string signedJson = json;
	signedJson.insert(signedJson.size() - 1, ",\"signature\":\"deadbeef\"");
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << signedJson;
	}
	REQUIRE_FALSE(HeaderCheckpoint::ParseAnchorManifest(path, anchor, error));
	REQUIRE(error.find("signature") != std::string::npos);
}

TEST_CASE("HeaderCheckpoint::ModeAndAnchorEnv")
{
	SetEnv("GRINPP_HEADER_CKPT", "off");
	REQUIRE(HeaderCheckpoint::ModeFromEnv() == HeaderCheckpoint::EMode::Off);
	SetEnv("GRINPP_HEADER_CKPT", "full");
	REQUIRE(HeaderCheckpoint::ModeFromEnv() == HeaderCheckpoint::EMode::Full);
	SetEnv("GRINPP_HEADER_CKPT", "hash");
	REQUIRE(HeaderCheckpoint::ModeFromEnv() == HeaderCheckpoint::EMode::HashOnly);
	SetEnv("GRINPP_HEADER_CKPT", "checkpoint");
	REQUIRE(HeaderCheckpoint::ModeFromEnv() == HeaderCheckpoint::EMode::Write);
	SetEnv("GRINPP_HEADER_CKPT", "fast");
	REQUIRE(HeaderCheckpoint::ModeFromEnv() == HeaderCheckpoint::EMode::Fast);

	// "write" is not an accepted value; it must disable the prototype.
	SetEnv("GRINPP_HEADER_CKPT", "write");
	REQUIRE(HeaderCheckpoint::ModeFromEnv() == HeaderCheckpoint::EMode::Off);

	SetEnv("GRINPP_HEADER_CKPT", "off");
	SetEnv("GRINPP_HEADER_CKPT_BOOTSTRAP", "1");
	REQUIRE(HeaderCheckpoint::BootstrapFromEnv());
	SetEnv("GRINPP_HEADER_CKPT_BOOTSTRAP", "0");
	REQUIRE_FALSE(HeaderCheckpoint::BootstrapFromEnv());

	SetEnv("GRINPP_HEADER_CKPT_TRUST_ANCHOR", "ABC123");
	REQUIRE(HeaderCheckpoint::TrustAnchorFromEnv() == "ABC123");
	SetEnv("GRINPP_HEADER_CKPT_TRUST_ANCHOR", "");
	REQUIRE(HeaderCheckpoint::TrustAnchorFromEnv().empty());
}

TEST_CASE("HeaderCheckpoint::ThreadPolicy")
{
	// CPU policy: leave one core for node/P2P/UI, but never reserve on 1-2 cores.
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(1, 8192) == 1);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(2, 8192) == 2);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(4, 8192) == 3);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(8, 8192) == 7);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(16, 8192) == 8);

	// Memory policy: keep 1 GiB reserve, ~512 MiB per worker, cap at 8.
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(8, 512) == 1);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(8, 2048) == 2);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(8, 4096) == 6);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(8, 0) <= 8);

	SetEnv("GRINPP_HEADER_CKPT_THREADS", "4");
	REQUIRE(HeaderCheckpoint::ResolveThreadCount() == 4);
	SetEnv("GRINPP_HEADER_CKPT_THREADS", "100");
	REQUIRE(HeaderCheckpoint::ResolveThreadCount() == 32);
	SetEnv("GRINPP_HEADER_CKPT_THREADS", "auto");
	REQUIRE(HeaderCheckpoint::ResolveThreadCount() >= 1);
	REQUIRE(HeaderCheckpoint::ResolveThreadCount() <= 8);
	SetEnv("GRINPP_HEADER_CKPT_THREADS", "invalid");
	REQUIRE(HeaderCheckpoint::ResolveThreadCount() >= 1);
	SetEnv("GRINPP_HEADER_CKPT_THREADS", "");
}

TEST_CASE("HeaderCheckpoint::ThreadPolicyBounds")
{
	// Manual cap is accepted for every value 1..32 and clamps above 32.
	for (uint32_t requested = 1; requested <= 32; ++requested)
	{
		SetEnv("GRINPP_HEADER_CKPT_THREADS", std::to_string(requested).c_str());
		REQUIRE(HeaderCheckpoint::ResolveThreadCount() == requested);
	}
	SetEnv("GRINPP_HEADER_CKPT_THREADS", "0");
	REQUIRE(HeaderCheckpoint::ResolveThreadCount() >= 1);
	SetEnv("GRINPP_HEADER_CKPT_THREADS", "-1");
	REQUIRE(HeaderCheckpoint::ResolveThreadCount() >= 1);
	SetEnv("GRINPP_HEADER_CKPT_THREADS", "not-a-number");
	REQUIRE(HeaderCheckpoint::ResolveThreadCount() >= 1);
	SetEnv("GRINPP_HEADER_CKPT_THREADS", "");

	// Low-resource policy never returns zero and keeps a hard worker cap.
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(16, 512) == 1);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(1, 512) == 1);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(0, 512) == 1);
	REQUIRE(HeaderCheckpoint::ChooseThreadCount(64, 65536) == 8);
}

TEST_CASE("HeaderCheckpoint::BoundedExtensionCapAndMmrPredicates")
{
	// Production Refresh uses ExtensionEntryCountAllowed for the cap.
	REQUIRE(HeaderCheckpoint::ExtensionEntryCountAllowed(100, 10100));   // exactly 10000
	REQUIRE_FALSE(HeaderCheckpoint::ExtensionEntryCountAllowed(100, 10101)); // 10001
	REQUIRE_FALSE(HeaderCheckpoint::ExtensionEntryCountAllowed(100, 99));

	// Production Refresh uses ExtensionMmrNodeMatches for leaf/internal nodes.
	const Hash expected = Hash::ValueOf(1);
	REQUIRE(HeaderCheckpoint::ExtensionMmrNodeMatches(7, 7, expected, expected));
	REQUIRE_FALSE(HeaderCheckpoint::ExtensionMmrNodeMatches(7, 8, expected, expected));
	REQUIRE_FALSE(HeaderCheckpoint::ExtensionMmrNodeMatches(7, 7, expected, Hash::ValueOf(2)));
}

