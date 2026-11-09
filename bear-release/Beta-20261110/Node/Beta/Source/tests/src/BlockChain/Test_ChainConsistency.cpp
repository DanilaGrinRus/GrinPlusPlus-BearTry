#include <catch.hpp>

#include <BlockChain/ChainConsistency.h>

TEST_CASE("ChainConsistency::ConsistentStoresNeedNoRepair")
{
	const ChainRepairPlan plan = ComputeChainRepair(100, 100, 50);
	REQUIRE(plan.IsConsistent());
}

TEST_CASE("ChainConsistency::HeaderMmrAheadIsRewoundToChain")
{
	// Crash after committing extra MMR leaves but before the chain index.
	const ChainRepairPlan plan = ComputeChainRepair(120, 100, 50);
	REQUIRE(!plan.IsConsistent());
	REQUIRE(plan.RewindHeaderMmr);
	REQUIRE(!plan.RewindCandidate);
	REQUIRE(!plan.RewindConfirmed);
	REQUIRE(plan.TargetEntries == 100);
}

TEST_CASE("ChainConsistency::ChainAheadIsRewoundToHeaderMmr")
{
	// Crash after committing chain index entries but before the MMR.
	const ChainRepairPlan plan = ComputeChainRepair(80, 100, 50);
	REQUIRE(!plan.IsConsistent());
	REQUIRE(!plan.RewindHeaderMmr);
	REQUIRE(plan.RewindCandidate);
	// The confirmed chain at 51 entries is already below the target and must not
	// be rewound forward.
	REQUIRE(!plan.RewindConfirmed);
	REQUIRE(plan.TargetEntries == 80);
}

TEST_CASE("ChainConsistency::ConfirmedChainCannotBeAheadOfCandidate")
{
	const ChainRepairPlan plan = ComputeChainRepair(100, 100, 120);
	REQUIRE(!plan.IsConsistent());
	REQUIRE(!plan.RewindHeaderMmr);
	REQUIRE(!plan.RewindCandidate);
	REQUIRE(plan.RewindConfirmed);
	REQUIRE(plan.TargetEntries == 100);
}
TEST_CASE("ChainConsistency::CheckpointTruncatesStoresAhead")
{
	// A crash committed the MMR and candidate chain beyond the last checkpoint.
	const ChainRepairPlan plan = ComputeChainRepairToCheckpoint(
		120, 105, 50,
		100, 100, 60);
	REQUIRE(!plan.IsConsistent());
	REQUIRE(plan.RewindHeaderMmr);
	REQUIRE(plan.RewindCandidate);
	REQUIRE(!plan.RewindConfirmed);
	REQUIRE(plan.TargetMmrEntries == 100);
	REQUIRE(plan.TargetCandidateEntries == 100);
	REQUIRE(plan.TargetConfirmedEntries == 50);
}

TEST_CASE("ChainConsistency::CheckpointNeverRewindsForward")
{
	// Stores lag the checkpoint (the last writes were not durable): keep the
	// actual, consistent state instead of inventing entries.
	const ChainRepairPlan plan = ComputeChainRepairToCheckpoint(
		80, 80, 50,
		100, 100, 100);
	REQUIRE(plan.IsConsistent());
	REQUIRE(plan.TargetMmrEntries == 80);
	REQUIRE(plan.TargetCandidateEntries == 80);
	REQUIRE(plan.TargetConfirmedEntries == 50);
}

TEST_CASE("ChainConsistency::CheckpointClampsConfirmedChain")
{
	const ChainRepairPlan plan = ComputeChainRepairToCheckpoint(
		100, 100, 120,
		100, 100, 100);
	REQUIRE(!plan.IsConsistent());
	REQUIRE(plan.RewindConfirmed);
	REQUIRE(!plan.RewindHeaderMmr);
	REQUIRE(!plan.RewindCandidate);
	REQUIRE(plan.TargetConfirmedEntries == 100);
}
TEST_CASE("ChainConsistency::CheckpointTruncatesMmrWhenChainLost")
{
	// MMR commit was durable, the candidate chain commit was not.
	const ChainRepairPlan plan = ComputeChainRepairToCheckpoint(
		100, 90, 80,
		100, 100, 100);
	REQUIRE(!plan.IsConsistent());
	REQUIRE(plan.RewindHeaderMmr);
	REQUIRE(!plan.RewindCandidate);
	REQUIRE(!plan.RewindConfirmed);
	REQUIRE(plan.TargetMmrEntries == 90);
	REQUIRE(plan.TargetCandidateEntries == 90);
	REQUIRE(plan.TargetConfirmedEntries == 80);
}