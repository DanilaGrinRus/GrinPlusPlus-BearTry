#include <catch.hpp>

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Crypto/Models/Hash.h>
#include <Crypto/Models/RangeProof.h>

#include <P2P/Messages/MessageTypes.h>
#include <P2P/Messages/SegmentRequestMessage.h>
#include <P2P/Messages/SegmentResponseMessage.h>
#include <P2P/PIBD/BitmapSegmentAssembler.h>
#include <P2P/PIBD/PibdParams.h>
#include <PMMR/Common/MMRUtil.h>
#include <PMMR/Models/BitmapBlock.h>
#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/Segment.h>
#include <PMMR/Models/SegmentIdentifier.h>
#include <PMMR/Models/SegmentProof.h>
#include <PMMR/Models/SegmentException.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// Byte-exact PIBD wire tests.
//
// Every expected byte string below is derived from the Rust Grin 5.5.2
// serialization code (all integers are big-endian):
//   * core/src/core/pmmr/segment.rs  - SegmentIdentifier, Segment<T>, SegmentProof
//   * chain/src/txhashset/bitmap_accumulator.rs - BitmapChunk/Block/Segment
//   * core/src/core/transaction.rs   - TxKernel / KernelFeatures (v2 wire)
//   * core/src/ser.rs                - RangeProof, OutputIdentifier
//   * p2p/src/msg.rs                 - SegmentRequest / *_SegmentResponse bodies
// The upstream C++ models are the shared PIBD code (GrinPlusPlus master
// fb9a13b7) that this tree is built on; these tests pin that the models did
// not drift from the Rust reference during the C463/API/FastInit merge.
namespace
{
	Hash FillHash(const uint8_t value)
	{
		return Hash(std::vector<uint8_t>(32, value));
	}

	Commitment FillCommitment(const uint8_t value)
	{
		return Commitment(CBigInteger<33>(std::vector<uint8_t>(33, value)));
	}

	Signature FillSignature(const uint8_t value)
	{
		return Signature(CBigInteger<64>(std::vector<uint8_t>(64, value)));
	}

	void AppendU16(std::vector<uint8_t>& bytes, const uint16_t value)
	{
		bytes.push_back((uint8_t)(value >> 8));
		bytes.push_back((uint8_t)value);
	}

	void AppendU64(std::vector<uint8_t>& bytes, const uint64_t value)
	{
		for (int shift = 56; shift >= 0; shift -= 8)
		{
			bytes.push_back((uint8_t)(value >> shift));
		}
	}

	void AppendHash(std::vector<uint8_t>& bytes, const Hash& hash)
	{
		const std::vector<unsigned char>& data = hash.GetData();
		bytes.insert(bytes.end(), data.cbegin(), data.cend());
	}

	std::vector<uint8_t> FrameBody(const IMessage& message, const EProtocolVersion version)
	{
		const std::vector<uint8_t> full = message.Serialize(version);
		REQUIRE(full.size() >= 11);

		// magic(2) | type(1) | length(8) | body
		REQUIRE(full[2] == (uint8_t)message.GetMessageType());
		const std::vector<uint8_t> lengthBytes(full.cbegin() + 3, full.cbegin() + 11);
		ByteBuffer lengthBuffer(lengthBytes);
		REQUIRE(lengthBuffer.ReadU64() == (uint64_t)(full.size() - 11));

		return std::vector<uint8_t>(full.cbegin() + 11, full.cend());
	}

	std::vector<uint8_t> KernelLeaf(
		const EKernelFeatures features,
		const uint64_t fee,
		const uint64_t lockHeight,
		const uint8_t excessFill,
		const uint8_t signatureFill)
	{
		std::vector<uint8_t> leaf;
		leaf.push_back((uint8_t)features);

		if (features == EKernelFeatures::DEFAULT_KERNEL
			|| features == EKernelFeatures::HEIGHT_LOCKED
			|| features == EKernelFeatures::NO_RECENT_DUPLICATE)
		{
			// fee_fields: (shift << 40) | fee, big-endian u64.
			AppendU64(leaf, fee & 0xFFFFFFFFFFULL);
		}

		if (features == EKernelFeatures::HEIGHT_LOCKED)
		{
			AppendU64(leaf, lockHeight);
		}
		else if (features == EKernelFeatures::NO_RECENT_DUPLICATE)
		{
			AppendU16(leaf, (uint16_t)lockHeight);
		}

		leaf.insert(leaf.end(), 33, excessFill);
		leaf.insert(leaf.end(), 64, signatureFill);
		return leaf;
	}

	// Grin v1 kernel-feature layout (the layout used for kernel hashing):
	// Plain/Coinbase carry 8/16 zero "empty" bytes, NRD pads relative height
	// with six zero bytes. HeightLocked is identical in v1 and v2.
	std::vector<uint8_t> KernelLeafV1(
		const EKernelFeatures features,
		const uint64_t fee,
		const uint64_t lockHeight,
		const uint8_t excessFill,
		const uint8_t signatureFill)
	{
		std::vector<uint8_t> leaf;
		leaf.push_back((uint8_t)features);

		if (features == EKernelFeatures::DEFAULT_KERNEL)
		{
			AppendU64(leaf, fee & 0xFFFFFFFFFFULL);
			leaf.insert(leaf.end(), 8, 0x00);
		}
		else if (features == EKernelFeatures::COINBASE_KERNEL)
		{
			leaf.insert(leaf.end(), 16, 0x00);
		}
		else if (features == EKernelFeatures::HEIGHT_LOCKED)
		{
			AppendU64(leaf, fee & 0xFFFFFFFFFFULL);
			AppendU64(leaf, lockHeight);
		}
		else
		{
			AppendU64(leaf, fee & 0xFFFFFFFFFFULL);
			leaf.insert(leaf.end(), 6, 0x00);
			AppendU16(leaf, (uint16_t)lockHeight);
		}

		leaf.insert(leaf.end(), 33, excessFill);
		leaf.insert(leaf.end(), 64, signatureFill);
		return leaf;
	}
}

// ---------------------------------------------------------------------------
// SegmentIdentifier and SegmentProof
// ---------------------------------------------------------------------------

