#include <catch.hpp>

#include <PMMR/Common/MmrStructure.h>

TEST_CASE("MmrStructure::NodesForLeaves")
{
	REQUIRE(MmrStructure::NodesForLeaves(0) == 0);
	REQUIRE(MmrStructure::NodesForLeaves(1) == 1);
	REQUIRE(MmrStructure::NodesForLeaves(2) == 3);
	REQUIRE(MmrStructure::NodesForLeaves(3) == 4);
	REQUIRE(MmrStructure::NodesForLeaves(4) == 7);
	REQUIRE(MmrStructure::NodesForLeaves(5) == 8);
	REQUIRE(MmrStructure::NodesForLeaves(6) == 10);
	REQUIRE(MmrStructure::NodesForLeaves(7) == 11);
	REQUIRE(MmrStructure::NodesForLeaves(8) == 15);
}

TEST_CASE("MmrStructure::RejectsImpossibleNodeCounts")
{
	REQUIRE(MmrStructure::IsValidNodeCount(0));
	REQUIRE(MmrStructure::IsValidNodeCount(1));
	REQUIRE(MmrStructure::IsValidNodeCount(3));
	REQUIRE(MmrStructure::IsValidNodeCount(4));
	REQUIRE(MmrStructure::IsValidNodeCount(7));

	REQUIRE(!MmrStructure::IsValidNodeCount(2));
	REQUIRE(!MmrStructure::IsValidNodeCount(5));
	REQUIRE(!MmrStructure::IsValidNodeCount(6));
	REQUIRE(!MmrStructure::IsValidNodeCount(9));
	REQUIRE(!MmrStructure::IsValidNodeCount(12));
}

TEST_CASE("MmrStructure::LargestValidNodeCountAndInverse")
{
	REQUIRE(MmrStructure::LargestValidNodeCount(0) == 0);
	REQUIRE(MmrStructure::LargestValidNodeCount(2) == 1);
	REQUIRE(MmrStructure::LargestValidNodeCount(5) == 4);
	REQUIRE(MmrStructure::LargestValidNodeCount(12) == 11);
	REQUIRE(MmrStructure::LargestValidNodeCount(15) == 15);

	REQUIRE(MmrStructure::LeavesForNodes(11) == 7);
	REQUIRE(MmrStructure::LeavesForNodes(12) == 0);
	REQUIRE(MmrStructure::LeavesForNodes(0) == 0);
}
TEST_CASE("MmrStructure::RoundTripsLargeHeaderMmrSizes")
{
	// Regression: the previous binary-search upper bound was below the real leaf
	// count for large MMRs (L is slightly larger than nodes/2), which truncated a
	// healthy 3.3M-header MMR to zero.
	const uint64_t leaves[] = {
		16, 1000, 100000, 3317054, 3317028
	};
	for (const uint64_t l : leaves) {
		const uint64_t nodes = MmrStructure::NodesForLeaves(l);
		REQUIRE(MmrStructure::IsValidNodeCount(nodes));
		REQUIRE(MmrStructure::LeavesForNodes(nodes) == l);
	}
}