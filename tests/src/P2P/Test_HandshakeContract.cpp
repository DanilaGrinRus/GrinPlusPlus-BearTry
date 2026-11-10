#include <catch.hpp>

#include <Core/Config.h>
#include <Core/Global.h>
#include <Core/Genesis.h>
#include <Core/Enums/ProtocolVersion.h>
#include <Core/Exceptions/DeserializationException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Crypto/Models/Hash.h>
#include <Net/SocketAddress.h>
#include <P2P/Capabilities.h>
#include <P2P/Common.h>
#include <P2P/Messages/HandMessage.h>
#include <P2P/Messages/MessageHeader.h>

namespace
{
	std::vector<uint8_t> HandHeaderBytes(const uint64_t bodyLength)
	{
		Serializer serializer(ProtocolVersion::Local());
		serializer.AppendByteVector(Global::GetConfig().GetMagicBytes());
		serializer.Append<uint8_t>((uint8_t)MessageTypes::Hand);
		serializer.Append<uint64_t>(bodyLength);
		return serializer.GetBytes();
	}
}

// The handshake compares the decoded Shake/Hand genesis hash with the
// process-local Global::GetGenesisHash() (HandShake.cpp:69 and :103). This test
// pins the wire contract: a Hand message round-trips with its genesis field
// intact, so a handshake cannot silently accept a different chain.
TEST_CASE("HandShake message round-trips with the configured genesis")
{
	const Hash genesis = Global::GetGenesisHash();
	HandMessage hand(
		P2P::PROTOCOL_VERSION,
		Capabilities(7),
		0x1234567890ABCDEFULL,
		genesis,
		100000,
		SocketAddress("127.0.0.1", 3414),
		SocketAddress("127.0.0.1", 3415),
		"Grin++ test"
	);

	const std::vector<uint8_t> bytes = hand.Serialize(ProtocolVersion::Local());
	REQUIRE(bytes.size() > 11);

	ByteBuffer buffer(bytes);
	const MessageHeader header = MessageHeader::Deserialize(buffer);
	REQUIRE(header.GetMessageType() == MessageTypes::Hand);
	REQUIRE(header.GetMessageLength() == bytes.size() - 11);
	REQUIRE(header.GetMagicBytes() == Global::GetConfig().GetMagicBytes());

	const HandMessage parsed = HandMessage::Deserialize(buffer);
	REQUIRE(parsed.GetHash() == genesis);
	REQUIRE(parsed.GetNonce() == 0x1234567890ABCDEFULL);
	REQUIRE(parsed.GetVersion() == P2P::PROTOCOL_VERSION);
	REQUIRE(parsed.GetTotalDifficulty() == 100000);
	REQUIRE(parsed.GetUserAgent() == "Grin++ test");
}

// The genesis check is dynamic, not a hardcoded mainnet constant: under
// AUTOMATED_TESTING the node's chain is the floonet chain, so a mainnet Hand
// must not compare equal to Global::GetGenesisHash().
TEST_CASE("HandShake genesis comparison is chain-local")
{
	const Hash localGenesis = Global::GetGenesisHash();
	REQUIRE(localGenesis == Genesis::FLOONET_GENESIS.GetHash());
	REQUIRE(localGenesis != Genesis::MAINNET_GENESIS.GetHash());

	HandMessage foreign(
		P2P::PROTOCOL_VERSION,
		Capabilities(7),
		42,
		Genesis::MAINNET_GENESIS.GetHash(),
		0,
		SocketAddress("127.0.0.1", 3414),
		SocketAddress("127.0.0.1", 3415),
		"Grin++ test"
	);

	ByteBuffer buffer(foreign.Serialize(ProtocolVersion::Local()));
	const MessageHeader header = MessageHeader::Deserialize(buffer);
	REQUIRE(header.GetMessageType() == MessageTypes::Hand);
	const HandMessage parsed = HandMessage::Deserialize(buffer);
	REQUIRE(parsed.GetHash() == Genesis::MAINNET_GENESIS.GetHash());
	REQUIRE_FALSE(parsed.GetHash() == Global::GetGenesisHash());
}

// GP-3 reads and validates the 11-byte header before the body. The header
// parser must enforce the protocol's 4x forward-compatibility cap, so an
// oversized Hand body is rejected before any body read happens.
TEST_CASE("MessageHeader enforces the handshake body cap")
{
	const uint64_t handCap = MessageTypes::GetMaximumSize(MessageTypes::Hand);
	REQUIRE(handCap == 128);

	{
		ByteBuffer buffer(HandHeaderBytes(handCap));
		REQUIRE_NOTHROW(MessageHeader::Deserialize(buffer));
	}
	{
		ByteBuffer buffer(HandHeaderBytes(handCap * 4));
		REQUIRE_NOTHROW(MessageHeader::Deserialize(buffer));
	}
	{
		ByteBuffer buffer(HandHeaderBytes(handCap * 4 + 1));
		REQUIRE_THROWS_AS(MessageHeader::Deserialize(buffer), DeserializationException);
	}

	// A foreign chain's magic bytes must fail before any length is trusted.
	{
		Serializer serializer(ProtocolVersion::Local());
		serializer.Append<uint8_t>(0xFF);
		serializer.Append<uint8_t>(0xFF);
		serializer.Append<uint8_t>((uint8_t)MessageTypes::Hand);
		serializer.Append<uint64_t>(0);
		ByteBuffer buffer(serializer.GetBytes());
		REQUIRE_THROWS_AS(MessageHeader::Deserialize(buffer), DeserializationException);
	}
}
