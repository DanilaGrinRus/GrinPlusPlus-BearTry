#pragma once

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>

#include <cstdint>
#include <vector>

// A bitmap "chunk" of 1024 contiguous bits.
// Matches grin v5.5.2 BitmapChunk (chain/src/txhashset/bitmap_accumulator.rs):
// the wire form is the raw 128 bytes, least-significant bit first within a byte.
class BitmapChunk
{
public:
	static constexpr size_t LEN_BITS = 1024;
	static constexpr size_t LEN_BYTES = LEN_BITS / 8;

	BitmapChunk()
		: m_bytes(LEN_BYTES, 0)
	{
	}

	bool Get(const size_t idx) const
	{
		if (idx >= LEN_BITS) {
			throw DESERIALIZATION_EXCEPTION("Bitmap chunk index is out of range");
		}

		return (m_bytes[idx / 8] & (uint8_t)(1u << (7 - (idx % 8)))) != 0;
	}

	void Set(const size_t idx, const bool value)
	{
		if (idx >= LEN_BITS) {
			throw DESERIALIZATION_EXCEPTION("Bitmap chunk index is out of range");
		}

		const uint8_t mask = (uint8_t)(1u << (7 - (idx % 8)));
		if (value) {
			m_bytes[idx / 8] = (uint8_t)(m_bytes[idx / 8] | mask);
		} else {
			m_bytes[idx / 8] = (uint8_t)(m_bytes[idx / 8] & (uint8_t)~mask);
		}
	}

	bool Any() const
	{
		for (const uint8_t byte : m_bytes) {
			if (byte != 0) {
				return true;
			}
		}

		return false;
	}

	// Serialized element bytes (128 bytes); used both for hashing and for the
	// PMMR data file representation.
	const std::vector<uint8_t>& GetBytes() const noexcept { return m_bytes; }

	void Serialize(Serializer& serializer) const
	{
		serializer.AppendByteVector(m_bytes);
	}

	static BitmapChunk Deserialize(ByteBuffer& byteBuffer)
	{
		BitmapChunk chunk;
		chunk.m_bytes = byteBuffer.ReadVector(LEN_BYTES);
		return chunk;
	}

private:
	std::vector<uint8_t> m_bytes;
};
