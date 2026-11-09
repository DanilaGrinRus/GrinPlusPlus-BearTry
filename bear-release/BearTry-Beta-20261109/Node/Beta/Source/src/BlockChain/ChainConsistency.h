#pragma once

#include <cstdint>

// Startup consistency check for the three append-only chain stores:
//   * header MMR hash file (header_mmr.bin) - one leaf per header
//   * candidate chain data file (candidate.chain) - 32-byte hash per entry
//   * confirmed chain data file (confirmed.chain)
//
// They are committed independently, so a crash between the two commits leaves
// the MMR and the chain at different heights. Loading such a state used to read
// a header beyond the MMR (access violation). The repair plan makes both sides
// agree again, preferring the chain index (it is what the rest of the DB refers
// to); extra MMR leaves are safe to drop and missing ones are recovered from the
// chain index.
struct ChainRepairPlan
{
	bool RewindHeaderMmr = false;
	bool RewindCandidate = false;
	bool RewindConfirmed = false;
	uint64_t TargetEntries = 0;
	uint64_t TargetMmrEntries = 0;
	uint64_t TargetCandidateEntries = 0;
	uint64_t TargetConfirmedEntries = 0;
	bool IsConsistent() const noexcept
	{
		return !RewindHeaderMmr && !RewindCandidate && !RewindConfirmed;
	}
};

inline ChainRepairPlan ComputeChainRepair(
	const uint64_t headerMmrLeaves,
	const uint64_t candidateEntries,
	const uint64_t confirmedEntries) noexcept
{
	// The chain index is the reference: the confirmed chain may never be ahead
	// of the candidate chain, and the header MMR may not be ahead of the chain.
	uint64_t targetEntries = candidateEntries;
	if (headerMmrLeaves < targetEntries) {
		// Chain is ahead of the MMR: bring both chain files back to the MMR.
		targetEntries = headerMmrLeaves;
	}

	if (targetEntries == 0) {
		targetEntries = 1;
	}

	ChainRepairPlan plan;
	plan.TargetEntries = targetEntries;
	plan.TargetMmrEntries = targetEntries;
	plan.TargetCandidateEntries = targetEntries;
	plan.TargetConfirmedEntries = (std::max)((uint64_t)1, (std::min)(confirmedEntries, targetEntries));
	plan.RewindHeaderMmr = headerMmrLeaves > targetEntries;
	plan.RewindCandidate = candidateEntries > targetEntries;
	plan.RewindConfirmed = confirmedEntries > plan.TargetConfirmedEntries;
	return plan;
}
// Repair plan driven by the last atomic chain.checkpoint. The checkpoint is an
// upper bound: stores may lag behind it after a crash (uncommitted writes are
// dropped) but never beyond it. Every store is truncated to the minimum of its
// own size and the checkpoint, and the candidate/confirmed heights are clamped
// to the resulting MMR so the invariant leaves == candidate entries holds.
inline ChainRepairPlan ComputeChainRepairToCheckpoint(
	const uint64_t headerMmrLeaves,
	const uint64_t candidateEntries,
	const uint64_t confirmedEntries,
	const uint64_t checkpointMmrLeaves,
	const uint64_t checkpointCandidateEntries,
	const uint64_t checkpointConfirmedEntries) noexcept
{
	// Never write forward: the target is bounded by both the checkpoint and the
	// stores themselves. If one store lost its last commit (e.g. the chain index
	// is behind while the MMR is ahead), truncate the other one too, otherwise
	// the invariant leaves == candidate entries would be violated.
	uint64_t target = (std::min)(
		(std::min)(headerMmrLeaves, candidateEntries),
		(std::min)(checkpointMmrLeaves, checkpointCandidateEntries));
	uint64_t targetConfirmed = (std::min)(
		(std::min)(confirmedEntries, checkpointConfirmedEntries),
		target);

	if (target == 0) {
		target = 1;
	}
	if (targetConfirmed == 0) {
		targetConfirmed = 1;
	}

	const uint64_t targetMmr = target;
	const uint64_t targetCandidate = target;

	ChainRepairPlan plan;
	plan.TargetEntries = targetCandidate;
	plan.TargetMmrEntries = targetMmr;
	plan.TargetCandidateEntries = targetCandidate;
	plan.TargetConfirmedEntries = targetConfirmed;
	plan.RewindHeaderMmr = headerMmrLeaves > targetMmr;
	plan.RewindCandidate = candidateEntries > targetCandidate;
	plan.RewindConfirmed = confirmedEntries > targetConfirmed;
	return plan;
}