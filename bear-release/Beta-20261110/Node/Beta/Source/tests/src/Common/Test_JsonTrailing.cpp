#include <catch.hpp>

#include <Core/Util/JsonUtil.h>

TEST_CASE("JsonUtil::Parse rejects trailing garbage")
{
	Json::Value parsed;

	REQUIRE(JsonUtil::Parse("{\"a\":1}", parsed));
	REQUIRE(parsed["a"].asInt() == 1);
	REQUIRE(JsonUtil::Parse("{\"a\":1} \r\n\t", parsed));
	REQUIRE(JsonUtil::Parse("{\"a\":[{\"b\":\"}\"}]}", parsed));

	REQUIRE_FALSE(JsonUtil::Parse("{\"a\":1}SENTINEL", parsed));
	REQUIRE_FALSE(JsonUtil::Parse("{\"a\":1}{\"b\":2}", parsed));
	REQUIRE_FALSE(JsonUtil::Parse("", parsed));
	REQUIRE_FALSE(JsonUtil::Parse("   ", parsed));
}
