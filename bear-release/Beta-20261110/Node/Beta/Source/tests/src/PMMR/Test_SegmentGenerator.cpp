#include <catch.hpp>

#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <PMMR/Common/MMRUtil.h>
#include <PMMR/Models/InMemoryPmmr.h>
#include <PMMR/Models/SegmentBitmap.h>
#include <PMMR/Models/Segment.h>
#include <PMMR/Models/SegmentException.h>
#include <PMMR/Models/SegmentGenerator.h>
#include <PMMR/Models/SegmentIdentifier.h>

#include <cstdint>
#include <vector>

namespace
{
	struct GenLeaf
	{
		uint32_t Value;

		void Serialize(Serializer& serializer) const
		{
			serializer.Append<uint32_t>(Value);
		}

		static GenLeaf Deserialize(ByteBuffer& byteBuffer)
		{
			return GenLeaf{ byteBuffer.ReadU32() };
		}
	};

	InMemoryPmmr<GenLeaf> MakePmmr(const uint64_t numLeaves)
	{
		std::vector<GenLeaf> leaves;
		leaves.reserve((size_t)numLeaves);
		for (uint64_t i = 0; i < numLeaves; ++i) {
			leaves.push_back(GenLeaf{ (uint32_t)(1000 + i) });
		}

		return InMemoryPmmr<GenLeaf>(std::move(leaves));
	}

	// Emulates a compact PMMR: leaf data exists only for bitmap entries.
	Segment<GenLeaf> GeneratePrunedFromCompactSource(
		const InMemoryPmmr<GenLeaf>& mmr,
		const SegmentIdentifier& identifier,
		const SegmentBitmap& bitmap)
	{
		return SegmentGenerator<GenLeaf>::GenerateFromSource(
			identifier,
			mmr.GetSize(),
			&bitmap,
			[&mmr, &bitmap](const uint64_t pos0) -> std::optional<GenLeaf> {
				const uint64_t leafIdx = MMRUtil::GetNumLeaves(pos0 + 1) - 1;
				const bool required = IsSegmentLeafRequired(pos0, mmr.GetSize(), bitmap);
				if (!required && !bitmap.Contains(leafIdx)) {
					return std::nullopt;
				}
				return std::optional<GenLeaf>(mmr.GetLeafData(pos0));
			},
			[&mmr](const uint64_t pos0) -> std::optional<Hash> {
				return std::optional<Hash>(mmr.GetHash(pos0));
			});
	}
}

TEST_CASE("InMemoryPmmr::Shape")
{
	const InMemoryPmmr<GenLeaf> oneLeaf = MakePmmr(1);
	REQUIRE(oneLeaf.GetSize() == 1);
	REQUIRE(oneLeaf.GetNumLeaves() == 1);

	const InMemoryPmmr<GenLeaf> fourLeaves = MakePmmr(4);
	REQUIRE(fourLeaves.GetSize() == 7);   // 2*4 - popcount(4) = 7
	REQUIRE(fourLeaves.IsLeafAt(0));
	REQUIRE(!fourLeaves.IsLeafAt(2));
	REQUIRE(fourLeaves.GetLeafData(3).Value == 1002);
	REQUIRE(fourLeaves.GetLeafData(4).Value == 1003);
}

TEST_CASE("SegmentGenerator::RoundTripsForAllShapes")
{
	const uint64_t leafCounts[] = { 1, 2, 3, 4, 5, 8, 13 };
	const uint8_t heights[] = { 0, 1, 2, 3 };

	for (const uint64_t numLeaves : leafCounts) {
		const InMemoryPmmr<GenLeaf> mmr = MakePmmr(numLeaves);
		const uint64_t mmrSize = mmr.GetSize();
		const Hash root = mmr.GetRoot();

		for (const uint8_t height : heights) {
			const uint64_t required = SegmentIdentifier::CountSegmentsRequired(mmrSize, height);
			for (uint64_t idx = 0; idx < required; ++idx) {
				const Segment<GenLeaf> segment =
					SegmentGenerator<GenLeaf>::Generate(mmr, SegmentIdentifier(height, idx));
				INFO("leaves=" << numLeaves << " height=" << (int)height << " idx=" << idx);
				REQUIRE_NOTHROW(segment.Validate(mmrSize, root));
			}
		}
	}
}

TEST_CASE("SegmentGenerator::RejectsNonExistentSegment")
{
	const InMemoryPmmr<GenLeaf> mmr = MakePmmr(13);
	const uint64_t required = SegmentIdentifier::CountSegmentsRequired(mmr.GetSize(), 2);
	REQUIRE_THROWS_AS(
		SegmentGenerator<GenLeaf>::Generate(mmr, SegmentIdentifier(2, required)), SegmentException);
}

