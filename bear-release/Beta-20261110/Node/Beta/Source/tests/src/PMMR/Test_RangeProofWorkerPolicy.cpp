#include <catch.hpp>

#include <PMMR/RangeProofBatchVerifier.h>

namespace
{
	int Workers(const int physicalCores, const uint64_t availableMb, const std::string& overrideValue)
	{
		return RangeProofBatchVerifier::ResolveWorkerPolicy(
			physicalCores, availableMb, overrideValue).Workers;
	}
}

TEST_CASE("RangeProof worker auto policy is bounded and memory-aware", "[workers]")
{
	// Automatic: one physical core is left for the node when there is more than
	// one; SMT is not assumed and the experimental cap is four.
	REQUIRE(Workers(1, 8192, "auto") == 1);
	REQUIRE(Workers(2, 8192, "auto") == 2);
	REQUIRE(Workers(3, 8192, "auto") == 2);
	REQUIRE(Workers(4, 8192, "auto") == 3);
	REQUIRE(Workers(8, 8192, "auto") == 4);
	REQUIRE(Workers(16, 65536, "AUTO") == 4);

	// Unknown memory is conservative; low memory reduces the worker count.
	REQUIRE(Workers(8, 0, "auto") == 1);
	REQUIRE(Workers(8, 1024, "auto") == 1);
	REQUIRE(Workers(8, 1800, "auto") == 2);
	REQUIRE(Workers(8, 2048, "auto") == 3);
	REQUIRE(Workers(8, 2304, "auto") == 4);

	// Compatibility default stays one worker; auto is explicit.
	REQUIRE(Workers(8, 65536, "") == 1);
}

TEST_CASE("Job memory limits use flags, committed usage and exact remainder", "[workers]")
{
	constexpr uint32_t PROCESS_MEMORY = 0x00000100;
	constexpr uint32_t JOB_MEMORY = 0x00000200;
	constexpr uint64_t MB = 1024 * 1024;

	auto unknown = RangeProofBatchVerifier::EvaluateJobMemory(
		false, true, PROCESS_MEMORY | JOB_MEMORY, 1, 1, 0, 0);
	REQUIRE_FALSE(unknown.Known);
	REQUIRE_FALSE(unknown.Limited);

	auto noJob = RangeProofBatchVerifier::EvaluateJobMemory(true, false, 0, 0, 0, 0, 0);
	REQUIRE(noJob.Known);
	REQUIRE_FALSE(noJob.Limited);

	auto noFlags = RangeProofBatchVerifier::EvaluateJobMemory(
		true, true, 0, 100 * MB, 100 * MB, 90 * MB, 90 * MB);
	REQUIRE(noFlags.Known);
	REQUIRE_FALSE(noFlags.Limited);

	auto processLimited = RangeProofBatchVerifier::EvaluateJobMemory(
		true, true, PROCESS_MEMORY, 100 * MB, 0, 100 * MB - 512 * 1024, 0);
	REQUIRE(processLimited.Known);
	REQUIRE(processLimited.Limited);
	REQUIRE(processLimited.RemainingBytes == 512 * 1024); // < 1 MiB is exact, not zero/unknown

	auto exhausted = RangeProofBatchVerifier::EvaluateJobMemory(
		true, true, JOB_MEMORY, 0, 100 * MB, 0, 101 * MB);
	REQUIRE(exhausted.Known);
	REQUIRE(exhausted.Limited);
	REQUIRE(exhausted.RemainingBytes == 0);

	auto both = RangeProofBatchVerifier::EvaluateJobMemory(
		true, true, PROCESS_MEMORY | JOB_MEMORY, 100 * MB, 200 * MB, 99 * MB, 150 * MB);
	REQUIRE(both.RemainingBytes == 1 * MB); // process remainder is the smaller one
}

TEST_CASE("RangeProof worker policy respects process affinity", "[workers]")
{
	const auto twoLogical = RangeProofBatchVerifier::ResolveWorkerPolicy(8, 8192, "auto", 2);
	REQUIRE(twoLogical.Workers == 2);
	REQUIRE(twoLogical.AllowedLogicalCores == 2);

	const auto oneLogical = RangeProofBatchVerifier::ResolveWorkerPolicy(8, 8192, "auto", 1);
	REQUIRE(oneLogical.Workers == 1);

	// A manual value cannot oversubscribe the process affinity mask.
	const auto manualCapped = RangeProofBatchVerifier::ResolveWorkerPolicy(8, 8192, "4", 2);
	REQUIRE(manualCapped.Workers == 2);
	REQUIRE(manualCapped.Reason.find("affinity") != std::string::npos);

	// Affinity is not a memory substitute: low memory still reduces to one.
	const auto lowMemory = RangeProofBatchVerifier::ResolveWorkerPolicy(8, 1024, "auto", 8);
	REQUIRE(lowMemory.Workers == 1);
}

TEST_CASE("RangeProof worker manual override wins and invalid values fall back", "[workers]")
{
	const auto manual = RangeProofBatchVerifier::ResolveWorkerPolicy(1, 1024, "2");
	REQUIRE(manual.Workers == 2);
	REQUIRE_FALSE(manual.Automatic);

	REQUIRE(Workers(16, 65536, "1") == 1);
	REQUIRE(Workers(16, 65536, "4") == 4);
	REQUIRE(Workers(16, 65536, "8") == 4); // explicit cap

	// Invalid/zero values are not silently treated as a manual one-worker choice;
	// they fall back to the automatic policy and report that in the reason.
	const auto invalid = RangeProofBatchVerifier::ResolveWorkerPolicy(4, 8192, "banana");
	REQUIRE(invalid.Automatic);
	REQUIRE(invalid.Workers == 3);
	REQUIRE(invalid.Reason.find("invalid") != std::string::npos);

	const auto zero = RangeProofBatchVerifier::ResolveWorkerPolicy(4, 8192, "0");
	REQUIRE(zero.Automatic);
	REQUIRE(zero.Workers == 3);
}


