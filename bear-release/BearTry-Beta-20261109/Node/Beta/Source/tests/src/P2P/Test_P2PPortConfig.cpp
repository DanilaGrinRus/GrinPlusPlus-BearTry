#include <catch.hpp>

#include <Core/Config/P2PConfig.h>
#include <Core/Enums/Environment.h>

namespace
{
	P2PConfig WithPort(const Json::Value& portValue)
	{
		Json::Value json;
		json[ConfigProps::P2P::P2P] = Json::Value();
		json[ConfigProps::P2P::P2P][ConfigProps::P2P::P2P_PORT] = portValue;
		return P2PConfig(Environment::FLOONET, json);
	}
}

TEST_CASE("P2PConfig::ReadsConfiguredP2PPort")
{
	Json::Value json;
	json[ConfigProps::P2P::P2P] = Json::Value();
	json[ConfigProps::P2P::P2P][ConfigProps::P2P::P2P_PORT] = 44556;

	const P2PConfig config(Environment::FLOONET, json);
	REQUIRE(config.GetP2PPort() == 44556);
}

TEST_CASE("P2PConfig::UsesNetworkDefaultPortWithoutOverride")
{
	const Json::Value json;
	const P2PConfig mainnet(Environment::MAINNET, json);
	const P2PConfig floonet(Environment::FLOONET, json);

	REQUIRE(mainnet.GetP2PPort() == 3414);
	REQUIRE(floonet.GetP2PPort() == 13414);
}

TEST_CASE("P2PConfig::AcceptsPortRangeBoundaries")
{
	REQUIRE(WithPort(1).GetP2PPort() == 1);
	REQUIRE(WithPort(44100).GetP2PPort() == 44100);
	REQUIRE(WithPort(65535).GetP2PPort() == 65535);
}

TEST_CASE("P2PConfig::RejectsInvalidPortWithoutWrapping")
{
	const uint16_t floonetDefault = 13414;

	// Zero and negative values must not wrap; >65535 must not truncate to 0.
	REQUIRE(WithPort(0).GetP2PPort() == floonetDefault);
	REQUIRE(WithPort(-1).GetP2PPort() == floonetDefault);
	REQUIRE(WithPort(65536).GetP2PPort() == floonetDefault);
	REQUIRE(WithPort(1000000).GetP2PPort() == floonetDefault);

	// Wrong JSON type is ignored rather than coerced.
	REQUIRE(WithPort("44556").GetP2PPort() == floonetDefault);
	REQUIRE(WithPort(44556.5).GetP2PPort() == floonetDefault);
}
