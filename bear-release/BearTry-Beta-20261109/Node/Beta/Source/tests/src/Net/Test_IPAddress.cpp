#include <catch.hpp>

#include <Net/IPAddress.h>

TEST_CASE("IPAddress")
{
    //
    // V4 Serialization
    //
    std::array<uint8_t, 4> ipv4_bytes{ 0x10, 0x20, 0x30, 0x40 };
    IPAddress ipv4 = IPAddress::CreateV4(ipv4_bytes);
    REQUIRE(ipv4.Serialized() == std::vector<uint8_t>{ 0x00, 0x10, 0x20, 0x30, 0x40 });
    REQUIRE(ByteBuffer(ipv4.Serialized()).Read<IPAddress>() == ipv4);

    //
    // V6 Serialization
    //
    std::array<uint8_t, 16> ipv6_bytes{ 0x05, 0x10, 0x15, 0x20, 0x25, 0x30, 0x35, 0x40, 0x45, 0x50, 0x55, 0x60, 0x65, 0x70, 0x75, 0x80 };
    IPAddress ipv6 = IPAddress::CreateV6(ipv6_bytes);
    REQUIRE(ipv6.Serialized() == std::vector<uint8_t>{ 0x01, 0x05, 0x10, 0x15, 0x20, 0x25, 0x30, 0x35, 0x40, 0x45, 0x50, 0x55, 0x60, 0x65, 0x70, 0x75, 0x80 });
    REQUIRE(ByteBuffer(ipv6.Serialized()).Read<IPAddress>() == ipv6);
}
#include <Net/PeerAPIPath.h>
#include "../../../src/Server/Node/API/NodeAPIParsing.h"

TEST_CASE("Node REST numeric parameters require a complete unsigned decimal")
{
    REQUIRE(NodeAPIParsing::UnsignedDecimal("0").value() == 0);
    REQUIRE(NodeAPIParsing::UnsignedDecimal("42").value() == 42);
    REQUIRE(NodeAPIParsing::UnsignedDecimal("18446744073709551615").value() == UINT64_MAX);
    REQUIRE_FALSE(NodeAPIParsing::UnsignedDecimal("18446744073709551616").has_value());
    REQUIRE_FALSE(NodeAPIParsing::UnsignedDecimal("-1").has_value());
    REQUIRE_FALSE(NodeAPIParsing::UnsignedDecimal("+1").has_value());
    REQUIRE_FALSE(NodeAPIParsing::UnsignedDecimal("1junk").has_value());
    REQUIRE_FALSE(NodeAPIParsing::UnsignedDecimal("").has_value());
}

TEST_CASE("Peer API parses IPv4 and IPv6 paths without truncation")
{
    REQUIRE(PeerAPIPath::Address("127.0.0.1/ban") == "127.0.0.1");
    REQUIRE(PeerAPIPath::Address("127.0.0.1:13414/unban") == "127.0.0.1");
    REQUIRE(PeerAPIPath::Address("2001:db8::1/unban") == "2001:db8::1");
    REQUIRE(PeerAPIPath::Address("[2001:db8::1]:13414/ban") == "2001:db8::1");
    REQUIRE(IPAddress::Parse(PeerAPIPath::Address("[::1]/unban")).IsV6());
    REQUIRE(PeerAPIPath::Address("[::1]:0/ban").empty());
    REQUIRE(PeerAPIPath::Address("[::1]:65536/ban").empty());
    REQUIRE(PeerAPIPath::Address("[::1]junk/ban").empty());
    REQUIRE(PeerAPIPath::Address("127.0.0.1:abc/ban").empty());
    REQUIRE(PeerAPIPath::Address("[::1/ban").empty());
}
