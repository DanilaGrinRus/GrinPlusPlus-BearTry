#include <catch.hpp>

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>

#include <cstdint>
#include <vector>

namespace
{
	std::vector<uint8_t> BigEndianU64(const uint64_t value)
	{
		std::vector<uint8_t> bytes(8);
		for (int i = 0; i < 8; ++i) {
			bytes[i] = (uint8_t)(value >> (56 - 8 * i));
		}

		return bytes;
	}
}

TEST_CASE("ByteBuffer::ReadBounds")
{
	const std::vector<uint8_t> bytes{ 0x01, 0x02, 0x03, 0x04 };
	ByteBuffer buffer(bytes);

	REQUIRE(buffer.ReadU16() == 0x0102);
	REQUIRE(buffer.GetRemainingSize() == 2);
	REQUIRE_THROWS_AS(buffer.ReadU32(), DeserializationException);
}

TEST_CASE("ByteBuffer::VarStrLengthOverflow")
{
	// A u64 length that wraps `m_index + length` must be rejected before any
	// allocation or pointer arithmetic happens. Regression test for the handshake
	// user-agent crash (GAP G-19).
	const uint64_t declaredLength = 0xFFFFFFFFFFFFFFE6ull;

	std::vector<uint8_t> payload(32, 0);
	const std::vector<uint8_t> lengthBytes = BigEndianU64(declaredLength);
	payload.insert(payload.end(), lengthBytes.cbegin(), lengthBytes.cend());

	ByteBuffer buffer(payload);
	buffer.ReadVector(32);
	REQUIRE_THROWS_AS(buffer.ReadVarStr(), DeserializationException);
}

TEST_CASE("ByteBuffer::VectorLengthOverflow")
{
	const uint64_t declaredLength = 0xFFFFFFFFFFFFFFE6ull;

	std::vector<uint8_t> payload(32, 0);
	ByteBuffer buffer(payload);
	buffer.ReadVector(32);
	REQUIRE_THROWS_AS(buffer.ReadVector(declaredLength), DeserializationException);
}

TEST_CASE("ByteBuffer::VarStrRoundTrip")
{
	Serializer serializer;
	const std::string text = "Grin++ 1.2.9";
	serializer.AppendVarStr(text);

	ByteBuffer buffer(serializer.GetBytes());
	REQUIRE(buffer.ReadVarStr() == text);
	REQUIRE(buffer.GetRemainingSize() == 0);
}