TEST_CASE("PibdWireAudit::SegmentIdentifierBytesMatchRust")
{
	const SegmentIdentifier identifier((uint8_t)11, 0x0102030405060708ULL);

	const std::vector<uint8_t> expected = {
		0x0B,
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
	};

	REQUIRE(identifier.Serialized() == expected);

	ByteBuffer buffer(identifier.Serialized());
	const SegmentIdentifier decoded = SegmentIdentifier::Deserialize(buffer);
	REQUIRE(decoded.GetHeight() == 11);
	REQUIRE(decoded.GetIdx() == 0x0102030405060708ULL);
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdWireAudit::SegmentProofBytesMatchRust")
{
	std::vector<Hash> hashes{ FillHash(0x11), FillHash(0x22) };
	const SegmentProof proof(std::move(hashes));

	std::vector<uint8_t> expected;
	AppendU64(expected, 2);
	expected.insert(expected.end(), 32, 0x11);
	expected.insert(expected.end(), 32, 0x22);

	REQUIRE(proof.Serialized() == expected);

	ByteBuffer buffer(expected);
	const SegmentProof decoded = SegmentProof::Deserialize(buffer);
	REQUIRE(decoded.Size() == 2);
	REQUIRE(decoded.GetHashes()[0] == FillHash(0x11));
	REQUIRE(buffer.GetRemainingSize() == 0);
}

// ---------------------------------------------------------------------------
// BitmapChunk / BitmapBlock
// ---------------------------------------------------------------------------

TEST_CASE("PibdWireAudit::BitmapChunkIsRawMsbFirstBytes")
{
	BitmapChunk chunk;
	chunk.Set(0, true);
	chunk.Set(9, true);
	chunk.Set(1023, true);

	// Rust BitmapChunk::write emits BitVec::to_bytes(): 128 raw bytes, bit 0 is
	// the most significant bit of byte 0.
	std::vector<uint8_t> expected(128, 0x00);
	expected[0] = 0x80;
	expected[1] = 0x40;
	expected[127] = 0x01;

	REQUIRE(chunk.GetBytes() == expected);

	Serializer chunkSerializer;
	chunk.Serialize(chunkSerializer);
	REQUIRE(chunkSerializer.GetBytes() == expected);

	ByteBuffer buffer(expected);
	const BitmapChunk decoded = BitmapChunk::Deserialize(buffer);
	REQUIRE(decoded.Get(0));
	REQUIRE(decoded.Get(9));
	REQUIRE(decoded.Get(1023));
	REQUIRE(!decoded.Get(1));
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdWireAudit::BitmapBlockPositiveModeBytes")
{
	BitmapBlock block(1);
	block.Set(0, true);
	block.Set(9, true);

	// u8 n_chunks, u8 mode(1 = Positive), u16 count, u16 indices.
	const std::vector<uint8_t> expected = {
		0x01, 0x01,
		0x00, 0x02,
		0x00, 0x00,
		0x00, 0x09
	};

	REQUIRE(block.Serialized() == expected);

	ByteBuffer buffer(expected);
	const BitmapBlock decoded = BitmapBlock::Deserialize(buffer);
	REQUIRE(decoded.GetNumChunks() == 1);
	REQUIRE(decoded.Get(0));
	REQUIRE(decoded.Get(9));
	REQUIRE(!decoded.Get(1));
}

TEST_CASE("PibdWireAudit::BitmapBlockEmptyChunkIsPositiveZero")
{
	// Rust threshold is BitmapBlock::NBITS / 16 = 4096, independent of the block
	// length. An empty block therefore always uses Positive mode with count 0.
	BitmapBlock block(1);

	const std::vector<uint8_t> expected = {
		0x01, 0x01, 0x00, 0x00
	};

	REQUIRE(block.Serialized() == expected);
}

TEST_CASE("PibdWireAudit::BitmapBlockNegativeModeBytes")
{
	BitmapBlock block(64);
	for (size_t i = 0; i < block.GetNumBits(); ++i)
	{
		block.Set(i, true);
	}
	block.Set(65535, false);

	// u8 n_chunks(64), u8 mode(2 = Negative), u16 count(1), u16 index(65535).
	const std::vector<uint8_t> expected = {
		0x40, 0x02,
		0x00, 0x01,
		0xFF, 0xFF
	};

	REQUIRE(block.Serialized() == expected);

	ByteBuffer buffer(expected);
	const BitmapBlock decoded = BitmapBlock::Deserialize(buffer);
	REQUIRE(decoded.GetNumChunks() == 64);
	REQUIRE(decoded.Get(0));
	REQUIRE(!decoded.Get(65535));
}

TEST_CASE("PibdWireAudit::BitmapBlockRawThresholdBytes")
{
	// Exactly 4096 set bits: count_pos >= threshold and count_neg >= threshold,
	// so Rust selects Raw. Total bits = 65536, bytes = 8192.
	BitmapBlock block(64);
	for (size_t i = 0; i < 4096; ++i)
	{
		block.Set(i, true);
	}

	const std::vector<uint8_t> bytes = block.Serialized();
	REQUIRE(bytes.size() == 2 + 8192);
	REQUIRE(bytes[0] == 0x40);
	REQUIRE(bytes[1] == 0x00); // Raw
	// Bits 0..4095 are set => the first 512 bytes are 0xFF.
	for (size_t i = 0; i < 512; ++i)
	{
		REQUIRE(bytes[2 + i] == 0xFF);
	}
	for (size_t i = 514; i < bytes.size(); ++i)
	{
		REQUIRE(bytes[i] == 0x00);
	}

	ByteBuffer buffer(bytes);
	const BitmapBlock decoded = BitmapBlock::Deserialize(buffer);
	REQUIRE(decoded.Get(0));
	REQUIRE(decoded.Get(4095));
	REQUIRE(!decoded.Get(4096));
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdWireAudit::BitmapBlockPositiveBoundaryBytes")
{
	// 4095 set bits: count_pos < 4096, so Positive mode, count = 0x0FFF.
	BitmapBlock block(64);
	for (size_t i = 0; i < 4095; ++i)
	{
		block.Set(i, true);
	}

	const std::vector<uint8_t> bytes = block.Serialized();
	REQUIRE(bytes.size() == 2 + 2 + (4095 * 2));
	REQUIRE(bytes[0] == 0x40);
	REQUIRE(bytes[1] == 0x01); // Positive
	REQUIRE(bytes[2] == 0x0F);
	REQUIRE(bytes[3] == 0xFF);
	// First index is 0, last index is 4094 (0x0FFE).
	REQUIRE(bytes[4] == 0x00);
	REQUIRE(bytes[5] == 0x00);
	REQUIRE(bytes[bytes.size() - 2] == 0x0F);
	REQUIRE(bytes[bytes.size() - 1] == 0xFE);

	ByteBuffer buffer(bytes);
	const BitmapBlock decoded = BitmapBlock::Deserialize(buffer);
	REQUIRE(decoded.Get(0));
	REQUIRE(decoded.Get(4094));
	REQUIRE(!decoded.Get(4095));
}

// ---------------------------------------------------------------------------
// BitmapSegment
// ---------------------------------------------------------------------------

TEST_CASE("PibdWireAudit::BitmapSegmentBytesMatchRust")
{
	std::vector<BitmapBlock> blocks;
	blocks.push_back(BitmapBlock(1));
	blocks.back().Set(0, true);

	const BitmapSegment segment(
		SegmentIdentifier((uint8_t)9, 0),
		std::move(blocks),
		SegmentProof::Empty());

	std::vector<uint8_t> expected;
	expected.push_back(0x09);
	AppendU64(expected, 0);          // idx
	AppendU16(expected, 1);          // n_blocks
	expected.push_back(0x01);        // n_chunks
	expected.push_back(0x01);        // Positive
	AppendU16(expected, 1);          // count
	AppendU16(expected, 0);          // index 0
	AppendU64(expected, 0);          // proof hash count

	REQUIRE(expected.size() == 25);
	REQUIRE(segment.Serialized() == expected);

	ByteBuffer buffer(expected);
	const BitmapSegment decoded = BitmapSegment::Deserialize(buffer);
	REQUIRE(decoded.GetIdentifier() == SegmentIdentifier((uint8_t)9, 0));
	REQUIRE(decoded.GetBlocks().size() == 1);
	REQUIRE(decoded.GetNumChunks() == 1);
	REQUIRE(decoded.ToSegment().GetLeafPositions() == std::vector<uint64_t>{ 0 });
	REQUIRE(decoded.ToSegment().GetLeafData()[0].Get(0));
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdWireAudit::BitmapSegmentMaxLeafOffsetBoundary")
{
	// Rust validate_blocks accepts height 13, idx 2^51-1: leaf_offset is the
	// largest representable multiple of 2^13 (2^64-2^13) and
	// leaf_offset + n_chunks - 1 == 2^64-1 does not overflow.
	const uint64_t idx = (1ULL << 51) - 1;

	std::vector<uint8_t> bytes;
	bytes.push_back(13);
	AppendU64(bytes, idx);
	AppendU16(bytes, 128);
	for (int block = 0; block < 128; ++block)
	{
		// 64 chunks, all bits set => Negative mode with count 0 (4 bytes).
		bytes.push_back(0x40);
		bytes.push_back(0x02);
		AppendU16(bytes, 0);
	}
	AppendU64(bytes, 0);

	ByteBuffer buffer(bytes);
	REQUIRE_NOTHROW(BitmapSegment::Deserialize(buffer));
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdWireAudit::BitmapSegmentRejectsLeafOffsetOverflow")
{
	// height 13, idx 2^51 => idx * 2^13 == 2^64, checked multiply must fail.
	// Rust: BitmapSegment::leaf_offset() returns TooLargeReadErr.
	std::vector<uint8_t> bytes;
	bytes.push_back(13);
	AppendU64(bytes, 1ULL << 51);
	AppendU16(bytes, 1);
	bytes.push_back(0x01); // 1 chunk
	bytes.push_back(0x01); // Positive
	AppendU16(bytes, 0);
	AppendU64(bytes, 0);

	ByteBuffer buffer(bytes);
	REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
}

TEST_CASE("PibdWireAudit::BitmapSegmentRejectsMalformedShapes")
{
	const auto withHeader = [](const uint8_t height, const uint64_t idx, const uint16_t blocks)
	{
		std::vector<uint8_t> bytes;
		bytes.push_back(height);
		AppendU64(bytes, idx);
		AppendU16(bytes, blocks);
		return bytes;
	};

	// Zero blocks.
	{
		const std::vector<uint8_t> bytes = withHeader(9, 0, 0);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
	}

	// More blocks than the height allows (height 9 => 8 blocks max).
	{
		std::vector<uint8_t> bytes = withHeader(9, 0, 9);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
	}

	// Height above the Rust maximum of 13.
	{
		std::vector<uint8_t> bytes = withHeader(14, 0, 1);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
	}

	// Block with 65 chunks.
	{
		std::vector<uint8_t> bytes = withHeader(9, 0, 1);
		bytes.push_back(65);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
	}

	// Last block with zero chunks.
	{
		std::vector<uint8_t> bytes = withHeader(9, 0, 1);
		bytes.push_back(0x00); // n_chunks
		bytes.push_back(0x01); // mode
		AppendU16(bytes, 0);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
	}

	// Non-final block that is not full (64 chunks).
	{
		std::vector<uint8_t> bytes = withHeader(9, 0, 2);
		bytes.push_back(0x01); // 1 chunk, not full
		bytes.push_back(0x01);
		AppendU16(bytes, 0);
		bytes.push_back(0x01);
		bytes.push_back(0x01);
		AppendU16(bytes, 0);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
	}

	// Positive index equal to n_bits.
	{
		std::vector<uint8_t> bytes = withHeader(9, 0, 1);
		bytes.push_back(0x01); // 1 chunk = 1024 bits
		bytes.push_back(0x01); // Positive
		AppendU16(bytes, 1);
		AppendU16(bytes, 1024);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
	}

	// Unknown serialization mode.
	{
		std::vector<uint8_t> bytes = withHeader(9, 0, 1);
		bytes.push_back(0x01);
		bytes.push_back(0x03);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
	}
}

TEST_CASE("PibdWireAudit::BitmapSegmentTruncationRejected")
{
	std::vector<BitmapBlock> blocks;
	blocks.push_back(BitmapBlock(1));
	blocks.back().Set(7, true);
	const std::vector<uint8_t> body = BitmapSegment(
		SegmentIdentifier((uint8_t)9, 0), std::move(blocks), SegmentProof(std::vector<Hash>{ FillHash(0x33) })).Serialized();

	REQUIRE(body.size() > 40);
	for (size_t cut = 0; cut < body.size(); ++cut)
	{
		ByteBuffer buffer(std::vector<uint8_t>(body.cbegin(), body.cbegin() + cut));
		REQUIRE_THROWS_AS(BitmapSegment::Deserialize(buffer), DeserializationException);
	}
}

// ---------------------------------------------------------------------------
// Segment<T> leaves: OutputIdentifier / RangeProof / TransactionKernel
// ---------------------------------------------------------------------------

TEST_CASE("PibdWireAudit::OutputSegmentBytesMatchRust")
{
	Segment<OutputIdentifier> segment(
		SegmentIdentifier((uint8_t)11, 2),
		{ 4 },
		{ FillHash(0x11) },
		{ 0, 3 },
		{
			OutputIdentifier(EOutputFeatures::DEFAULT, FillCommitment(0x22)),
			OutputIdentifier(EOutputFeatures::COINBASE_OUTPUT, FillCommitment(0x33))
		},
		SegmentProof(std::vector<Hash>{ FillHash(0x44) }));

	std::vector<uint8_t> expected;
	expected.push_back(0x0B);
	AppendU64(expected, 2);
	AppendU64(expected, 1);                 // one hash position
	AppendU64(expected, 5);                 // pos0 4 => wire 5
	expected.insert(expected.end(), 32, 0x11);
	AppendU64(expected, 2);                 // two leaf positions
	AppendU64(expected, 1);                 // pos0 0 => wire 1
	AppendU64(expected, 4);                 // pos0 3 => wire 4
	expected.push_back(0x00);               // DEFAULT output features
	expected.insert(expected.end(), 33, 0x22);
	expected.push_back(0x01);               // COINBASE_OUTPUT
	expected.insert(expected.end(), 33, 0x33);
	AppendU64(expected, 1);                 // proof: one hash
	expected.insert(expected.end(), 32, 0x44);

	REQUIRE(expected.size() == 189);
	REQUIRE(segment.Serialized(EProtocolVersion::V1000) == expected);

	ByteBuffer buffer(expected, EProtocolVersion::V1000);
	const Segment<OutputIdentifier> decoded = Segment<OutputIdentifier>::Deserialize(buffer);
	REQUIRE(decoded.GetHashPositions() == std::vector<uint64_t>{ 4 });
	REQUIRE(decoded.GetLeafPositions() == std::vector<uint64_t>{ 0, 3 });
	REQUIRE(decoded.GetLeafData()[0] == OutputIdentifier(EOutputFeatures::DEFAULT, FillCommitment(0x22)));
	REQUIRE(decoded.GetLeafData()[1] == OutputIdentifier(EOutputFeatures::COINBASE_OUTPUT, FillCommitment(0x33)));
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdWireAudit::RangeProofSegmentBytesMatchRust")
{
	std::vector<unsigned char> proof(675);
	for (size_t i = 0; i < proof.size(); ++i)
	{
		proof[i] = (unsigned char)(i & 0xFF);
	}

	Segment<RangeProof> segment(
		SegmentIdentifier((uint8_t)11, 0),
		{},
		{},
		{ 0 },
		{ RangeProof(std::move(proof)) },
		SegmentProof::Empty());

	std::vector<uint8_t> expected;
	expected.push_back(0x0B);
	AppendU64(expected, 0);
	AppendU64(expected, 0);                 // no hash positions
	AppendU64(expected, 1);                 // one leaf position
	AppendU64(expected, 1);                 // pos0 0 => wire 1
	AppendU64(expected, 675);               // Rust write_bytes length prefix
	for (size_t i = 0; i < 675; ++i)
	{
		expected.push_back((uint8_t)(i & 0xFF));
	}
	AppendU64(expected, 0);                 // proof count

	REQUIRE(expected.size() == 724);
	REQUIRE(segment.Serialized(EProtocolVersion::V1000) == expected);

	ByteBuffer buffer(expected, EProtocolVersion::V1000);
	const Segment<RangeProof> decoded = Segment<RangeProof>::Deserialize(buffer);
	REQUIRE(decoded.GetLeafPositions() == std::vector<uint64_t>{ 0 });
	REQUIRE(decoded.GetLeafData()[0].GetProofBytes().size() == 675);
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdWireAudit::RangeProofSizesRejectedAndTolerated")
{
	// Grin 5.5.2 RangeProof::read takes min(len, MAX_PROOF_SIZE); a byte above
	// 675 cannot be represented by the C++ model and must be refused rather
	// than silently truncated.
	const auto segmentWithProofSize = [](const uint64_t declaredSize, const size_t actualBytes)
	{
		std::vector<uint8_t> bytes;
		bytes.push_back(0x0B);
		AppendU64(bytes, 0);
		AppendU64(bytes, 0);
		AppendU64(bytes, 1);
		AppendU64(bytes, 1);
		AppendU64(bytes, declaredSize);
		bytes.insert(bytes.end(), actualBytes, 0x00);
		AppendU64(bytes, 0);               // proof count
		return bytes;
	};

	// 676 declared bytes: rejected (Rust caps the read and the rest of the
	// segment is then malformed).
	{
		const std::vector<uint8_t> bytes = segmentWithProofSize(676, 676);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(Segment<RangeProof>::Deserialize(buffer), DeserializationException);
	}

	// UINT64_MAX declared: rejected by the size limit before any read.
	{
		const std::vector<uint8_t> bytes = segmentWithProofSize((std::numeric_limits<uint64_t>::max)(), 0);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(Segment<RangeProof>::Deserialize(buffer), DeserializationException);
	}

	// A short (1 byte) proof is accepted by both implementations; Rust pads it
	// to MAX_PROOF_SIZE when hashing, so a C++ node cannot authenticate a
	// segment built from such a proof. Canonical Grin nodes only ever emit
	// 675-byte proofs; this test pins the current (documented) tolerance.
	{
		const std::vector<uint8_t> bytes = segmentWithProofSize(1, 1);
		ByteBuffer buffer(bytes);
		REQUIRE_NOTHROW(Segment<RangeProof>::Deserialize(buffer));
	}
}

TEST_CASE("PibdWireAudit::KernelSegmentV2BytesMatchRust")
{
	const std::vector<uint8_t> plainLeaf = KernelLeaf(EKernelFeatures::DEFAULT_KERNEL, 0x0102030405ULL, 0, 0xAA, 0xBB);
	const std::vector<uint8_t> heightLeaf = KernelLeaf(EKernelFeatures::HEIGHT_LOCKED, 0x0102030405ULL, 0x1122334455667788ULL, 0xAA, 0xBB);
	const std::vector<uint8_t> coinbaseLeaf = KernelLeaf(EKernelFeatures::COINBASE_KERNEL, 0, 0, 0xAA, 0xBB);
	const std::vector<uint8_t> nrdLeaf = KernelLeaf(EKernelFeatures::NO_RECENT_DUPLICATE, 0x0102030405ULL, 0x1234ULL, 0xAA, 0xBB);

	const auto expectSegment = [](const std::vector<uint8_t>& expectedLeaf, const TransactionKernel& kernel)
	{
		const Segment<TransactionKernel> segment(
			SegmentIdentifier((uint8_t)11, 0),
			{},
			{},
			{ 0 },
			{ kernel },
			SegmentProof::Empty());

		std::vector<uint8_t> expected;
		expected.push_back(0x0B);
		AppendU64(expected, 0);
		AppendU64(expected, 0);
		AppendU64(expected, 1);
		AppendU64(expected, 1);
		expected.insert(expected.end(), expectedLeaf.cbegin(), expectedLeaf.cend());
		AppendU64(expected, 0);

		REQUIRE(segment.Serialized(EProtocolVersion::V1000) == expected);

		ByteBuffer buffer(expected, EProtocolVersion::V1000);
		const Segment<TransactionKernel> decoded = Segment<TransactionKernel>::Deserialize(buffer);
		REQUIRE(decoded.GetLeafData().size() == 1);
		REQUIRE(decoded.GetLeafData()[0].GetFeatures() == kernel.GetFeatures());
		REQUIRE(decoded.GetLeafData()[0].GetFee() == kernel.GetFee());
		REQUIRE(decoded.GetLeafData()[0].GetLockHeight() == kernel.GetLockHeight());
		REQUIRE(buffer.GetRemainingSize() == 0);
	};

	// Plain: tag 0, fee_fields u64, excess 33, signature 64.
	expectSegment(plainLeaf, TransactionKernel(
		EKernelFeatures::DEFAULT_KERNEL, Fee(0, 0x0102030405ULL), 0,
		FillCommitment(0xAA), FillSignature(0xBB)));

	// Height locked: tag 2, fee u64, lock_height u64.
	expectSegment(heightLeaf, TransactionKernel(
		EKernelFeatures::HEIGHT_LOCKED, Fee(0, 0x0102030405ULL), 0x1122334455667788ULL,
		FillCommitment(0xAA), FillSignature(0xBB)));

	// Coinbase: tag 1, no fee, no lock.
	expectSegment(coinbaseLeaf, TransactionKernel(
		EKernelFeatures::COINBASE_KERNEL, Fee(), 0,
		FillCommitment(0xAA), FillSignature(0xBB)));

	// NRD: tag 3, fee u64, relative height u16.
	expectSegment(nrdLeaf, TransactionKernel(
		EKernelFeatures::NO_RECENT_DUPLICATE, Fee(0, 0x0102030405ULL), 0x1234ULL,
		FillCommitment(0xAA), FillSignature(0xBB)));
}

TEST_CASE("PibdWireAudit::KernelLeafV1MatchesRustHashLayout")
{
	// Grin hashes kernels with the v1 kernel-feature layout even when the wire
	// uses v2, and the kernel PMMR leaf hash is over that v1 encoding. C++ must
	// therefore reproduce both layouts byte-for-byte.
	const TransactionKernel plain(
		EKernelFeatures::DEFAULT_KERNEL, Fee(0, 0x0102030405ULL), 0,
		FillCommitment(0xAA), FillSignature(0xBB));
	const TransactionKernel height(
		EKernelFeatures::HEIGHT_LOCKED, Fee(0, 0x0102030405ULL), 0x1122334455667788ULL,
		FillCommitment(0xAA), FillSignature(0xBB));
	const TransactionKernel coinbase(
		EKernelFeatures::COINBASE_KERNEL, Fee(), 0,
		FillCommitment(0xAA), FillSignature(0xBB));
	const TransactionKernel nrd(
		EKernelFeatures::NO_RECENT_DUPLICATE, Fee(0, 0x0102030405ULL), 0x1234ULL,
		FillCommitment(0xAA), FillSignature(0xBB));

	const auto check = [](const TransactionKernel& kernel, const std::vector<uint8_t>& v1Expected, const std::vector<uint8_t>& v2Expected)
	{
		Serializer v1;
		kernel.Serialize(v1);
		REQUIRE(v1.GetBytes() == v1Expected);

		Serializer v2(EProtocolVersion::V1000);
		kernel.Serialize(v2);
		REQUIRE(v2.GetBytes() == v2Expected);
	};

	check(plain,
		KernelLeafV1(EKernelFeatures::DEFAULT_KERNEL, 0x0102030405ULL, 0, 0xAA, 0xBB),
		KernelLeaf(EKernelFeatures::DEFAULT_KERNEL, 0x0102030405ULL, 0, 0xAA, 0xBB));
	check(height,
		KernelLeafV1(EKernelFeatures::HEIGHT_LOCKED, 0x0102030405ULL, 0x1122334455667788ULL, 0xAA, 0xBB),
		KernelLeaf(EKernelFeatures::HEIGHT_LOCKED, 0x0102030405ULL, 0x1122334455667788ULL, 0xAA, 0xBB));
	check(coinbase,
		KernelLeafV1(EKernelFeatures::COINBASE_KERNEL, 0, 0, 0xAA, 0xBB),
		KernelLeaf(EKernelFeatures::COINBASE_KERNEL, 0, 0, 0xAA, 0xBB));
	check(nrd,
		KernelLeafV1(EKernelFeatures::NO_RECENT_DUPLICATE, 0x0102030405ULL, 0x1234ULL, 0xAA, 0xBB),
		KernelLeaf(EKernelFeatures::NO_RECENT_DUPLICATE, 0x0102030405ULL, 0x1234ULL, 0xAA, 0xBB));

	// v1 plain is 8 bytes longer than v2 (the unused lock/empty field).
	Serializer plainV1;
	plain.Serialize(plainV1);
	Serializer plainV2(EProtocolVersion::V1000);
	plain.Serialize(plainV2);
	REQUIRE(plainV1.GetBytes().size() == plainV2.GetBytes().size() + 8);
	REQUIRE(plainV1.GetBytes().size() == 1 + 8 + 8 + 33 + 64);
}

TEST_CASE("PibdWireAudit::SegmentLeafTypesRejectWrongDiscriminants")
{
	// OutputIdentifier: only 0 (plain) and 1 (coinbase) exist.
	{
		std::vector<uint8_t> bytes;
		bytes.push_back(0x0B);
		AppendU64(bytes, 0);
		AppendU64(bytes, 0);
		AppendU64(bytes, 1);
		AppendU64(bytes, 1);
		bytes.push_back(0x02);             // invalid output feature
		bytes.insert(bytes.end(), 33, 0x00);
		AppendU64(bytes, 0);
		ByteBuffer buffer(bytes);
		REQUIRE_THROWS_AS(Segment<OutputIdentifier>::Deserialize(buffer), DeserializationException);
	}

	// TransactionKernel: only tags 0..3 exist.
	{
		std::vector<uint8_t> bytes;
		bytes.push_back(0x0B);
		AppendU64(bytes, 0);
		AppendU64(bytes, 0);
		AppendU64(bytes, 1);
		AppendU64(bytes, 1);
		bytes.push_back(0x04);             // invalid kernel feature
		bytes.insert(bytes.end(), 33 + 64, 0x00);
		AppendU64(bytes, 0);
		ByteBuffer buffer(bytes, EProtocolVersion::V1000);
		REQUIRE_THROWS_AS(Segment<TransactionKernel>::Deserialize(buffer), DeserializationException);
	}
}

TEST_CASE("PibdWireAudit::SegmentCountsAndPositionsRejected")
{
	// Count above Rust MAX_SEGMENT_READ_ITEMS (1_000_000).
	{
		Serializer serializer;
		serializer.Append<uint8_t>(9);
		serializer.Append<uint64_t>(0);
		serializer.Append<uint64_t>(1000001);
		ByteBuffer buffer(serializer.GetBytes());
		REQUIRE_THROWS_AS(Segment<OutputIdentifier>::Deserialize(buffer), DeserializationException);
	}

	// 1_000_000 leaves with no payload.
	{
		Serializer serializer;
		serializer.Append<uint8_t>(9);
		serializer.Append<uint64_t>(0);
		serializer.Append<uint64_t>(0);
		serializer.Append<uint64_t>(1000000);
		ByteBuffer buffer(serializer.GetBytes());
		REQUIRE_THROWS_AS(Segment<OutputIdentifier>::Deserialize(buffer), DeserializationException);
	}

	// Wire position 0 is not valid: positions are pos0 + 1 and must be > 0.
	{
		Serializer serializer;
		serializer.Append<uint8_t>(9);
		serializer.Append<uint64_t>(0);
		serializer.Append<uint64_t>(1);
		serializer.Append<uint64_t>(0);    // invalid 1-based position
		ByteBuffer buffer(serializer.GetBytes());
		REQUIRE_THROWS_AS(Segment<OutputIdentifier>::Deserialize(buffer), DeserializationException);
	}

	// Duplicate leaf positions are not strictly increasing.
	{
		Serializer serializer;
		serializer.Append<uint8_t>(9);
		serializer.Append<uint64_t>(0);
		serializer.Append<uint64_t>(0);
		serializer.Append<uint64_t>(2);
		serializer.Append<uint64_t>(5);
		serializer.Append<uint64_t>(5);
		ByteBuffer buffer(serializer.GetBytes());
		REQUIRE_THROWS_AS(Segment<OutputIdentifier>::Deserialize(buffer), DeserializationException);
	}
}

// ---------------------------------------------------------------------------
// Message bodies (p2p/src/msg.rs, *_SegmentResponse)
// ---------------------------------------------------------------------------

TEST_CASE("PibdWireAudit::RequestMessagesBytesMatchRust")
{
	const Hash blockHash = FillHash(0xAB);
	const SegmentIdentifier identifier((uint8_t)11, 2);

	std::vector<uint8_t> expectedBody;
	AppendHash(expectedBody, blockHash);
	expectedBody.push_back(0x0B);
	AppendU64(expectedBody, 2);

	const std::pair<MessageTypes::EMessageType, MessageTypes::EMessageType> types[] = {
		{ MessageTypes::GetOutputBitmapSegment, MessageTypes::OutputBitmapSegment },
		{ MessageTypes::GetOutputSegment, MessageTypes::OutputSegment },
		{ MessageTypes::GetRangeProofSegment, MessageTypes::RangeProofSegment },
		{ MessageTypes::GetKernelSegment, MessageTypes::KernelSegment }
	};

	for (const auto& type : types)
	{
		REQUIRE(MessageTypes::GetMaximumSize(type.first) == 41);

		const SegmentRequestMessage message(type.first, blockHash, identifier);
		const std::vector<uint8_t> body = FrameBody(message, EProtocolVersion::V1000);
		REQUIRE(body == expectedBody);

		ByteBuffer buffer(body, EProtocolVersion::V1000);
		const SegmentRequestMessage decoded = SegmentRequestMessage::Deserialize(buffer, type.first);
		REQUIRE(decoded.GetBlockHash() == blockHash);
		REQUIRE(decoded.GetIdentifier() == identifier);
		REQUIRE(buffer.GetRemainingSize() == 0);

		// Truncated request body: every cut before the full body must fail.
		for (size_t cut = 0; cut < body.size(); ++cut)
		{
			ByteBuffer cutBuffer(std::vector<uint8_t>(body.cbegin(), body.cbegin() + cut));
			REQUIRE_THROWS_AS(
				SegmentRequestMessage::Deserialize(cutBuffer, type.first),
				DeserializationException);
		}
	}
}

TEST_CASE("PibdWireAudit::OutputBitmapSegmentResponseBytesMatchRust")
{
	std::vector<BitmapBlock> blocks;
	blocks.push_back(BitmapBlock(1));
	blocks.back().Set(0, true);

	const BitmapSegment bitmapSegment(
		SegmentIdentifier((uint8_t)9, 0), std::move(blocks), SegmentProof::Empty());

	const SegmentResponseMessage message = SegmentResponseMessage::CreateOutputBitmap(
		FillHash(0xCD), bitmapSegment, FillHash(0xEF));

	std::vector<uint8_t> expected;
	AppendHash(expected, FillHash(0xCD));
	expected.push_back(0x09);
	AppendU64(expected, 0);
	AppendU16(expected, 1);
	expected.push_back(0x01);
	expected.push_back(0x01);
	AppendU16(expected, 1);
	AppendU16(expected, 0);
	AppendU64(expected, 0);
	AppendHash(expected, FillHash(0xEF));

	REQUIRE(expected.size() == 89);
	const std::vector<uint8_t> body = FrameBody(message, EProtocolVersion::V1000);
	REQUIRE(body == expected);

	ByteBuffer buffer(body, EProtocolVersion::V1000);
	const SegmentResponseMessage decoded =
		SegmentResponseMessage::Deserialize(buffer, MessageTypes::OutputBitmapSegment);
	REQUIRE(decoded.GetBlockHash() == FillHash(0xCD));
	REQUIRE(decoded.GetExtraRoot().has_value());
	REQUIRE(decoded.GetExtraRoot().value() == FillHash(0xEF));
	REQUIRE(std::holds_alternative<BitmapSegment>(decoded.GetPayload()));
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdWireAudit::OutputSegmentResponseBytesMatchRust")
{
	const Segment<OutputIdentifier> segment(
		SegmentIdentifier((uint8_t)11, 0),
		{},
		{},
		{ 0 },
		{ OutputIdentifier(EOutputFeatures::DEFAULT, FillCommitment(0x22)) },
		SegmentProof::Empty());

	const SegmentResponseMessage message = SegmentResponseMessage::CreateOutputs(
		FillHash(0xCD), segment, FillHash(0xEF));

	std::vector<uint8_t> expected;
	AppendHash(expected, FillHash(0xCD));
	expected.push_back(0x0B);
	AppendU64(expected, 0);
	AppendU64(expected, 0);          // hashes
	AppendU64(expected, 1);          // leaves
	AppendU64(expected, 1);          // leaf pos 0
	expected.push_back(0x00);        // DEFAULT
	expected.insert(expected.end(), 33, 0x22);
	AppendU64(expected, 0);          // proof
	AppendHash(expected, FillHash(0xEF));

	const std::vector<uint8_t> body = FrameBody(message, EProtocolVersion::V1000);
	REQUIRE(body == expected);

	ByteBuffer buffer(body, EProtocolVersion::V1000);
	const SegmentResponseMessage decoded =
		SegmentResponseMessage::Deserialize(buffer, MessageTypes::OutputSegment);
	REQUIRE(decoded.GetExtraRoot().value() == FillHash(0xEF));
	REQUIRE(std::holds_alternative<Segment<OutputIdentifier>>(decoded.GetPayload()));
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdWireAudit::RangeProofAndKernelSegmentResponseBytesMatchRust")
{
	const Segment<RangeProof> rangeProofSegment(
		SegmentIdentifier((uint8_t)11, 0),
		{},
		{},
		{ 0 },
		{ RangeProof(std::vector<unsigned char>(675, 0x00)) },
		SegmentProof::Empty());

	const SegmentResponseMessage rangeProofMessage =
		SegmentResponseMessage::CreateRangeProofs(FillHash(0xCD), rangeProofSegment);

	{
		std::vector<uint8_t> expected;
		AppendHash(expected, FillHash(0xCD));
		expected.push_back(0x0B);
		AppendU64(expected, 0);
		AppendU64(expected, 0);
		AppendU64(expected, 1);
		AppendU64(expected, 1);
		AppendU64(expected, 675);
		expected.insert(expected.end(), 675, 0x00);
		AppendU64(expected, 0);

		REQUIRE(FrameBody(rangeProofMessage, EProtocolVersion::V1000) == expected);
	}

	const TransactionKernel kernel(
		EKernelFeatures::DEFAULT_KERNEL, Fee(0, 0x0102030405ULL), 0,
		FillCommitment(0xAA), FillSignature(0xBB));

	const Segment<TransactionKernel> kernelSegment(
		SegmentIdentifier((uint8_t)11, 1),
		{},
		{},
		{ 4 },
		{ kernel },
		SegmentProof::Empty());

	const SegmentResponseMessage kernelMessage =
		SegmentResponseMessage::CreateKernels(FillHash(0xCD), kernelSegment);

	{
		const std::vector<uint8_t> kernelLeaf =
			KernelLeaf(EKernelFeatures::DEFAULT_KERNEL, 0x0102030405ULL, 0, 0xAA, 0xBB);

		std::vector<uint8_t> expected;
		AppendHash(expected, FillHash(0xCD));
		expected.push_back(0x0B);
		AppendU64(expected, 1);
		AppendU64(expected, 0);
		AppendU64(expected, 1);
		AppendU64(expected, 5);          // pos0 4 => wire 5
		expected.insert(expected.end(), kernelLeaf.cbegin(), kernelLeaf.cend());
		AppendU64(expected, 0);

		REQUIRE(FrameBody(kernelMessage, EProtocolVersion::V1000) == expected);

		ByteBuffer buffer(FrameBody(kernelMessage, EProtocolVersion::V1000), EProtocolVersion::V1000);
		const SegmentResponseMessage decoded =
			SegmentResponseMessage::Deserialize(buffer, MessageTypes::KernelSegment);
		REQUIRE(!decoded.GetExtraRoot().has_value());
		REQUIRE(std::holds_alternative<Segment<TransactionKernel>>(decoded.GetPayload()));
		REQUIRE(buffer.GetRemainingSize() == 0);
	}
}

TEST_CASE("PibdWireAudit::ResponseMaximumSizesMatchRust")
{
	// p2p/src/msg.rs max_msg_size: segment requests 41, responses 2 *
	// max_block_size(), where max_block_size = (MAX_BLOCK_WEIGHT / OUTPUT_WEIGHT) * 708.
	const uint64_t maxBlockSize = (40000ULL / 21ULL) * 708ULL;
	REQUIRE(maxBlockSize == 1348032ULL);

	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::OutputBitmapSegment) == 2 * maxBlockSize);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::OutputSegment) == 2 * maxBlockSize);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::RangeProofSegment) == 2 * maxBlockSize);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::KernelSegment) == 2 * maxBlockSize);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::OutputBitmapSegment) == 2696064ULL);
}

