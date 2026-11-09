#include <catch.hpp>

#include <PoW/PoWValidator.h>

TEST_CASE("PoWValidator::SupportedEdgeBits")
{
	// Secondary Cuckoo29.
	REQUIRE(PoWValidator::IsSupportedEdgeBits(29));

	// Primary Cuckatoo sizes accepted by grin v5.5.2 (31..63).
	REQUIRE(PoWValidator::IsSupportedEdgeBits(31));
	REQUIRE(PoWValidator::IsSupportedEdgeBits(32));
	REQUIRE(PoWValidator::IsSupportedEdgeBits(63));

	// Below the primary minimum, secondary-only values, and sizes that would
	// shift a 64-bit word by >= 64 are rejected.
	REQUIRE(!PoWValidator::IsSupportedEdgeBits(0));
	REQUIRE(!PoWValidator::IsSupportedEdgeBits(1));
	REQUIRE(!PoWValidator::IsSupportedEdgeBits(24));
	REQUIRE(!PoWValidator::IsSupportedEdgeBits(28));
	REQUIRE(!PoWValidator::IsSupportedEdgeBits(30));
	REQUIRE(!PoWValidator::IsSupportedEdgeBits(64));
	REQUIRE(!PoWValidator::IsSupportedEdgeBits(255));
}