TEST_CASE("SegmentGenerator::ReassemblesFullTree")
{
	const uint64_t numLeaves = 13;
	const InMemoryPmmr<GenLeaf> mmr = MakePmmr(numLeaves);
	const uint64_t mmrSize = mmr.GetSize();
	const uint8_t height = 2;
	const uint64_t required = SegmentIdentifier::CountSegmentsRequired(mmrSize, height);
	REQUIRE(required == 4);

	std::vector<GenLeaf> reassembled((size_t)numLeaves);
	std::vector<bool> seen((size_t)numLeaves, false);

	for (uint64_t idx = 0; idx < required; ++idx) {
		const Segment<GenLeaf> segment =
			SegmentGenerator<GenLeaf>::Generate(mmr, SegmentIdentifier(height, idx));
		REQUIRE_NOTHROW(segment.Validate(mmrSize, mmr.GetRoot()));
		for (size_t i = 0; i < segment.GetLeafPositions().size(); ++i) {
			const uint64_t pos0 = segment.GetLeafPositions()[i];
			// Map the MMR position back to the leaf index by walking the leaves.
			uint64_t leafIndex = 0;
			for (uint64_t p = 0; p <= pos0; ++p) {
				if (MMRUtil::IsLeaf(p)) {
					++leafIndex;
				}
			}

			REQUIRE(leafIndex > 0);
			REQUIRE(!seen[(size_t)(leafIndex - 1)]);
			seen[(size_t)(leafIndex - 1)] = true;
			reassembled[(size_t)(leafIndex - 1)] = segment.GetLeafData()[i];
		}
	}

	for (const bool value : seen) {
		REQUIRE(value);
	}

	const InMemoryPmmr<GenLeaf> rebuilt(std::move(reassembled));
	REQUIRE(rebuilt.GetSize() == mmrSize);
	REQUIRE(rebuilt.GetRoot() == mmr.GetRoot());
}

TEST_CASE("SegmentGenerator::TamperedSegmentIsRejected")
{
	const InMemoryPmmr<GenLeaf> mmr = MakePmmr(13);
	const uint64_t mmrSize = mmr.GetSize();
	const Segment<GenLeaf> segment =
		SegmentGenerator<GenLeaf>::Generate(mmr, SegmentIdentifier(2, 1));

	std::vector<GenLeaf> tampered = segment.GetLeafData();
	REQUIRE(!tampered.empty());
	tampered[0].Value += 1;

	const Segment<GenLeaf> bad(
		segment.GetIdentifier(),
		segment.GetHashPositions(),
		segment.GetHashes(),
		segment.GetLeafPositions(),
		tampered,
		segment.GetProof()
	);

	REQUIRE_THROWS_AS(bad.Validate(mmrSize, mmr.GetRoot()), SegmentException);
}

TEST_CASE("SegmentGenerator::PrunedSegmentsValidateWithBitmap")
{
	// 8 leaves => complete tree, MMR size 15.
	const InMemoryPmmr<GenLeaf> mmr = MakePmmr(8);
	const uint64_t mmrSize = mmr.GetSize();
	REQUIRE(mmrSize == 15);
	const Hash root = mmr.GetRoot();

	// Only leaf 0 is unspent: leaves 0 and 1 are required, the rest are pruned.
	VectorSegmentBitmap bitmap(8);
	bitmap.Set(0, true);

	// Height 2 => two segments of four leaves.
	const uint64_t required = SegmentIdentifier::CountSegmentsRequired(mmrSize, 2);
	REQUIRE(required == 2);
	for (uint64_t idx = 0; idx < required; ++idx) {
		const Segment<GenLeaf> segment =
			GeneratePrunedFromCompactSource(mmr, SegmentIdentifier(2, idx), bitmap);
		INFO("pruned height 2 segment idx=" << idx);
		REQUIRE(segment.GetLeafData().size() < 4);   // pruning actually removed leaves
		REQUIRE_NOTHROW(segment.ValidateWithBitmap(mmrSize, bitmap, root));
	}
}

TEST_CASE("SegmentGenerator::FullyPrunedSegmentUsesStoredParentHash")
{
	const InMemoryPmmr<GenLeaf> mmr = MakePmmr(8);
	const uint64_t mmrSize = mmr.GetSize();
	const Hash root = mmr.GetRoot();

	VectorSegmentBitmap bitmap(8);
	bitmap.Set(0, true);

	// Height 1, idx 1: leaves 2 and 3, both pruned.
	const Segment<GenLeaf> segment =
		GeneratePrunedFromCompactSource(mmr, SegmentIdentifier(1, 1), bitmap);
	REQUIRE(segment.GetLeafData().empty());
	REQUIRE(!segment.GetHashes().empty());

	const std::optional<Hash> segmentRoot = segment.RootWithBitmap(mmrSize, bitmap);
	REQUIRE(!segmentRoot.has_value());   // fully pruned segment has no root
	REQUIRE_NOTHROW(segment.ValidateWithBitmap(mmrSize, bitmap, root));

	// A wrong stored hash must not validate.
	std::vector<Hash> tampered = segment.GetHashes();
	tampered.back() = root;
	const Segment<GenLeaf> bad(
		segment.GetIdentifier(),
		segment.GetHashPositions(),
		tampered,
		segment.GetLeafPositions(),
		segment.GetLeafData(),
		segment.GetProof()
	);
	REQUIRE_THROWS_AS(bad.ValidateWithBitmap(mmrSize, bitmap, root), SegmentException);
}

TEST_CASE("SegmentGenerator::SendsAvailableLeafDataEvenWhenBitmapIsClear")
{
	// grin Segment::from_pmmr sends leaf data whenever the backend has it,
	// regardless of the bitmap bit. The receiver then removes spent leaves from
	// its leaf set. Omitting such a leaf creates a gap in the sequential PMMR.
	const InMemoryPmmr<GenLeaf> mmr = MakePmmr(8);
	VectorSegmentBitmap bitmap(8);
	bitmap.Set(0, true);

	const Segment<GenLeaf> segment =
		SegmentGenerator<GenLeaf>::Generate(mmr, SegmentIdentifier(2, 1), &bitmap);
	REQUIRE(segment.GetLeafData().size() == 4);
	REQUIRE_NOTHROW(segment.ValidateWithBitmap(mmr.GetSize(), bitmap, mmr.GetRoot()));
}
