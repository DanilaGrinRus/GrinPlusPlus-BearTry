#include <catch.hpp>

#include <P2P/Seed/SeederPolicy.h>

TEST_CASE("SeederPolicy::TargetOutbound")
{
	// Old configs with MIN_PEERS 2-3 still aim for 8.
	REQUIRE(SeederPolicy::TargetOutbound(2, 12) == 8);
	REQUIRE(SeederPolicy::TargetOutbound(2, 30) == 8);
	REQUIRE(SeederPolicy::TargetOutbound(3, 12) == 8);

	// MAX_PEERS caps the target.
	REQUIRE(SeederPolicy::TargetOutbound(2, 5) == 5);
	REQUIRE(SeederPolicy::TargetOutbound(1, 5) == 5);

	// A configured minimum above 8 is respected.
	REQUIRE(SeederPolicy::TargetOutbound(10, 60) == 10);
	REQUIRE(SeederPolicy::TargetOutbound(12, 30) == 12);

	// Boundary cases.
	REQUIRE(SeederPolicy::TargetOutbound(8, 8) == 8);
	REQUIRE(SeederPolicy::TargetOutbound(0, 0) == 0);
	REQUIRE(SeederPolicy::ConnectionsToAdd(3, 8, 15) == 5);
	REQUIRE(SeederPolicy::ConnectionsToAdd(8, 8, 15) == 0);
}
