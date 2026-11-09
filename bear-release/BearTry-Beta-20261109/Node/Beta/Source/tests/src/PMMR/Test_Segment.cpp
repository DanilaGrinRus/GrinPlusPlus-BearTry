#include <catch.hpp>

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Models/OutputIdentifier.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>

#include <PMMR/Models/BitmapBlock.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/Segment.h>
#include <PMMR/Models/SegmentIdentifier.h>
#include <PMMR/Models/SegmentProof.h>
#include <PMMR/Common/MMRUtil.h>

#include <P2P/Messages/SegmentRequestMessage.h>
#include <P2P/Messages/GetHeaderSegmentMessage.h>
#include <P2P/Messages/HeaderSegmentMessage.h>
#include <P2P/Messages/SegmentResponseMessage.h>

#include <cstdint>
#include <vector>

namespace
{
	struct TestLeaf
	{
		uint32_t Value;

		void Serialize(Serializer& serializer) const
		{
			serializer.Append<uint32_t>(Value);
		}

		static TestLeaf Deserialize(ByteBuffer& byteBuffer)
		{
			return TestLeaf{ byteBuffer.ReadU32() };
		}
	};

	Hash MakeHash(const uint8_t seed)
	{
		std::vector<uint8_t> bytes(32, seed);
		return Hash(std::move(bytes));
	}

	std::vector<uint8_t> BodyOf(const IMessage& message)
	{
		const std::vector<uint8_t> serialized = message.Serialize(EProtocolVersion::V1);
		REQUIRE(serialized.size() > 11);
		return std::vector<uint8_t>(serialized.cbegin() + 11, serialized.cend());
	}
}

TEST_CASE("MMRUtil::Positions")
{
	// A complete MMR over 4 leaves: leaves at 0,1,3,4, size 7.
	REQUIRE(MMRUtil::InsertionToPmmrIndex(0) == 0);
	REQUIRE(MMRUtil::InsertionToPmmrIndex(1) == 1);
	REQUIRE(MMRUtil::InsertionToPmmrIndex(2) == 3);
	REQUIRE(MMRUtil::InsertionToPmmrIndex(3) == 4);

	REQUIRE(MMRUtil::GetNumLeaves(7) == 4);
	REQUIRE(MMRUtil::GetNumLeaves(1) == 1);
	REQUIRE(MMRUtil::GetNumLeaves(0) == 0);

	REQUIRE(MMRUtil::IsLeaf(0));
	REQUIRE(MMRUtil::IsLeaf(4));
	REQUIRE(!MMRUtil::IsLeaf(2));

	REQUIRE(MMRUtil::BintreePostorderHeight(6) == 2);
	REQUIRE(MMRUtil::BintreePostorderHeight(2) == 1);

	// From leaf 4: sibling 3 (parent 5), then sibling 2 (parent 6).
	const std::vector<std::pair<uint64_t, uint64_t>> branch = MMRUtil::FamilyBranch(4, 7);
	REQUIRE(branch.size() == 2);
	REQUIRE(branch[0] == std::make_pair((uint64_t)5, (uint64_t)3));
	REQUIRE(branch[1] == std::make_pair((uint64_t)6, (uint64_t)2));
	REQUIRE(!MMRUtil::IsLeftSibling(4));  // position 4 is the right child of 5
	REQUIRE(MMRUtil::IsLeftSibling(3));   // position 3 is the left child of 5
	// Node 5 is the subtree root of leaves 3 and 4; a leaf is its own extreme.
	REQUIRE(MMRUtil::BintreeLeftmost(5) == 3);
	REQUIRE(MMRUtil::BintreeRightmost(5) == 4);
	REQUIRE(MMRUtil::BintreeLeftmost(4) == 4);
	REQUIRE(MMRUtil::BintreeRightmost(4) == 4);
}

