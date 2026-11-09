#include <catch.hpp>

#include <Common/Util/BitUtil.h>
#include <Core/Serialization/Serializer.h>
#include <P2P/PIBD/BitmapSegmentAssembler.h>
#include <PMMR/Common/MMRHashUtil.h>
#include <PMMR/Common/MMRUtil.h>
#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/InMemoryPmmr.h>
#include <PMMR/Models/Segment.h>
#include <PMMR/Models/SegmentException.h>
#include <PMMR/Models/SegmentGenerator.h>
#include <PMMR/Models/SegmentIdentifier.h>

#include <cstdint>
#include <vector>

namespace
{
	// PMMR size for a given number of leaves (2N - popcount(N)).
	uint64_t MmrSizeForLeaves(const uint64_t numLeaves)
	{
		return 2 * numLeaves - (uint64_t)BitUtil::CountBitsSet(numLeaves);
	}

	InMemoryPmmr<BitmapChunk> MakeBitmapMmr(const std::vector<BitmapChunk>& chunks)
	{
		std::vector<BitmapChunk> copy = chunks;
		return InMemoryPmmr<BitmapChunk>(std::move(copy));
	}

	BitmapSegment SegmentFor(
		const InMemoryPmmr<BitmapChunk>& mmr, const uint64_t idx)
	{
		const Segment<BitmapChunk> segment =
			SegmentGenerator<BitmapChunk>::Generate(mmr, SegmentIdentifier(BitmapSegmentAssembler::SEGMENT_HEIGHT, idx));
		return BitmapSegment::FromSegment(segment);
	}
}

TEST_CASE("BitmapSegment::FromSegmentRoundTrip")
{
	BitmapChunk chunk0;
	chunk0.Set(0, true);
	chunk0.Set(1023, true);

	std::vector<BitmapChunk> chunks{ chunk0, BitmapChunk() };
	const InMemoryPmmr<BitmapChunk> mmr = MakeBitmapMmr(chunks);

	const BitmapSegment segment = SegmentFor(mmr, 0);
	REQUIRE(segment.GetIdentifier() == SegmentIdentifier(BitmapSegmentAssembler::SEGMENT_HEIGHT, 0));
	REQUIRE(segment.GetNumChunks() == 2);

	const Segment<BitmapChunk> back = segment.ToSegment();
	REQUIRE(back.GetLeafData().size() == 2);
	REQUIRE(back.GetLeafData()[0].Get(0));
	REQUIRE(back.GetLeafData()[0].Get(1023));
	REQUIRE(!back.GetLeafData()[1].Any());
}

TEST_CASE("BitmapSegmentAssembler::SingleSegment")
{
	const uint64_t numOutputs = 1500;   // two 1024-bit chunks

	BitmapChunk chunk0;
	chunk0.Set(0, true);
	chunk0.Set(5, true);
	chunk0.Set(1023, true);
	BitmapChunk chunk1;
	chunk1.Set(0, true);
	chunk1.Set(7, true);

	std::vector<BitmapChunk> chunks{ chunk0, chunk1 };
	const InMemoryPmmr<BitmapChunk> mmr = MakeBitmapMmr(chunks);
	const Hash root = mmr.GetRoot();

	// The header output root commits H(output_mmr_size || output_root || bitmap_root).
	const Hash outputRoot = Hash::ValueOf(3);
	const Hash headerRoot = MMRHashUtil::HashParentWithIndex(outputRoot, root, MmrSizeForLeaves(numOutputs));
	BitmapSegmentAssembler assembler(MmrSizeForLeaves(numOutputs), headerRoot);
	REQUIRE(assembler.GetNumChunks() == 2);
	REQUIRE(assembler.GetRequiredSegments() == 1);
	REQUIRE(!assembler.IsComplete());

	const BitmapSegment segment = SegmentFor(mmr, 0);
	REQUIRE(assembler.Accept(segment, outputRoot));
	REQUIRE(assembler.IsComplete());
	REQUIRE(assembler.BuildBitmapMmrRoot() == root);
	REQUIRE(assembler.GetOutputMmrRoot() == outputRoot);
	REQUIRE(!assembler.Accept(segment, outputRoot));   // duplicate is ignored

	const VectorSegmentBitmap bitmap = assembler.BuildBitmap();
	REQUIRE(bitmap.Contains(0));
	REQUIRE(bitmap.Contains(5));
	REQUIRE(bitmap.Contains(1023));
	REQUIRE(bitmap.Contains(1024));
	REQUIRE(bitmap.Contains(1031));
	REQUIRE(!bitmap.Contains(1));
	REQUIRE(!bitmap.Contains(1025));
}