TEST_CASE("PibdWireAudit::TrailingBytesAreIgnoredLikeRust")
{
	// Rust decode_message() parses the body with BufReader and does not require
	// the reader to be exhausted, so trailing bytes inside a framed body are
	// ignored. C++ matches. (Frame length changes are handled by the header.)
	std::vector<BitmapBlock> blocks;
	blocks.push_back(BitmapBlock(1));
	const BitmapSegment segment(
		SegmentIdentifier((uint8_t)9, 0), std::move(blocks), SegmentProof::Empty());

	std::vector<uint8_t> bytes = segment.Serialized();
	bytes.push_back(0x00);
	bytes.push_back(0xFF);

	ByteBuffer buffer(bytes);
	const BitmapSegment decoded = BitmapSegment::Deserialize(buffer);
	REQUIRE(decoded.GetIdentifier() == SegmentIdentifier((uint8_t)9, 0));
	REQUIRE(buffer.GetRemainingSize() == 2);
}

TEST_CASE("PibdWireAudit::SegmentResponseTruncationRejected")
{
	const Segment<TransactionKernel> segment(
		SegmentIdentifier((uint8_t)11, 0),
		{},
		{},
		{ 0 },
		{ TransactionKernel(EKernelFeatures::COINBASE_KERNEL, Fee(), 0, FillCommitment(0xAA), FillSignature(0xBB)) },
		SegmentProof::Empty());

	const SegmentResponseMessage message =
		SegmentResponseMessage::CreateKernels(FillHash(0xCD), segment);

	const std::vector<uint8_t> body = FrameBody(message, EProtocolVersion::V1000);
	for (size_t cut = 0; cut < body.size(); ++cut)
	{
		ByteBuffer buffer(std::vector<uint8_t>(body.cbegin(), body.cbegin() + cut), EProtocolVersion::V1000);
		REQUIRE_THROWS_AS(
			SegmentResponseMessage::Deserialize(buffer, MessageTypes::KernelSegment),
			DeserializationException);
	}
}