TEST_CASE("SegmentIdentifier::WireFormat")
{
	const SegmentIdentifier identifier(9, 3);
	const std::vector<uint8_t> bytes = identifier.Serialized();
	REQUIRE(bytes.size() == 9);
	REQUIRE(bytes[0] == 9);
	REQUIRE(bytes[1] == 0); // idx is big-endian u64
	REQUIRE(bytes[8] == 3);

	ByteBuffer buffer(bytes);
	const SegmentIdentifier deserialized = SegmentIdentifier::Deserialize(buffer);
	REQUIRE(deserialized == identifier);
	REQUIRE(deserialized.SegmentCapacity() == 512);
	REQUIRE(deserialized.LeafOffset() == 3 * 512);
}

TEST_CASE("SegmentIdentifier::RejectsOversizedHeight")
{
	// height 33 is above the supported maximum and must not reach a shift by >= 64.
	const std::vector<uint8_t> bytes{ 33, 0, 0, 0, 0, 0, 0, 0, 0 };
	ByteBuffer buffer(bytes);
	REQUIRE_THROWS_AS(SegmentIdentifier::Deserialize(buffer), DeserializationException);
}

TEST_CASE("SegmentIdentifier::CountSegmentsRequired")
{
	// 1024 leaves with height 9 => exactly 2 segments.
	REQUIRE(SegmentIdentifier::CountSegmentsRequired(2047, 9) == 2);
	// 1 leaf => 1 segment.
	REQUIRE(SegmentIdentifier::CountSegmentsRequired(1, 9) == 1);
	// 513 leaves (mmr size 1024) => 2 segments.
	REQUIRE(MMRUtil::GetNumLeaves(1024) == 513);
	REQUIRE(SegmentIdentifier::CountSegmentsRequired(1024, 9) == 2);
}

