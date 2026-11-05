#include <catch.hpp>

#include <Common/Util/BitUtil.h>
#include <Common/Util/FileUtil.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Crypto/Models/Hash.h>
#include <PMMR/Models/BitmapBlock.h>
#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/Segment.h>

#include <filesystem.h>
#include <vector>

// Bit ordering in grin's BitVec::to_bytes is MSB-first within each byte. Raw
// bitmap blocks and bitmap chunks are serialized as those bytes, and sparse
// blocks encode bit indices from the same numbering. Getting this order wrong
// still round-trips raw blocks but corrupts sparse (Positive/Negative) blocks.
TEST_CASE("BitmapBlock::BitOrderMatchesGrin")
{
	BitmapBlock block(1);
	block.Set(0, true);
	block.Set(9, true);
	REQUIRE(block.Get(0));
	REQUIRE(block.Get(9));
	REQUIRE(!block.Get(1));

	// Two set bits out of 1024 select Positive mode: nc, mode, u16 count, two u16 positions.
	const std::vector<uint8_t> bytes = block.Serialized();
	REQUIRE(bytes.size() == 2 + 2 + 4);
	REQUIRE(bytes[0] == 1);
	REQUIRE(bytes[1] == 1);
	REQUIRE(bytes[2] == 0x00);
	REQUIRE(bytes[3] == 0x02);
	REQUIRE(bytes[4] == 0x00);
	REQUIRE(bytes[5] == 0x00);
	REQUIRE(bytes[6] == 0x00);
	REQUIRE(bytes[7] == 0x09);

	ByteBuffer buffer(bytes);
	const BitmapBlock parsed = BitmapBlock::Deserialize(buffer);
	REQUIRE(parsed.Get(0));
	REQUIRE(parsed.Get(9));
	REQUIRE(!parsed.Get(8));

	// Raw chunk bytes: bit 0 lives in the most significant bit of byte 0.
	BitmapChunk chunk;
	chunk.Set(0, true);
	chunk.Set(9, true);
	REQUIRE(chunk.Get(0));
	REQUIRE(chunk.Get(9));
	const std::vector<uint8_t>& chunkBytes = chunk.GetBytes();
	REQUIRE(chunkBytes[0] == 0x80);
	REQUIRE(chunkBytes[1] == 0x40);
}

// Vectors produced by a small program built against grin_chain 5.5.2
// (BitmapAccumulator with 3344 chunks, segments height 9 and 13). The files are
// BitmapSegment bodies without the p2p response wrapper; the test is skipped when
// they are absent (CI, clean checkouts).
TEST_CASE("PibdOracleVector::BitmapRootsMatchRust")
{
	const fs::path dir = "fixtures/pibd/oracle";
	const fs::path fullSegmentPath = dir / "seg_h9_i0.bin";
	const fs::path wholeSegmentPath = dir / "seg_h13_i0.bin";
	if (!FileUtil::Exists(fullSegmentPath) || !FileUtil::Exists(wholeSegmentPath)) {
		WARN("Rust oracle vectors are not present; skipping");
		return;
	}

	const uint64_t bitmapMmrSize = 6684;

	auto loadSegment = [](const fs::path& path) -> BitmapSegment {
		std::vector<uint8_t> data;
		REQUIRE(FileUtil::ReadFile(path, data));
		ByteBuffer buffer(data);
		const BitmapSegment segment = BitmapSegment::Deserialize(buffer);
		REQUIRE(buffer.GetRemainingSize() == 0);
		return segment;
	};

	const BitmapSegment fullSegment = loadSegment(fullSegmentPath);
	REQUIRE(fullSegment.GetIdentifier() == SegmentIdentifier(9, 0));
	const Segment<BitmapChunk> fullPmmrSegment = fullSegment.ToSegment();
	REQUIRE(fullPmmrSegment.GetLeafData().size() == 512);
	const Hash fullRoot = fullPmmrSegment.Root(bitmapMmrSize);
	REQUIRE(fullRoot.GetData() ==
		CBigInteger<32>::FromHex("ff6d8a52517c299fa5873a017e12de772625cab7400f4d81cc8fb32612e8a743").GetData());

	const BitmapSegment wholeSegment = loadSegment(wholeSegmentPath);
	REQUIRE(wholeSegment.GetIdentifier() == SegmentIdentifier(13, 0));
	const Segment<BitmapChunk> wholePmmrSegment = wholeSegment.ToSegment();
	REQUIRE(wholePmmrSegment.GetLeafData().size() == 3344);
	const Hash wholeRoot = wholePmmrSegment.Root(bitmapMmrSize);
	REQUIRE(wholeRoot.GetData() ==
		CBigInteger<32>::FromHex("d8bcf07b29a930cf9934509eae96a7c07f219fb6150d5b07a85da47839611b25").GetData());
}
