#pragma once

#include <Core/Models/OutputLocation.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>
#include <Crypto/Hasher.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/Hash.h>
#include <cstdint>
#include <vector>

// Metadata record for the persistent kernel excess -> PMMR position index.
//
// The record is the commit point of the index: entries are written to a new
// generation first, then this record is published with a single RocksDB Put.
// A crash before that Put leaves the previous (complete) generation active.
// The tip hash, height, kernel root and kernel count bind the index to one
// exact chain/PMMR state; the checksum detects in-place entry corruption even
// when the entry count still matches.
class KernelIndexState : public Traits::ISerializable
{
public:
	static constexpr uint32_t FORMAT_VERSION = 1;

	KernelIndexState() = default;

	KernelIndexState(
		const uint64_t generation,
		const uint64_t tipHeight,
		const Hash& tipHash,
		const Hash& kernelRoot,
		const uint64_t kernelCount,
		const uint64_t entryCount,
		const Hash& checksum)
		: m_generation(generation),
		m_tipHeight(tipHeight),
		m_tipHash(tipHash),
		m_kernelRoot(kernelRoot),
		m_kernelCount(kernelCount),
		m_entryCount(entryCount),
		m_checksum(checksum)
	{
	}

	uint64_t GetGeneration() const noexcept { return m_generation; }
	uint64_t GetTipHeight() const noexcept { return m_tipHeight; }
	const Hash& GetTipHash() const noexcept { return m_tipHash; }
	const Hash& GetKernelRoot() const noexcept { return m_kernelRoot; }
	uint64_t GetKernelCount() const noexcept { return m_kernelCount; }
	uint64_t GetEntryCount() const noexcept { return m_entryCount; }
	const Hash& GetChecksum() const noexcept { return m_checksum; }

	void SetChecksum(const Hash& checksum) noexcept { m_checksum = checksum; }
	void SetEntryCount(const uint64_t entryCount) noexcept { m_entryCount = entryCount; }

	void Serialize(Serializer& serializer) const noexcept final
	{
		serializer.Append<uint32_t>(FORMAT_VERSION);
		serializer.Append<uint64_t>(m_generation);
		serializer.Append<uint64_t>(m_tipHeight);
		serializer.AppendBigInteger<32>(m_tipHash);
		serializer.AppendBigInteger<32>(m_kernelRoot);
		serializer.Append<uint64_t>(m_kernelCount);
		serializer.Append<uint64_t>(m_entryCount);
		serializer.AppendBigInteger<32>(m_checksum);
	}

	static KernelIndexState Deserialize(ByteBuffer& byteBuffer)
	{
		const uint32_t version = byteBuffer.ReadU32();
		if (version != FORMAT_VERSION) {
			throw DESERIALIZATION_EXCEPTION("Unsupported kernel index state version");
		}

		KernelIndexState state;
		state.m_generation = byteBuffer.ReadU64();
		state.m_tipHeight = byteBuffer.ReadU64();
		state.m_tipHash = byteBuffer.ReadBigInteger<32>();
		state.m_kernelRoot = byteBuffer.ReadBigInteger<32>();
		state.m_kernelCount = byteBuffer.ReadU64();
		state.m_entryCount = byteBuffer.ReadU64();
		state.m_checksum = byteBuffer.ReadBigInteger<32>();
		return state;
	}

private:
	uint64_t m_generation = 0;
	uint64_t m_tipHeight = 0;
	Hash m_tipHash;
	Hash m_kernelRoot;
	uint64_t m_kernelCount = 0;
	uint64_t m_entryCount = 0;
	Hash m_checksum;
};

inline Hash KernelIndexEntryHash(const Commitment& excess, const OutputLocation& location)
{
	Serializer serializer;
	serializer.AppendBigInteger<33>(excess.GetBytes());
	serializer.Append<uint64_t>(location.GetLeafIndex().GetPosition());
	serializer.Append<uint64_t>(location.GetBlockHeight());
	return Hasher::Blake2b(serializer.GetBytes());
}

// Additive checksum: XOR is commutative, so rewind/reorg can remove entries
// without a full rescan while still detecting a changed/missing entry.
inline Hash KernelIndexXorHash(Hash accumulator, const Commitment& excess, const OutputLocation& location)
{
	const Hash entry = KernelIndexEntryHash(excess, location);
	auto bytes = accumulator.GetData();
	const auto& other = entry.GetData();
	for (size_t i = 0; i < bytes.size() && i < other.size(); i++) {
		bytes[i] ^= other[i];
	}
	return Hash(bytes);
}