TEST_CASE("SegmentProof::WireFormat")
{
	std::vector<Hash> hashes;
	hashes.push_back(MakeHash(1));
	hashes.push_back(MakeHash(2));
	const SegmentProof proof(std::move(hashes));

	const std::vector<uint8_t> bytes = proof.Serialized();
	REQUIRE(bytes.size() == 8 + 2 * 32);

	ByteBuffer buffer(bytes);
	const SegmentProof deserialized = SegmentProof::Deserialize(buffer);
	REQUIRE(deserialized.Size() == 2);
	REQUIRE(deserialized.GetHashes()[0] == MakeHash(1));
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("SegmentProof::RejectsHugeCounts")
{
	// count above MAX_HASHES
	std::vector<uint8_t> bytes(8, 0);
	bytes[0] = 0x00;
	bytes[7] = 0x01; // 1 hash, but no hash bytes follow
	ByteBuffer buffer(bytes);
	REQUIRE_THROWS_AS(SegmentProof::Deserialize(buffer), DeserializationException);

	// count larger than the payload can hold
	std::vector<uint8_t> huge(8, 0);
	huge[0] = 0x00;
	huge[1] = 0x10; // 4096 hashes, payload has none
	ByteBuffer hugeBuffer(huge);
	REQUIRE_THROWS_AS(SegmentProof::Deserialize(hugeBuffer), DeserializationException);
}

TEST_CASE("Segment<TestLeaf>::RoundTrip")
{
	std::vector<uint64_t> hashPositions{ 4 };
	std::vector<Hash> hashes{ MakeHash(7) };
	std::vector<uint64_t> leafPositions{ 0, 3 };
	std::vector<TestLeaf> leaves{ TestLeaf{ 11 }, TestLeaf{ 22 } };
	std::vector<Hash> proofHashes{ MakeHash(9) };

	const Segment<TestLeaf> segment(
		SegmentIdentifier(2, 1),
		std::move(hashPositions),
		std::move(hashes),
		std::move(leafPositions),
		std::move(leaves),
		SegmentProof(std::move(proofHashes))
	);

	const std::vector<uint8_t> bytes = segment.Serialized();
	ByteBuffer buffer(bytes);
	const Segment<TestLeaf> deserialized = Segment<TestLeaf>::Deserialize(buffer);

	REQUIRE(deserialized.GetIdentifier() == SegmentIdentifier(2, 1));
	REQUIRE(deserialized.GetHashPositions() == std::vector<uint64_t>{ 4 });
	REQUIRE(deserialized.GetHashes().size() == 1);
	REQUIRE(deserialized.GetLeafPositions() == std::vector<uint64_t>{ 0, 3 });
	REQUIRE(deserialized.GetLeafData().size() == 2);
	REQUIRE(deserialized.GetLeafData()[0].Value == 11);
	REQUIRE(deserialized.GetLeafData()[1].Value == 22);
	REQUIRE(deserialized.GetProof().Size() == 1);
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("Segment::RejectsNonIncreasingPositions")
{
	// identifier(2,1) + one hash at wire position 4, then a bad leaf position list.
	std::vector<uint8_t> bytes;
	Serializer serializer;
	serializer.Append<uint8_t>(2);
	serializer.Append<uint64_t>(1);
	serializer.Append<uint64_t>(1);   // one hash position
	serializer.Append<uint64_t>(4);   // wire pos 4 -> internal 3
	serializer.AppendByteVector(std::vector<uint8_t>(32, 0));
	serializer.Append<uint64_t>(2);   // two leaf positions
	serializer.Append<uint64_t>(3);   // 3
	serializer.Append<uint64_t>(3);   // equal -> not strictly increasing
	bytes = serializer.GetBytes();

	ByteBuffer buffer(bytes);
	REQUIRE_THROWS_AS(Segment<TestLeaf>::Deserialize(buffer), DeserializationException);
}

TEST_CASE("Segment::RejectsHugeItemCounts")
{
	Serializer serializer;
	serializer.Append<uint8_t>(9);
	serializer.Append<uint64_t>(0);
	serializer.Append<uint64_t>(1000001); // above MAX_SEGMENT_READ_ITEMS

	ByteBuffer buffer(serializer.GetBytes());
	REQUIRE_THROWS_AS(Segment<TestLeaf>::Deserialize(buffer), DeserializationException);
}

TEST_CASE("BitmapBlock::RoundTripModes")
{
	// Sparse: positive index mode.
	BitmapBlock sparse(16);
	sparse.Set(5, true);
	sparse.Set(9000, true);
	const std::vector<uint8_t> sparseBytes = sparse.Serialized();
	REQUIRE(sparseBytes[0] == 16);        // chunk count
	REQUIRE(sparseBytes[1] == 1);         // Positive mode
	ByteBuffer sparseBuffer(sparseBytes);
	const BitmapBlock sparseBack = BitmapBlock::Deserialize(sparseBuffer);
	REQUIRE(sparseBack.GetNumChunks() == 16);
	REQUIRE(sparseBack.Get(5));
	REQUIRE(sparseBack.Get(9000));
	REQUIRE(!sparseBack.Get(6));

	// Dense: negative index mode.
	BitmapBlock dense(16);
	for (size_t i = 0; i < dense.GetNumBits(); ++i) {
		dense.Set(i, true);
	}
	dense.Set(123, false);
	const std::vector<uint8_t> denseBytes = dense.Serialized();
	REQUIRE(denseBytes[1] == 2);          // Negative mode
	ByteBuffer denseBuffer(denseBytes);
	const BitmapBlock denseBack = BitmapBlock::Deserialize(denseBuffer);
	REQUIRE(denseBack.Get(0));
	REQUIRE(!denseBack.Get(123));

	// Mixed: raw mode (both set and unset counts >= MAX_BITS / 16).
	BitmapBlock mixed(16);
	for (size_t i = 0; i < mixed.GetNumBits(); ++i) {
		mixed.Set(i, (i % 2) == 0);
	}
	const std::vector<uint8_t> mixedBytes = mixed.Serialized();
	REQUIRE(mixedBytes[1] == 0);          // Raw mode
	REQUIRE(mixedBytes.size() == 2 + mixed.GetNumBits() / 8);
	ByteBuffer mixedBuffer(mixedBytes);
	const BitmapBlock mixedBack = BitmapBlock::Deserialize(mixedBuffer);
	REQUIRE(mixedBack.Get(0));
	REQUIRE(!mixedBack.Get(1));
}

TEST_CASE("BitmapBlock::RejectsMalformedInput")
{
	// Invalid chunk count.
	std::vector<uint8_t> tooManyChunks{ 65, 0 };
	ByteBuffer chunkBuffer(tooManyChunks);
	REQUIRE_THROWS_AS(BitmapBlock::Deserialize(chunkBuffer), DeserializationException);

	// Positive index out of range.
	Serializer serializer;
	serializer.Append<uint8_t>(1);   // 1 chunk = 1024 bits
	serializer.Append<uint8_t>(1);   // Positive
	serializer.Append<uint16_t>(1);
	serializer.Append<uint16_t>(1024); // == n_bits, must be rejected
	ByteBuffer indexBuffer(serializer.GetBytes());
	REQUIRE_THROWS_AS(BitmapBlock::Deserialize(indexBuffer), DeserializationException);

	// Unknown mode.
	std::vector<uint8_t> badMode{ 1, 7 };
	ByteBuffer modeBuffer(badMode);
	REQUIRE_THROWS_AS(BitmapBlock::Deserialize(modeBuffer), DeserializationException);
}

TEST_CASE("BitmapSegment::ValidationAndRoundTrip")
{
	// height 2 => up to 4 chunks; a single trailing block of 4 chunks is valid.
	std::vector<BitmapBlock> blocks;
	blocks.push_back(BitmapBlock(4));
	blocks.back().Set(1, true);
	const BitmapSegment segment(SegmentIdentifier(2, 0), std::move(blocks), SegmentProof::Empty());

	const std::vector<uint8_t> bytes = segment.Serialized();
	ByteBuffer buffer(bytes);
	const BitmapSegment deserialized = BitmapSegment::Deserialize(buffer);
	REQUIRE(deserialized.GetIdentifier() == SegmentIdentifier(2, 0));
	REQUIRE(deserialized.GetBlocks().size() == 1);
	REQUIRE(deserialized.GetNumChunks() == 4);
	REQUIRE(buffer.GetRemainingSize() == 0);

	// height above MAX_SEGMENT_HEIGHT is rejected.
	Serializer badHeightSerializer;
	badHeightSerializer.Append<uint8_t>(14);
	badHeightSerializer.Append<uint64_t>(0);
	badHeightSerializer.Append<uint16_t>(1);
	ByteBuffer badHeight(badHeightSerializer.GetBytes());
	REQUIRE_THROWS_AS(BitmapSegment::Deserialize(badHeight), DeserializationException);

	// zero blocks is rejected.
	Serializer noBlocksSerializer;
	noBlocksSerializer.Append<uint8_t>(2);
	noBlocksSerializer.Append<uint64_t>(0);
	noBlocksSerializer.Append<uint16_t>(0);
	ByteBuffer noBlocks(noBlocksSerializer.GetBytes());
	REQUIRE_THROWS_AS(BitmapSegment::Deserialize(noBlocks), DeserializationException);
}

TEST_CASE("BitmapSegment::RejectsNonFullLeadingBlock")
{
	std::vector<BitmapBlock> blocks;
	blocks.push_back(BitmapBlock(4));   // not full, but not the last block
	blocks.push_back(BitmapBlock(1));
	const BitmapSegment segment(SegmentIdentifier(4, 0), std::move(blocks), SegmentProof::Empty());

	ByteBuffer buffer(segment.Serialized());
	REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
}

TEST_CASE("SegmentRequestMessage::WireFormat")
{
	const SegmentRequestMessage message(
		MessageTypes::GetKernelSegment, MakeHash(3), SegmentIdentifier(11, 2));

	ByteBuffer body(BodyOf(message));
	const SegmentRequestMessage deserialized =
		SegmentRequestMessage::Deserialize(body, MessageTypes::GetKernelSegment);

	REQUIRE(deserialized.GetMessageType() == MessageTypes::GetKernelSegment);
	REQUIRE(deserialized.GetBlockHash() == MakeHash(3));
	REQUIRE(deserialized.GetIdentifier() == SegmentIdentifier(11, 2));
	REQUIRE(body.GetRemainingSize() == 0);
}

TEST_CASE("SegmentResponseMessage::RoundTripAllTypes")
{
	// OutputBitmapSegment (22): block hash, bitmap segment, output root.
	{
		std::vector<BitmapBlock> blocks;
		blocks.push_back(BitmapBlock(2));
		const SegmentResponseMessage message = SegmentResponseMessage::CreateOutputBitmap(
			MakeHash(4), BitmapSegment(SegmentIdentifier(1, 0), std::move(blocks), SegmentProof::Empty()), MakeHash(5));

		ByteBuffer body(BodyOf(message));
		const SegmentResponseMessage deserialized =
			SegmentResponseMessage::Deserialize(body, MessageTypes::OutputBitmapSegment);
		REQUIRE(deserialized.GetMessageType() == MessageTypes::OutputBitmapSegment);
		REQUIRE(deserialized.GetBlockHash() == MakeHash(4));
		REQUIRE(deserialized.GetExtraRoot().has_value());
		REQUIRE(deserialized.GetExtraRoot().value() == MakeHash(5));
		REQUIRE(std::holds_alternative<BitmapSegment>(deserialized.GetPayload()));
		REQUIRE(body.GetRemainingSize() == 0);
	}

	// OutputSegment (24): block hash, Segment<OutputIdentifier>, output bitmap root.
	{
		const Segment<OutputIdentifier> segment(
			SegmentIdentifier(11, 0), {}, {}, {}, {}, SegmentProof::Empty());
		const SegmentResponseMessage message = SegmentResponseMessage::CreateOutputs(
			MakeHash(6), segment, MakeHash(7));

		ByteBuffer body(BodyOf(message));
		const SegmentResponseMessage deserialized =
			SegmentResponseMessage::Deserialize(body, MessageTypes::OutputSegment);
		REQUIRE(deserialized.GetMessageType() == MessageTypes::OutputSegment);
		REQUIRE(deserialized.GetExtraRoot().value() == MakeHash(7));
		REQUIRE(std::holds_alternative<Segment<OutputIdentifier>>(deserialized.GetPayload()));
		REQUIRE(body.GetRemainingSize() == 0);
	}

	// RangeProofSegment (26) and KernelSegment (28) have no extra root.
	{
		const Segment<RangeProof> segment(
			SegmentIdentifier(11, 1), {}, {}, {}, {}, SegmentProof::Empty());
		const SegmentResponseMessage message = SegmentResponseMessage::CreateRangeProofs(MakeHash(8), segment);

		ByteBuffer body(BodyOf(message));
		const SegmentResponseMessage deserialized =
			SegmentResponseMessage::Deserialize(body, MessageTypes::RangeProofSegment);
		REQUIRE(!deserialized.GetExtraRoot().has_value());
		REQUIRE(std::holds_alternative<Segment<RangeProof>>(deserialized.GetPayload()));
		REQUIRE(body.GetRemainingSize() == 0);
	}

	{
		const Segment<TransactionKernel> segment(
			SegmentIdentifier(11, 3), {}, {}, {}, {}, SegmentProof::Empty());
		const SegmentResponseMessage message = SegmentResponseMessage::CreateKernels(MakeHash(9), segment);

		ByteBuffer body(BodyOf(message));
		const SegmentResponseMessage deserialized =
			SegmentResponseMessage::Deserialize(body, MessageTypes::KernelSegment);
		REQUIRE(std::holds_alternative<Segment<TransactionKernel>>(deserialized.GetPayload()));
		REQUIRE(body.GetRemainingSize() == 0);
	}
}

TEST_CASE("SegmentResponseMessage::RejectsWrongType")
{
	ByteBuffer body(std::vector<uint8_t>(32, 0));
	REQUIRE_THROWS_AS(
		SegmentResponseMessage::Deserialize(body, MessageTypes::Ping), DeserializationException);
}

namespace
{
	Hash LeafHash(const TestLeaf& leaf, const uint64_t pos0)
	{
		Serializer serializer;
		leaf.Serialize(serializer);
		return MMRHashUtil::HashLeafWithIndex(serializer.GetBytes(), pos0);
	}

	// A complete 4-leaf MMR (size 7):
	//   0,1 leaves -> parent 2; 3,4 leaves -> parent 5; root 6 = parent(2,5).
	struct TestMmr7
	{
		Hash Leaf0;
		Hash Leaf1;
		Hash Leaf2;
		Hash Leaf3;
		Hash Node2;
		Hash Node5;
		Hash Root;

		TestMmr7()
		{
			Leaf0 = LeafHash(TestLeaf{ 10 }, 0);
			Leaf1 = LeafHash(TestLeaf{ 20 }, 1);
			Leaf2 = LeafHash(TestLeaf{ 30 }, 3);
			Leaf3 = LeafHash(TestLeaf{ 40 }, 4);
			Node2 = MMRHashUtil::HashParentWithIndex(Leaf0, Leaf1, 2);
			Node5 = MMRHashUtil::HashParentWithIndex(Leaf2, Leaf3, 5);
			Root = MMRHashUtil::HashParentWithIndex(Node2, Node5, 6);
		}
	};
}

TEST_CASE("Segment::RootAndValidateFullSegment")
{
	const TestMmr7 mmr;
	const Segment<TestLeaf> segment(
		SegmentIdentifier(2, 0),
		{},
		{},
		{ 0, 1, 3, 4 },
		{ TestLeaf{ 10 }, TestLeaf{ 20 }, TestLeaf{ 30 }, TestLeaf{ 40 } },
		SegmentProof::Empty()
	);

	REQUIRE(segment.Root(7) == mmr.Root);

	const std::pair<Hash, uint64_t> parent = segment.FirstUnprunedParent(7);
	REQUIRE(parent.first == mmr.Root);
	REQUIRE(parent.second == 7);

	REQUIRE_NOTHROW(segment.Validate(7, mmr.Root));
}

TEST_CASE("Segment::ValidateRejectsTampering")
{
	const TestMmr7 mmr;
	const Segment<TestLeaf> segment(
		SegmentIdentifier(2, 0),
		{},
		{},
		{ 0, 1, 3, 4 },
		{ TestLeaf{ 10 }, TestLeaf{ 20 }, TestLeaf{ 30 }, TestLeaf{ 41 } }, // tampered
		SegmentProof::Empty()
	);

	REQUIRE(segment.Root(7) != mmr.Root);
	REQUIRE_THROWS_AS(segment.Validate(7, mmr.Root), SegmentException);

	// A correct segment against the wrong root must fail as well.
	const Segment<TestLeaf> good(
		SegmentIdentifier(2, 0),
		{},
		{},
		{ 0, 1, 3, 4 },
		{ TestLeaf{ 10 }, TestLeaf{ 20 }, TestLeaf{ 30 }, TestLeaf{ 40 } },
		SegmentProof::Empty()
	);
	REQUIRE_THROWS_AS(good.Validate(7, mmr.Leaf0), SegmentException);
}

TEST_CASE("Segment::RootForLeftSubtreeWithRightSiblingProof")
{
	const TestMmr7 mmr;
	// Segment height 1, idx 0: leaves 0 and 1 (positions 0..2, full subtree).
	const Segment<TestLeaf> segment(
		SegmentIdentifier(1, 0),
		{},
		{},
		{ 0, 1 },
		{ TestLeaf{ 10 }, TestLeaf{ 20 } },
		SegmentProof(std::vector<Hash>{ mmr.Node5 })
	);

	REQUIRE(segment.Root(7) == mmr.Node2);
	REQUIRE_NOTHROW(segment.Validate(7, mmr.Root));

	// Proof order matters: without the sibling hash validation must fail.
	const Segment<TestLeaf> noProof(
		SegmentIdentifier(1, 0),
		{},
		{},
		{ 0, 1 },
		{ TestLeaf{ 10 }, TestLeaf{ 20 } },
		SegmentProof::Empty()
	);
	REQUIRE_THROWS_AS(noProof.Validate(7, mmr.Root), SegmentException);
}

TEST_CASE("Segment::RootForRightSubtreeWithLeftSiblingProof")
{
	const TestMmr7 mmr;
	// Segment height 1, idx 1: leaves 2 and 3 (positions 3..5).
	const Segment<TestLeaf> segment(
		SegmentIdentifier(1, 1),
		{},
		{},
		{ 3, 4 },
		{ TestLeaf{ 30 }, TestLeaf{ 40 } },
		SegmentProof(std::vector<Hash>{ mmr.Node2 })
	);

	REQUIRE(segment.Root(7) == mmr.Node5);
	REQUIRE_NOTHROW(segment.Validate(7, mmr.Root));
}

TEST_CASE("BitmapSegment::ToSegmentAndValidate")
{
	// Bitmap MMR over two chunks: leaves at positions 0 and 1, root at 2.
	const BitmapChunk chunk0;
	BitmapChunk chunk1;
	chunk1.Set(5, true);

	const Hash hash0 = MMRHashUtil::HashLeafWithIndex(chunk0.GetBytes(), 0);
	const Hash hash1 = MMRHashUtil::HashLeafWithIndex(chunk1.GetBytes(), 1);
	const Hash root = MMRHashUtil::HashParentWithIndex(hash0, hash1, 2);

	std::vector<BitmapBlock> blocks;
	blocks.push_back(BitmapBlock(1));
	blocks.back().Set(5, true);
	const BitmapSegment bitmapSegment(
		SegmentIdentifier(0, 1), std::move(blocks), SegmentProof(std::vector<Hash>{ hash0 }));

	const Segment<BitmapChunk> segment = bitmapSegment.ToSegment();
	REQUIRE(segment.GetLeafPositions() == std::vector<uint64_t>{ 1 });
	REQUIRE(segment.GetLeafData().size() == 1);
	REQUIRE(segment.GetLeafData()[0].Get(5));
	REQUIRE(segment.Root(3) == hash1);
	REQUIRE_NOTHROW(segment.Validate(3, root));

	// Same segment without the sibling proof cannot validate.
	std::vector<BitmapBlock> blocksNoProof;
	blocksNoProof.push_back(BitmapBlock(1));
	blocksNoProof.back().Set(5, true);
	const BitmapSegment noProof(
		SegmentIdentifier(0, 1), std::move(blocksNoProof), SegmentProof::Empty());
	REQUIRE_THROWS_AS(noProof.ToSegment().Validate(3, root), SegmentException);
}

TEST_CASE("SegmentIdentifier::LeafOffsetOverflow")
{
	// height 32 => capacity 2^32; an index above 2^32-1 cannot be
	// multiplied safely and must be rejected before scheduling work.
	const SegmentIdentifier overflow((uint8_t)32, (uint64_t)1 << 32);
	REQUIRE_THROWS_AS(overflow.LeafOffset(), DeserializationException);

	const SegmentIdentifier valid((uint8_t)32, ((uint64_t)1 << 32) - 1);
	REQUIRE_NOTHROW(valid.LeafOffset());
}

TEST_CASE("Pihd::MessagesRoundtrip")
{
	const SegmentIdentifier id((uint8_t)9, 7);
	const GetHeaderSegmentMessage request(id);
	const std::vector<uint8_t> requestBody = BodyOf(request);
	ByteBuffer requestBuffer(requestBody, EProtocolVersion::V1);
	const GetHeaderSegmentMessage decodedRequest = GetHeaderSegmentMessage::Deserialize(requestBuffer);
	REQUIRE(decodedRequest.GetIdentifier() == id);
	REQUIRE(requestBuffer.GetRemainingSize() == 0);

	const HeaderSegmentMessage response(id, std::vector<BlockHeader>());
	const std::vector<uint8_t> responseBody = BodyOf(response);
	ByteBuffer responseBuffer(responseBody, EProtocolVersion::V1);
	const HeaderSegmentMessage decodedResponse = HeaderSegmentMessage::Deserialize(responseBuffer);
	REQUIRE(decodedResponse.GetIdentifier() == id);
	REQUIRE(decodedResponse.GetHeaders().empty());
	REQUIRE(responseBuffer.GetRemainingSize() == 0);
}
