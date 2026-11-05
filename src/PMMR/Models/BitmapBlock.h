#pragma once

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>

#include <algorithm>
#include <cstdint>
#include <vector>

// A block of up to 2^16 bits with a compact wire encoding, chosen by occupancy.
// Wire format matches grin v5.5.2 chain/src/txhashset/bitmap_accumulator.rs
// (BitmapBlock): u8 chunk_count, u8 mode, then raw bits or index lists.
class BitmapBlock : public Traits::ISerializable
{
public:
	static constexpr size_t CHUNK_BITS = 1024;
	// A block covers 2^16 bits, i.e. 64 chunks of 1024 bits (grin BitmapBlock::NBITS).
	static constexpr size_t MAX_CHUNKS = 64;
	static constexpr size_t MAX_BITS = MAX_CHUNKS * CHUNK_BITS;

	enum class ESerializationMode : uint8_t
	{
		Raw = 0,
		Positive = 1,
		Negative = 2
	};

	explicit BitmapBlock(const size_t numChunks)
		: m_numChunks(numChunks)
	{
		if (numChunks > MAX_CHUNKS) {
			throw DESERIALIZATION_EXCEPTION("Bitmap block chunk count exceeds the maximum");
		}

		m_bytes.assign((numChunks * CHUNK_BITS) / 8, 0);
	}

	size_t GetNumChunks() const noexcept { return m_numChunks; }
	size_t GetNumBits() const noexcept { return m_numChunks * CHUNK_BITS; }

	bool Get(const size_t idx) const
	{
		if (idx >= GetNumBits()) {
			throw DESERIALIZATION_EXCEPTION("Bitmap block index is out of range");
		}

		return (m_bytes[idx / 8] & (uint8_t)(1u << (7 - (idx % 8)))) != 0;
	}

	void Set(const size_t idx, const bool value)
	{
		if (idx >= GetNumBits()) {
			throw DESERIALIZATION_EXCEPTION("Bitmap block index is out of range");
		}

		const uint8_t mask = (uint8_t)(1u << (7 - (idx % 8)));
		if (value) {
			m_bytes[idx / 8] = (uint8_t)(m_bytes[idx / 8] | mask);
		} else {
			m_bytes[idx / 8] = (uint8_t)(m_bytes[idx / 8] & (uint8_t)~mask);
		}
	}

	void Serialize(Serializer& serializer) const final
	{
		const size_t numBits = GetNumBits();
		size_t setBits = 0;
		for (size_t i = 0; i < numBits; ++i) {
			if (Get(i)) {
				++setBits;
			}
		}

		serializer.Append<uint8_t>((uint8_t)m_numChunks);
		const size_t unsetBits = numBits - setBits;
		const size_t threshold = MAX_BITS / 16;

		if (setBits < threshold) {
			serializer.Append<uint8_t>((uint8_t)ESerializationMode::Positive);
			serializer.Append<uint16_t>((uint16_t)setBits);
			for (size_t i = 0; i < numBits; ++i) {
				if (Get(i)) {
					serializer.Append<uint16_t>((uint16_t)i);
				}
			}
		} else if (unsetBits < threshold) {
			serializer.Append<uint8_t>((uint8_t)ESerializationMode::Negative);
			serializer.Append<uint16_t>((uint16_t)unsetBits);
			for (size_t i = 0; i < numBits; ++i) {
				if (!Get(i)) {
					serializer.Append<uint16_t>((uint16_t)i);
				}
			}
		} else {
			serializer.Append<uint8_t>((uint8_t)ESerializationMode::Raw);
			serializer.AppendByteVector(m_bytes);
		}
	}

	static BitmapBlock Deserialize(ByteBuffer& byteBuffer)
	{
		const uint8_t numChunks = byteBuffer.ReadU8();
		if (numChunks > MAX_CHUNKS) {
			throw DESERIALIZATION_EXCEPTION_F("Bitmap block chunk count {} exceeds the maximum", numChunks);
		}

		const size_t numBits = (size_t)numChunks * CHUNK_BITS;
		const uint8_t mode = byteBuffer.ReadU8();

		BitmapBlock block(numChunks);
		switch ((ESerializationMode)mode) {
			case ESerializationMode::Raw:
			{
				const std::vector<uint8_t> bytes = byteBuffer.ReadVector(numBits / 8);
				block.m_bytes = bytes;
				break;
			}
			case ESerializationMode::Positive:
			{
				const uint16_t count = byteBuffer.ReadU16();
				for (uint16_t i = 0; i < count; ++i) {
					const uint16_t pos = byteBuffer.ReadU16();
					if ((size_t)pos >= numBits) {
						throw DESERIALIZATION_EXCEPTION("Bitmap block positive index is out of range");
					}

					block.Set((size_t)pos, true);
				}

				break;
			}
			case ESerializationMode::Negative:
			{
				block.m_bytes.assign(numBits / 8, 0xFF);
				const uint16_t count = byteBuffer.ReadU16();
				for (uint16_t i = 0; i < count; ++i) {
					const uint16_t pos = byteBuffer.ReadU16();
					if ((size_t)pos >= numBits) {
						throw DESERIALIZATION_EXCEPTION("Bitmap block negative index is out of range");
					}

					block.Set((size_t)pos, false);
				}

				break;
			}
			default:
				throw DESERIALIZATION_EXCEPTION_F("Unknown bitmap block serialization mode {}", mode);
		}

		return block;
	}

private:
	size_t m_numChunks;
	std::vector<uint8_t> m_bytes;
};
