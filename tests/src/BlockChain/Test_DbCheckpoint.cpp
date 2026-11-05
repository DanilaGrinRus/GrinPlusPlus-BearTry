#include <catch.hpp>

#include <BlockChain/DbCheckpoint.h>

#include <filesystem.h>
#include <fstream>

namespace
{
	fs::path TempCheckpointPath()
	{
		static uint32_t counter = 0;
		const fs::path dir = fs::temp_directory_path() /
			("grinpp-checkpoint-" + std::to_string(++counter));
		fs::create_directories(dir);
		return dir / "chain.checkpoint";
	}
}

TEST_CASE("DbCheckpoint::RoundTrip")
{
	const fs::path path = TempCheckpointPath();

	DbCheckpointData written;
	written.Sequence = 42;
	written.MmrLeaves = 1000;
	written.CandidateEntries = 999;
	written.ConfirmedEntries = 700;
	REQUIRE(DbCheckpoint::Write(path, written));

	DbCheckpointData read;
	REQUIRE(DbCheckpoint::Read(path, read));
	REQUIRE(read.Sequence == 42);
	REQUIRE(read.MmrLeaves == 1000);
	REQUIRE(read.CandidateEntries == 999);
	REQUIRE(read.ConfirmedEntries == 700);

	// Rewriting replaces the previous checkpoint.
	written.Sequence = 43;
	written.MmrLeaves = 1001;
	REQUIRE(DbCheckpoint::Write(path, written));
	REQUIRE(DbCheckpoint::Read(path, read));
	REQUIRE(read.Sequence == 43);
	REQUIRE(read.MmrLeaves == 1001);
}

TEST_CASE("DbCheckpoint::RejectsCorruptionAndTruncation")
{
	const fs::path path = TempCheckpointPath();

	DbCheckpointData data;
	data.Sequence = 1;
	data.MmrLeaves = 10;
	data.CandidateEntries = 10;
	data.ConfirmedEntries = 5;
	REQUIRE(DbCheckpoint::Write(path, data));

	{
		std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
		file.seekp(20);
		file.put(static_cast<char>(0x7F));
	}

	DbCheckpointData read;
	REQUIRE(!DbCheckpoint::Read(path, read));

	// Truncated checkpoint is not trusted either.
	REQUIRE(DbCheckpoint::Write(path, data));
	fs::resize_file(path, 20);
	REQUIRE(!DbCheckpoint::Read(path, read));

	REQUIRE(!DbCheckpoint::Read(path.string() + ".missing", read));
}

TEST_CASE("DbCheckpoint::RejectsInvalidHeights")
{
	const fs::path path = TempCheckpointPath();

	DbCheckpointData data;
	data.Sequence = 1;
	data.MmrLeaves = 10;
	data.CandidateEntries = 10;
	data.ConfirmedEntries = 20; // confirmed ahead of candidate
	REQUIRE(DbCheckpoint::Write(path, data));

	DbCheckpointData read;
	REQUIRE(!DbCheckpoint::Read(path, read));
}
