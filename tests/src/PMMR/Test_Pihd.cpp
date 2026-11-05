#include <catch.hpp>

#include <P2P/PihdParams.h>

TEST_CASE("Pihd::SegmentArithmetic")
{
	REQUIRE(Pihd::HEADER_SEGMENT_HEIGHT == 9);
	REQUIRE(Pihd::HEADERS_PER_SEGMENT == 512);
	REQUIRE(Pihd::SegmentStartHeight(0) == 1);
	REQUIRE(Pihd::SegmentStartHeight(1) == 513);
	REQUIRE(Pihd::SegmentStartHeight(2) == 1025);

	REQUIRE(Pihd::FirstSegmentIdxForHeight(1) == 0);
	REQUIRE(Pihd::FirstSegmentIdxForHeight(2) == 0);
	REQUIRE(Pihd::FirstSegmentIdxForHeight(512) == 0);
	REQUIRE(Pihd::FirstSegmentIdxForHeight(513) == 1);
	REQUIRE(Pihd::FirstSegmentIdxForHeight(1024) == 1);
	REQUIRE(Pihd::FirstSegmentIdxForHeight(1025) == 2);

	// The first segment starts at height 1, not at genesis (height 0).
	REQUIRE(Pihd::FirstSegmentIdxForHeight(0) == 0);
}