// ---------------------------------------------------------------------------
// Positions / leaf indices / bitmap shifts (PIBD conventions)
// ---------------------------------------------------------------------------

TEST_CASE("PibdWireAudit::PmmrPositionIsNotLeafIndex")
{
	// Rust pmmr::insertion_to_pmmr_index(k) = 2k - popcount(k).
	const uint64_t expected[] = { 0, 1, 3, 4, 7, 8, 10, 11, 15, 16 };
	for (uint64_t leafIndex = 0; leafIndex < 10; ++leafIndex)
	{
		REQUIRE(MMRUtil::InsertionToPmmrIndex(leafIndex) == expected[leafIndex]);
		REQUIRE(MMRUtil::PmmrLeafToInsertionIndex(expected[leafIndex]) == leafIndex);
	}
	REQUIRE(!MMRUtil::PmmrLeafToInsertionIndex(2).has_value()); // internal node
}

TEST_CASE("PibdWireAudit::BitmapSegmentChunkShiftUsesLeafOffset")
{
	// Bitmap segment height 9 => 512 chunks per segment. Segment idx 3 starts
	// at chunk 1536, whose PMMR leaf position is insertion_to_pmmr_index(1536)
	// = 2*1536 - popcount(1536) = 3070. Bits inside each chunk address
	// chunkIdx * 1024 + bit of the output bitmap.
	std::vector<BitmapBlock> blocks;
	blocks.push_back(BitmapBlock(1));
	blocks.back().Set(0, true);
	blocks.back().Set(1023, true);

	const BitmapSegment segment(
		SegmentIdentifier((uint8_t)9, 3), std::move(blocks), SegmentProof::Empty());
	const Segment<BitmapChunk> pmmrSegment = segment.ToSegment();

	REQUIRE(pmmrSegment.GetLeafPositions() == std::vector<uint64_t>{ 3070 });
	REQUIRE(pmmrSegment.GetLeafData().size() == 1);
	REQUIRE(pmmrSegment.GetLeafData()[0].Get(0));
	REQUIRE(pmmrSegment.GetLeafData()[0].Get(1023));
	REQUIRE(!pmmrSegment.GetLeafData()[0].Get(1));
}

