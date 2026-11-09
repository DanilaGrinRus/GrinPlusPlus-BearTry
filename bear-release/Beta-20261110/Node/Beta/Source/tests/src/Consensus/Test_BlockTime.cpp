#include <catch.hpp>

#include <Consensus.h>

using namespace Consensus;

TEST_CASE("Consensus::GetMaxBlockTime")
{
	const auto now = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000500));

	REQUIRE(GetMaxBlockTime(now) == 1300);
}

TEST_CASE("Consensus::GetMaxCoinbaseHeight")
{
	REQUIRE(GetMaxCoinbaseHeight(Environment::MAINNET, 1440) == 0);
	REQUIRE(GetMaxCoinbaseHeight(Environment::MAINNET, 1441) == 1);
}

TEST_CASE("Consensus::GetHorizonHeight")
{
	REQUIRE(GetHorizonHeight(10080) == 0);
	REQUIRE(GetHorizonHeight(10081) == 1);
}