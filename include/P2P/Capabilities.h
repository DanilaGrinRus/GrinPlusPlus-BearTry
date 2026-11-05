// Derived from grin 5.5.2 (Rust), Copyright The Grin Developers.
// Licensed under the Apache License, Version 2.0. Modified for the Grin++ C++ fork.
// See THIRD_PARTY_NOTICES.md and LICENSES/Apache-2.0.txt for details.
#pragma once

#include <cstdint>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <json/json.h>

class Capabilities
{
public:
	enum ECapability
	{
		// We don't know (yet) what the peer can do.
		UNKNOWN = 0x00,

		// Can provide full history of headers back to genesis (for at least one
		// arbitrary fork). Grin 5.5.2 calls bit 0x01 HEADER_HIST; older Grin and
		// Grin++ called it FULL_HIST. Wire format is identical.
		HEADER_HIST = 0x01,
		FULL_HIST = HEADER_HIST,

		// Can provide the TxHashSet for some recent-enough height.
		TXHASHET_HIST = 0x02, // LIGHT_CLIENT: We can prune old range-proofs and drop unnecessary rangeproof hashes if this isn't set

		// Can provide a list of healthy peers
		PEER_LIST = 0x04,

		// Can broadcast and request transactions by kernel hash (Grin 5.5.2).
		TX_KERNEL_HASH = 0x08,

		// Can provide PIBD segments during initial byte download (Grin 5.5.2).
		PIBD_HIST = 0x10,

		// Can provide historical blocks for archival sync (Grin 5.5.2).
		BLOCK_HIST = 0x20,

		// As above, with the PIBD serialization fix (Grin PR #3705) applied.
		PIBD_HIST_1 = 0x40,

		// PIHD (parallel initial header download) extension bit from Grin
		// staging. v5.5.2 has no messages 29/30. This node has an experimental
		// client path (P2P.PIHD_MODE=client) but no server implementation, so
		// the bit is never advertised; legacy header sync remains the fallback.
		PIHD_HIST = 0x80,

		FAST_SYNC_NODE = (TXHASHET_HIST | PEER_LIST),
		ARCHIVE_NODE = (HEADER_HIST | TXHASHET_HIST | PEER_LIST),

		// Capabilities this node advertises today. PIBD serving is implemented
		// only for the currently open flushed horizon; it does not yet provide
		// the older archive horizon a normal Rust 5.5.2 peer selects. Do not
		// advertise PIBD bits until that snapshot lifecycle is integrated and
		// bounded. BLOCK_HIST stays off until archival block serving exists.
		ADVERTISED = (HEADER_HIST | FAST_SYNC_NODE),
	};

	// Runtime handshake value. The compile-time ADVERTISED mask stays
	// PIBD-free; the PIBD bits are added only when the caller has a validated,
	// ready archive that matches the current deterministic horizon
	// (PibdArchiveServer::ShouldAdvertise). Never derive this from the presence
	// of a zip file, a preparing state or an unvalidated load.
	static Capabilities Advertised(const bool pibdServingReady)
	{
		uint32_t value = (uint32_t)ADVERTISED;
		if (pibdServingReady) {
			value |= (uint32_t)(PIBD_HIST | PIBD_HIST_1);
		}

		return Capabilities(value);
	}

	Capabilities(const uint32_t value)
		: m_value(value)
	{

	}

	Capabilities(const ECapability value)
		: m_value(value)
	{

	}

	inline bool IsUnknown() const noexcept { return m_value == UNKNOWN; }
	inline bool HasCapability(const ECapability capability) const noexcept { return (m_value & (uint32_t)capability) == (uint32_t)capability; }
	inline void AddCapability(const ECapability capability) noexcept { m_value = (m_value | capability); }
	inline ECapability GetCapability() const noexcept { return (ECapability)m_value; }

	void Serialize(Serializer& serializer) const
	{
		serializer.Append<uint32_t>(m_value);
	}

	static Capabilities Deserialize(ByteBuffer& byteBuffer)
	{
		const uint32_t value = byteBuffer.ReadU32();
		return Capabilities(value);
	}

	Json::Value ToJSON() const
	{
		Json::Value json;
		json["bits"] = Json::UInt(m_value);
		return json;
	}

private:
	uint32_t m_value;
};