TEST_CASE("PibdWireAudit::BitmapBitToLeafIndexMapping")
{
	// Bit i of the output bitmap is the (i / 1024, i % 1024) bit of chunk i/1024,
	// i.e. output leaf index i (Rust BitmapAccumulator chunk_idx = idx / 1024).
	BitmapChunk chunk;
	chunk.Set(0, true);
	chunk.Set(1023, true);

	std::vector<BitmapChunk> chunks(2);
	chunks[0] = chunk;
	chunks[1].Set(1, true);

	// Rebuild the flat bitmap the way the assembler does and check the shift.
	std::vector<bool> bitmap(chunks.size() * BitmapChunk::LEN_BITS, false);
	for (size_t chunkIndex = 0; chunkIndex < chunks.size(); ++chunkIndex)
	{
		for (size_t bit = 0; bit < BitmapChunk::LEN_BITS; ++bit)
		{
			if (chunks[chunkIndex].Get(bit))
			{
				bitmap[(chunkIndex * BitmapChunk::LEN_BITS) + bit] = true;
			}
		}
	}

	REQUIRE((bool)bitmap[0]);
	REQUIRE((bool)bitmap[1023]);
	REQUIRE((bool)bitmap[1025]);
	REQUIRE(!(bool)bitmap[1024]);
	REQUIRE(bitmap.size() == 2048);
}

