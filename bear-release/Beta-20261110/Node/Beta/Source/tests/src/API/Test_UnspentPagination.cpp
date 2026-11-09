#include <catch.hpp>

#include <API/Node/Handlers/GetUnspentOutputsHandler.h>

#include <cstdint>
#include <limits>
#include <optional>

namespace
{
	constexpr uint64_t MAX = (std::numeric_limits<uint64_t>::max)();
	constexpr uint64_t MAX_POSITIONS = 8192;
}

TEST_CASE("Unspent outputs pagination: fresh chain, start=1")
{
	// 20 leaves occupy positions 0..37 (MMR size 38), as in Rust's PMMR tests.
	const auto plan = GetUnspentOutputsHandler::PlanScan(1, std::nullopt, 38, MAX_POSITIONS);
	REQUIRE(plan.hasWork);
	REQUIRE(plan.startPosition == 0);
	REQUIRE(plan.scanEndPosition == 37);
	REQUIRE(plan.bound == 38);
	REQUIRE(plan.highestIndex == 38);
}

TEST_CASE("Unspent outputs pagination: bounded range")
{
	// Rust test: elements_from_pmmr_index(8, 1000, Some(34)) starts at position 7.
	const auto plan = GetUnspentOutputsHandler::PlanScan(8, std::optional<uint64_t>(34), 38, MAX_POSITIONS);
	REQUIRE(plan.hasWork);
	REQUIRE(plan.startPosition == 7);
	REQUIRE(plan.scanEndPosition == 33);
	REQUIRE(plan.bound == 34);
	REQUIRE(plan.highestIndex == 34);
}

TEST_CASE("Unspent outputs pagination: end beyond the MMR size")
{
	const auto plan = GetUnspentOutputsHandler::PlanScan(1, std::optional<uint64_t>(100), 38, MAX_POSITIONS);
	REQUIRE(plan.hasWork);
	REQUIRE(plan.startPosition == 0);
	REQUIRE(plan.scanEndPosition == 37);
	REQUIRE(plan.bound == 100);
	REQUIRE(plan.highestIndex == 100);
}

TEST_CASE("Unspent outputs pagination: empty and out-of-range requests")
{
	auto plan = GetUnspentOutputsHandler::PlanScan(100, std::nullopt, 38, MAX_POSITIONS);
	REQUIRE_FALSE(plan.hasWork);
	REQUIRE(plan.startPosition == 99);
	REQUIRE(plan.highestIndex == 38);

	plan = GetUnspentOutputsHandler::PlanScan(1, std::optional<uint64_t>(0), 38, MAX_POSITIONS);
	REQUIRE_FALSE(plan.hasWork);
	REQUIRE(plan.bound == 0);

	plan = GetUnspentOutputsHandler::PlanScan(10, std::optional<uint64_t>(5), 38, MAX_POSITIONS);
	REQUIRE_FALSE(plan.hasWork);
	REQUIRE(plan.startPosition == 9);
	REQUIRE(plan.bound == 5);

	plan = GetUnspentOutputsHandler::PlanScan(1, std::nullopt, 38, 0);
	REQUIRE_FALSE(plan.hasWork);
}

TEST_CASE("Unspent outputs pagination: scan cap and extreme indices")
{
	uint64_t plan = 0;
	{
		const auto capped = GetUnspentOutputsHandler::PlanScan(1, std::nullopt, 1000000, MAX_POSITIONS);
		REQUIRE(capped.hasWork);
		REQUIRE(capped.startPosition == 0);
		REQUIRE(capped.scanEndPosition == MAX_POSITIONS - 1);
		plan = capped.scanEndPosition;
	}
	REQUIRE(plan == 8191);

	// start = UINT64_MAX: saturating conversion yields UINT64_MAX - 1.
	{
		const auto plan2 = GetUnspentOutputsHandler::PlanScan(MAX, std::nullopt, MAX, MAX_POSITIONS);
		REQUIRE(plan2.hasWork);
		REQUIRE(plan2.startPosition == MAX - 1);
		REQUIRE(plan2.scanEndPosition == MAX - 1);
	}

	// start = 0 is valid and scans from position 0.
	{
		const auto plan3 = GetUnspentOutputsHandler::PlanScan(0, std::nullopt, 38, MAX_POSITIONS);
		REQUIRE(plan3.hasWork);
		REQUIRE(plan3.startPosition == 0);
		REQUIRE(plan3.scanEndPosition == 37);
	}
}
