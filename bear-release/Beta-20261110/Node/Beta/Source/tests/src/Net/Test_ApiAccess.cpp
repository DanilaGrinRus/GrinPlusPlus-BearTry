#include <catch.hpp>

#include <Net/Servers/RPC/ApiAccess.h>

#include <cstdint>
#include <string>

namespace
{
	const std::string VALID_TOKEN =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
}

TEST_CASE("ApiAccess::ValidateToken")
{
	REQUIRE(ApiAccess::ValidateToken(VALID_TOKEN, VALID_TOKEN));
	REQUIRE(ApiAccess::ValidateToken(
		"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF", VALID_TOKEN));
	REQUIRE_FALSE(ApiAccess::ValidateToken("", VALID_TOKEN));
	REQUIRE_FALSE(ApiAccess::ValidateToken(VALID_TOKEN.substr(0, 63), VALID_TOKEN));
	REQUIRE_FALSE(ApiAccess::ValidateToken(
		"1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", VALID_TOKEN));
	REQUIRE_FALSE(ApiAccess::ValidateToken(VALID_TOKEN, ""));
}

TEST_CASE("ApiAccess::IsAllowedHost")
{
	const uint16_t port = 13424;
	REQUIRE(ApiAccess::IsAllowedHost("127.0.0.1:13424", port));
	REQUIRE(ApiAccess::IsAllowedHost("localhost:13424", port));
	REQUIRE(ApiAccess::IsAllowedHost("[::1]:13424", port));
	REQUIRE(ApiAccess::IsAllowedHost("127.0.0.1", port));
	REQUIRE_FALSE(ApiAccess::IsAllowedHost("", port));
	REQUIRE_FALSE(ApiAccess::IsAllowedHost("evil.com:13424", port));
	REQUIRE_FALSE(ApiAccess::IsAllowedHost("127.0.0.1:9999", port));
	REQUIRE_FALSE(ApiAccess::IsAllowedHost("user@127.0.0.1:13424", port));
	REQUIRE_FALSE(ApiAccess::IsAllowedHost("192.168.1.10:13424", port));
}

TEST_CASE("ApiAccess::IsAllowedOrigin")
{
	const uint16_t port = 13424;
	REQUIRE(ApiAccess::IsAllowedOrigin("", port));
	REQUIRE(ApiAccess::IsAllowedOrigin("null", port));
	REQUIRE(ApiAccess::IsAllowedOrigin("http://127.0.0.1:3000", port));
	REQUIRE(ApiAccess::IsAllowedOrigin("http://localhost:3000", port));
	REQUIRE(ApiAccess::IsAllowedOrigin("https://127.0.0.1", port));
	REQUIRE_FALSE(ApiAccess::IsAllowedOrigin("https://evil.com", port));
	REQUIRE_FALSE(ApiAccess::IsAllowedOrigin("file:///etc/passwd", port));
	REQUIRE_FALSE(ApiAccess::IsAllowedOrigin("http://192.168.1.10:3000", port));
}