TEST_CASE("PibdWireAudit::BitmapChunkCountAndMmrSize")
{
	// The assembler is constructed with the output MMR *size* (nodes), not the
	// leaf count: n_leaves(size) outputs, ceil(n_leaves/1024) bitmap chunks and
	// bitmap MMR size 2*chunks - popcount(chunks).
	const BitmapSegmentAssembler oneChunk(2047, Hash());   // 1024 leaves
	REQUIRE(oneChunk.GetNumOutputs() == 1024);
	REQUIRE(oneChunk.GetNumChunks() == 1);
	REQUIRE(oneChunk.GetBitmapMmrSize() == 1);
	REQUIRE(oneChunk.GetRequiredSegments() == 1);

	const BitmapSegmentAssembler twoChunks(2048, Hash());  // 1025 leaves
	REQUIRE(twoChunks.GetNumOutputs() == 1025);
	REQUIRE(twoChunks.GetNumChunks() == 2);
	REQUIRE(twoChunks.GetBitmapMmrSize() == 3);
	REQUIRE(twoChunks.GetRequiredSegments() == 1);

	const BitmapSegmentAssembler fullSegments(1048575, Hash()); // 524288 leaves
	REQUIRE(fullSegments.GetNumOutputs() == 512 * 1024);
	REQUIRE(fullSegments.GetNumChunks() == 512);
	REQUIRE(fullSegments.GetBitmapMmrSize() == 1023);
	REQUIRE(fullSegments.GetRequiredSegments() == 1);

	const BitmapSegmentAssembler twoSegments(1048576, Hash()); // 524289 leaves
	REQUIRE(twoSegments.GetNumOutputs() == (512 * 1024) + 1);
	REQUIRE(twoSegments.GetNumChunks() == 513);
	REQUIRE(twoSegments.GetBitmapMmrSize() == 1024);
	REQUIRE(twoSegments.GetRequiredSegments() == 2);
}

