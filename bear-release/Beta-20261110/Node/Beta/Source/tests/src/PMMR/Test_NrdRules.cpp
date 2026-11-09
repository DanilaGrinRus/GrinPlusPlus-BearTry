#include <catch.hpp>

#include <Consensus.h>
#include <PMMR/NrdRules.h>

TEST_CASE("NrdRules relative height matches grin")
{
	// relative_height=2: a duplicate at the next height is invalid, at +2 valid.
	REQUIRE_FALSE(NrdRules::IsRelativeHeightSatisfied(9, 10, 2, 0));
	REQUIRE(NrdRules::IsRelativeHeightSatisfied(9, 11, 2, 0));

	// relative_height=1 allows the next block.
	REQUIRE(NrdRules::IsRelativeHeightSatisfied(9, 10, 1, 0));

	// A previous entry older than the two-week cutoff must be ignored.
	// At the cutoff boundary the entry is still considered: 1100-100 < 1440.
	REQUIRE_FALSE(NrdRules::IsRelativeHeightSatisfied(100, 1100, 1440, 100));
	// One height before the cutoff is ignored.
	REQUIRE(NrdRules::IsRelativeHeightSatisfied(99, 1100, 1440, 100));
	REQUIRE(Consensus::WEEK_HEIGHT == 7 * Consensus::DAY_HEIGHT);
}

TEST_CASE("NrdRules rejects a duplicate kernel in one block")
{
	std::map<Commitment, uint64_t> blockKernels;
	const Commitment excess = Commitment(CBigInteger<33>::ValueOf(7));
	REQUIRE(NrdRules::RecordBlockKernel(blockKernels, excess, 10));
	REQUIRE_FALSE(NrdRules::RecordBlockKernel(blockKernels, excess, 10));
}

TEST_CASE("NrdRules boundary cases match Rust saturating semantics")
{
	// Rust computes saturating_sub(current, previous); a decreasing height
	// therefore yields zero and cannot satisfy any positive relative height.
	REQUIRE_FALSE(NrdRules::IsRelativeHeightSatisfied(10, 9, 1, 0));
	REQUIRE(NrdRules::IsRelativeHeightSatisfied(10, 10, 0, 0));

	// Exact boundary: diff == relative_height is valid.
	REQUIRE(NrdRules::IsRelativeHeightSatisfied(100, 1444, 1344, 0));

	// Cutoff boundary: previous == cutoff is considered, previous < cutoff is not.
	REQUIRE_FALSE(NrdRules::IsRelativeHeightSatisfied(1000, 1001, 2, 1000));
	REQUIRE(NrdRules::IsRelativeHeightSatisfied(999, 1001, 2, 1000));
}

TEST_CASE("NrdRules maximum relative height boundary matches Rust WEEK_HEIGHT")
{
	// Rust NRDRelativeHeight::MAX == consensus::WEEK_HEIGHT (1..=MAX only).
	REQUIRE(Consensus::WEEK_HEIGHT == 7 * Consensus::DAY_HEIGHT);

	const uint64_t relative = Consensus::WEEK_HEIGHT;
	const uint64_t previous = 5'000'000;

	// diff == MAX is accepted, diff == MAX - 1 is rejected.
	REQUIRE(NrdRules::IsRelativeHeightSatisfied(previous, previous + relative, relative, 0));
	REQUIRE_FALSE(NrdRules::IsRelativeHeightSatisfied(previous, previous + relative - 1, relative, 0));

	// Larger diffs stay accepted and decreasing heights stay rejected
	// (Rust saturating_sub => diff 0 < relative_height).
	REQUIRE(NrdRules::IsRelativeHeightSatisfied(previous, previous + relative + 1, relative, 0));
	REQUIRE_FALSE(NrdRules::IsRelativeHeightSatisfied(previous + 1, previous, relative, 0));
}

TEST_CASE("NrdRules block duplicate detection is per excess")
{
	std::map<Commitment, uint64_t> blockKernels;
	const Commitment excess = Commitment(CBigInteger<33>::ValueOf(9));

	REQUIRE(NrdRules::RecordBlockKernel(blockKernels, excess, 100));
	REQUIRE_FALSE(NrdRules::RecordBlockKernel(blockKernels, excess, 100));

	// Two different excesses are independent.
	const Commitment other = Commitment(CBigInteger<33>::ValueOf(10));
	REQUIRE(NrdRules::RecordBlockKernel(blockKernels, other, 100));
}