TEST_CASE("BitmapSegmentAssembler::RejectsInvalidSegments")
{
	const uint64_t numOutputs = 1500;
	std::vector<BitmapChunk> chunks{ BitmapChunk(), BitmapChunk() };
	const InMemoryPmmr<BitmapChunk> mmr = MakeBitmapMmr(chunks);
	const Hash root = mmr.GetRoot();
	const BitmapSegment valid = SegmentFor(mmr, 0);

	const Hash outputRoot = Hash::ValueOf(4);
	const Hash headerRoot = MMRHashUtil::HashParentWithIndex(outputRoot, root, MmrSizeForLeaves(numOutputs));

	// Wrong horizon root.
	BitmapSegmentAssembler wrongRoot(MmrSizeForLeaves(numOutputs), HASH::ZERO);
	REQUIRE_THROWS_AS(wrongRoot.Accept(valid, outputRoot), SegmentException);

	// Correct segment but a wrong carried output root: the combined root fails.
	BitmapSegmentAssembler wrongOutputRoot(MmrSizeForLeaves(numOutputs), headerRoot);
	REQUIRE_THROWS_AS(wrongOutputRoot.Accept(valid, HASH::ZERO), SegmentException);

	// Wrong segment height.
	BitmapSegmentAssembler assembler(MmrSizeForLeaves(numOutputs), headerRoot);
	const Segment<BitmapChunk> wrongHeight = SegmentGenerator<BitmapChunk>::Generate(
		mmr, SegmentIdentifier(BitmapSegmentAssembler::SEGMENT_HEIGHT + 1, 0));
	REQUIRE_THROWS_AS(assembler.Accept(BitmapSegment::FromSegment(wrongHeight), outputRoot), SegmentException);

	// Out of range index: relabel the valid segment (generation itself would
	// refuse to create a segment that does not exist).
	const BitmapSegment outOfRange(
		SegmentIdentifier(BitmapSegmentAssembler::SEGMENT_HEIGHT, 1),
		valid.GetBlocks(),
		valid.GetProof());
	REQUIRE_THROWS_AS(assembler.Accept(outOfRange, outputRoot), SegmentException);

	// Tampered chunk bits must break the merkle proof.
	std::vector<BitmapBlock> blocks;
	blocks.push_back(BitmapBlock(2));
	blocks.back().Set(3, true);
	const BitmapSegment tampered(
		SegmentIdentifier(BitmapSegmentAssembler::SEGMENT_HEIGHT, 0),
		std::move(blocks),
		valid.GetProof());
	REQUIRE_THROWS_AS(assembler.Accept(tampered, outputRoot), SegmentException);
}

TEST_CASE("BitmapSegmentAssembler::MultipleSegmentsInReverseOrder")
{
	const uint64_t numChunks = 600;                          // needs two segments
	const uint64_t numOutputs = numChunks * BitmapChunk::LEN_BITS;

	std::vector<BitmapChunk> chunks((size_t)numChunks);
	for (uint64_t i = 0; i < numChunks; ++i) {
		chunks[(size_t)i].Set((size_t)(i % BitmapChunk::LEN_BITS), true);
	}

	const InMemoryPmmr<BitmapChunk> mmr = MakeBitmapMmr(chunks);
	const Hash root = mmr.GetRoot();

	const Hash outputRoot = Hash::ValueOf(5);
	const Hash headerRoot = MMRHashUtil::HashParentWithIndex(outputRoot, root, MmrSizeForLeaves(numOutputs));
	BitmapSegmentAssembler assembler(MmrSizeForLeaves(numOutputs), headerRoot);
	REQUIRE(assembler.GetNumChunks() == numChunks);
	REQUIRE(assembler.GetRequiredSegments() == 2);

	// Request/response order is not guaranteed: accept the last segment first.
	const BitmapSegment second = SegmentFor(mmr, 1);
	const BitmapSegment first = SegmentFor(mmr, 0);

	REQUIRE(assembler.Accept(second, outputRoot));
	REQUIRE(assembler.GetReceivedSegments() == 1);
	REQUIRE(!assembler.IsComplete());
	REQUIRE(assembler.GetMissing().size() == 1);
	REQUIRE(assembler.GetMissing()[0] == SegmentIdentifier(BitmapSegmentAssembler::SEGMENT_HEIGHT, 0));

	REQUIRE(assembler.Accept(first, outputRoot));
	REQUIRE(assembler.IsComplete());
	REQUIRE(assembler.GetMissing().empty());
	REQUIRE(assembler.BuildBitmapMmrRoot() == root);

	const VectorSegmentBitmap bitmap = assembler.BuildBitmap();
	REQUIRE(bitmap.Contains(0));
	REQUIRE(bitmap.Contains(513 * BitmapChunk::LEN_BITS + 513));
	REQUIRE(bitmap.Contains(599 * BitmapChunk::LEN_BITS + 599));
	REQUIRE(!bitmap.Contains(513 * BitmapChunk::LEN_BITS + 512));
}