TEST_CASE("PibdWireAudit::SegmentLeafRequiredUsesLeafIndices")
{
	// Output bitmap bit set for leaf index 3 only. In a 4-leaf MMR (size 7),
	// positions are leaves 0,1 at 0,1 and leaves 2,3 at 3,4.
	VectorSegmentBitmap bitmap(4);
	bitmap.Set(3, true);

	// Position 4 is leaf 3: required because its own bit is set.
	REQUIRE(IsSegmentLeafRequired(4, 7, bitmap));
	// Position 3 is leaf 2: sibling of leaf 3, so also required.
	REQUIRE(IsSegmentLeafRequired(3, 7, bitmap));
	// Position 0 is leaf 0: neither its bit nor leaf 1's bit is set.
	REQUIRE(!IsSegmentLeafRequired(0, 7, bitmap));

	// The very last leaf of an incomplete MMR is always required (Rust
	// Segment::root special case): size 5 = peaks at 2 and 4, so position 4 is
	// the last leaf; an empty bitmap still requires it.
	VectorSegmentBitmap empty(4);
	REQUIRE(IsSegmentLeafRequired(4, 5, empty));
	REQUIRE(!IsSegmentLeafRequired(3, 5, empty));
}

TEST_CASE("PibdWireAudit::SegmentHeightRangesMatchRustAdapters")
{
	// servers/src/common/adapters.rs: bitmap 9..14, output 11..16,
	// rangeproof 7..12, kernel 9..14 (half-open ranges => 9-13 etc.).
	REQUIRE(PibdParams::BITMAP_SEGMENT_HEIGHT == 9);
	REQUIRE(PibdParams::OUTPUT_SEGMENT_HEIGHT == 11);
	REQUIRE(PibdParams::RANGEPROOF_SEGMENT_HEIGHT == 11);
	REQUIRE(PibdParams::KERNEL_SEGMENT_HEIGHT == 11);

	REQUIRE(!PibdParams::IsValidBitmapSegmentHeight(8));
	REQUIRE(PibdParams::IsValidBitmapSegmentHeight(9));
	REQUIRE(PibdParams::IsValidBitmapSegmentHeight(13));
	REQUIRE(!PibdParams::IsValidBitmapSegmentHeight(14));

	REQUIRE(!PibdParams::IsValidOutputSegmentHeight(10));
	REQUIRE(PibdParams::IsValidOutputSegmentHeight(11));
	REQUIRE(PibdParams::IsValidOutputSegmentHeight(15));
	REQUIRE(!PibdParams::IsValidOutputSegmentHeight(16));

	REQUIRE(!PibdParams::IsValidRangeProofSegmentHeight(6));
	REQUIRE(PibdParams::IsValidRangeProofSegmentHeight(7));
	REQUIRE(PibdParams::IsValidRangeProofSegmentHeight(11));
	REQUIRE(!PibdParams::IsValidRangeProofSegmentHeight(12));

	REQUIRE(!PibdParams::IsValidKernelSegmentHeight(8));
	REQUIRE(PibdParams::IsValidKernelSegmentHeight(9));
	REQUIRE(PibdParams::IsValidKernelSegmentHeight(13));
	REQUIRE(!PibdParams::IsValidKernelSegmentHeight(14));
}
