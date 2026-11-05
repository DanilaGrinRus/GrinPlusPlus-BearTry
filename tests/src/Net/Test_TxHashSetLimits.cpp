#include <catch.hpp>

#include <P2P/Pipeline/TxHashSetLimits.h>

TEST_CASE("TxHashSetLimits::PlausibleArchiveSizes")
{
	REQUIRE(!TxHashSetLimits::IsPlausibleSize(0));
	REQUIRE(TxHashSetLimits::IsPlausibleSize(1));
	REQUIRE(TxHashSetLimits::IsPlausibleSize(3ULL * 1024 * 1024 * 1024));
	REQUIRE(TxHashSetLimits::IsPlausibleSize(TxHashSetLimits::MAX_ZIPPED_SIZE));
	REQUIRE(!TxHashSetLimits::IsPlausibleSize(TxHashSetLimits::MAX_ZIPPED_SIZE + 1));
}
