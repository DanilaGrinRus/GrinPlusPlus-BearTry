#include <catch.hpp>
#include <PMMR/RangeProofBatchVerifier.h>
#include <PMMR/TxHashSetImpl.h>
#include <PMMR/Common/MMRUtil.h>
#include <P2P/SyncStatus.h>
#include <Common/Logger.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <iostream>

TEST_CASE("G49 bounded worker verification")
{
	const auto runCase = [&](const std::string& name, const bool expected, const bool actual,
		const std::string& detail) {
		const bool pass = (expected == actual);
		if (pass) {
			LOG_INFO_F("G49SELFTEST case={} result=PASS {}", name, detail);
		}
		else {
			LOG_ERROR_F("G49SELFTEST case={} result=FAIL expected={} actual={} {}",
				name, expected ? "true" : "false", actual ? "true" : "false", detail);
			INFO(name << ": " << detail);
			CHECK(pass);
		}
		return pass;
	};

	const auto runBatches = [](const size_t count, const size_t lastCount,
		const RangeProofBatchVerifier::BatchFn& verify) {
		return [count, lastCount, verify](const RangeProofBatchVerifier::SubmitFn& submit) {
			for (size_t i = 0; i < count; ++i) {
				const size_t proofs = (i + 1 == count) ? lastCount : 3;
				RangeProofBatchVerifier::Batch batch;
				batch.FirstLeafPosition = i;
				batch.LastLeafPosition = i;
				for (size_t j = 0; j < proofs; ++j) {
					Commitment commitment;
					std::vector<unsigned char> proofBytes(8, static_cast<unsigned char>(i));
					batch.Proofs.emplace_back(commitment, RangeProof(std::move(proofBytes)));
				}
				if (!submit(std::move(batch))) {
					return false;
				}
			}
			return true;
		};
	};

	bool allPassed = true;

	// 1. Success path with an intentionally partial last batch.
	{
		RangeProofBatchVerifier::Stats stats;
		std::vector<unsigned char> seen;
		std::mutex seenMutex;
		const RangeProofBatchVerifier::BatchFn verify =
			[&](const size_t, const RangeProofBatchVerifier::Batch& batch) {
				std::lock_guard<std::mutex> lock(seenMutex);
				seen.push_back(batch.Proofs.front().second.GetProofBytes().front());
				return true;
			};
		const bool ok = RangeProofBatchVerifier::Execute(2, 2, RangeProofBatchVerifier::CancelFn(), verify, runBatches(7, 2, verify), RangeProofBatchVerifier::ProgressFn(), &stats);
		allPassed = runCase("success_last_partial", true, ok,
			"submitted=" + std::to_string(stats.SubmittedBatches) +
			" verified=" + std::to_string(stats.VerifiedBatches) +
			" proofs=" + std::to_string(stats.VerifiedProofs)) && allPassed;
		std::vector<unsigned char> sortedSeen = seen;
		std::sort(sortedSeen.begin(), sortedSeen.end());
		const bool noGapsOrDuplicates =
			sortedSeen.size() == 7 &&
			std::adjacent_find(sortedSeen.begin(), sortedSeen.end()) == sortedSeen.end() &&
			sortedSeen.front() == 0 && sortedSeen.back() == 6;
		allPassed = runCase("success_no_gaps_or_duplicates", true, noGapsOrDuplicates,
			"seen=" + std::to_string(seen.size())) && allPassed;
		allPassed = runCase("success_counts_match", true,
			stats.SubmittedBatches == 7 && stats.VerifiedBatches == 7 &&
			stats.SubmittedProofs == stats.VerifiedProofs &&
			stats.VerifiedProofs == static_cast<uint64_t>(20),
			"submitted_proofs=" + std::to_string(stats.SubmittedProofs)) && allPassed;
	}

	// 1b. Repetition stress: repeated 4-worker runs must be deterministic, with
	// no deadlock, no lost batch and no unbounded queue growth.
	{
		unsigned failures = 0;
		uint64_t totalVerified = 0;
		const RangeProofBatchVerifier::BatchFn verify =
			[](const size_t, const RangeProofBatchVerifier::Batch&) { return true; };
		for (unsigned rep = 0; rep < 25; ++rep) {
			RangeProofBatchVerifier::Stats stats;
			const bool ok = RangeProofBatchVerifier::Execute(4, 4, RangeProofBatchVerifier::CancelFn(), verify, runBatches(20, 7, verify), RangeProofBatchVerifier::ProgressFn(), &stats);
			if (!ok || stats.VerifiedBatches != 20 || stats.VerifiedProofs != 64 ||
				stats.MaxQueueDepth > 4) {
				++failures;
			}
			totalVerified += stats.VerifiedProofs;
		}
		allPassed = runCase("stress_25_runs_4_workers", true,
			failures == 0 && totalVerified == static_cast<uint64_t>(25 * 64),
			"failures=" + std::to_string(failures) +
			" verified=" + std::to_string(totalVerified)) && allPassed;
	}

	// 2. A failing batch must fail the whole run.
	{
		RangeProofBatchVerifier::Stats stats;
		const RangeProofBatchVerifier::BatchFn verify =
			[&](const size_t, const RangeProofBatchVerifier::Batch& batch) {
				return batch.Proofs.front().second.GetProofBytes().front() != 3;
			};
		const bool ok = RangeProofBatchVerifier::Execute(2, 2, RangeProofBatchVerifier::CancelFn(), verify, runBatches(8, 3, verify), RangeProofBatchVerifier::ProgressFn(), &stats);
		allPassed = runCase("batch_failure_fails_overall", true, ok == false,
			"verified_batches=" + std::to_string(stats.VerifiedBatches)) && allPassed;
	}

	// 3. A worker exception must fail the whole run.
	{
		RangeProofBatchVerifier::Stats stats;
		const RangeProofBatchVerifier::BatchFn verify =
			[&](const size_t, const RangeProofBatchVerifier::Batch& batch) -> bool {
				if (batch.Proofs.front().second.GetProofBytes().front() == 2) {
					throw std::runtime_error("injected worker exception");
				}
				return true;
			};
		const bool ok = RangeProofBatchVerifier::Execute(2, 2, RangeProofBatchVerifier::CancelFn(), verify, runBatches(8, 3, verify), RangeProofBatchVerifier::ProgressFn(), &stats);
		allPassed = runCase("worker_exception_fails_overall", true, ok == false,
			"verified_batches=" + std::to_string(stats.VerifiedBatches)) && allPassed;
	}

	// 4. Cancellation is an overall failure.
	{
		RangeProofBatchVerifier::Stats stats;
		std::atomic<bool> cancel{ false };
		const RangeProofBatchVerifier::BatchFn verify =
			[&](const size_t, const RangeProofBatchVerifier::Batch&) {
				cancel = true;
				return true;
			};
		const RangeProofBatchVerifier::CancelFn cancelled = [&cancel]() { return cancel.load(); };
		const bool ok = RangeProofBatchVerifier::Execute(2, 2, cancelled, verify, runBatches(8, 3, verify), RangeProofBatchVerifier::ProgressFn(), &stats);
		allPassed = runCase("cancellation_fails_overall", true, ok == false,
			"submitted=" + std::to_string(stats.SubmittedBatches)) && allPassed;
	}

	// 5. Empty batch is a hard failure, never silently skipped.
	{
		RangeProofBatchVerifier::Stats stats;
		const RangeProofBatchVerifier::BatchFn verify =
			[](const size_t, const RangeProofBatchVerifier::Batch&) { return true; };
		const RangeProofBatchVerifier::ProduceFn produce =
			[](const RangeProofBatchVerifier::SubmitFn& submit) {
				RangeProofBatchVerifier::Batch empty;
				return submit(std::move(empty));
			};
		const bool ok = RangeProofBatchVerifier::Execute(2, 2, RangeProofBatchVerifier::CancelFn(), verify, produce, RangeProofBatchVerifier::ProgressFn(), &stats);
		allPassed = runCase("empty_batch_fails_overall", true, ok == false,
			"submitted=" + std::to_string(stats.SubmittedBatches)) && allPassed;
	}

	// 6. The queue depth must stay bounded under a slow worker.
	{
		RangeProofBatchVerifier::Stats stats;
		const RangeProofBatchVerifier::BatchFn verify =
			[](const size_t, const RangeProofBatchVerifier::Batch&) {
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				return true;
			};
		const bool ok = RangeProofBatchVerifier::Execute(2, 2, RangeProofBatchVerifier::CancelFn(), verify, runBatches(40, 1, verify), RangeProofBatchVerifier::ProgressFn(), &stats);
		allPassed = runCase("bounded_queue", true, ok && stats.MaxQueueDepth <= 2,
			"max_queue_depth=" + std::to_string(stats.MaxQueueDepth)) && allPassed;
	}

	// 7. Fewer than two workers is rejected by the runner.
	{
		RangeProofBatchVerifier::Stats stats;
		const bool ok = RangeProofBatchVerifier::Execute(1, 2, RangeProofBatchVerifier::CancelFn(),
			[](const size_t, const RangeProofBatchVerifier::Batch&) { return true; },
			[](const RangeProofBatchVerifier::SubmitFn&) { return true; },
			RangeProofBatchVerifier::ProgressFn(), &stats);
		allPassed = runCase("workers_below_two_rejected", true, ok == false, "") && allPassed;
	}

	REQUIRE(allPassed);
}

TEST_CASE("G49 three worker execute preserves all batches")
{
	RangeProofBatchVerifier::Stats stats;
	const auto verify = [](const size_t, const RangeProofBatchVerifier::Batch&) { return true; };
	const auto produce = [](const RangeProofBatchVerifier::SubmitFn& submit) {
		for (size_t i = 0; i < 9; ++i) {
			RangeProofBatchVerifier::Batch batch;
			batch.FirstLeafPosition = i;
			batch.LastLeafPosition = i;
			batch.Proofs.emplace_back(Commitment(), RangeProof(std::vector<unsigned char>(8, 1)));
			if (!submit(std::move(batch))) {
				return false;
			}
		}
		return true;
	};
	REQUIRE(RangeProofBatchVerifier::Execute(
		3, 3, RangeProofBatchVerifier::CancelFn(), verify, produce,
		RangeProofBatchVerifier::ProgressFn(), &stats));
	REQUIRE(stats.SubmittedBatches == 9);
	REQUIRE(stats.VerifiedBatches == 9);
	REQUIRE(stats.SubmittedProofs == stats.VerifiedProofs);
	REQUIRE(stats.MaxQueueDepth <= 3);
}

TEST_CASE("G49 TxHashSet validates every unspent proof")
{
	const auto dir = fs::temp_directory_path() / "g49-proof-parity";
	fs::create_directories(dir / "output");
	fs::create_directories(dir / "rangeproof");
	{
		auto outputs = OutputPMMR::Load(dir);
		auto proofs = RangeProofPMMR::Load(dir);
		TxHashSet state(nullptr, outputs, proofs, nullptr);
		SyncStatus status;
		const auto& genesis = Global::GetGenesisBlock().GetOutputs().front();
		REQUIRE(RangeProofBatchVerifier::Verify(state, status));
		REQUIRE(RangeProofBatchVerifier::VerifyParallel(state, status, 3));
		REQUIRE(RangeProofBatchVerifier::VerifyParallel(state, status, 4));
		auto bytes = genesis.GetRangeProof().GetProofBytes();
		REQUIRE_FALSE(bytes.empty());
		bytes.back() ^= 1;
		outputs->Append(OutputIdentifier::FromOutput(genesis));
		proofs->Append(RangeProof(std::move(bytes)));
		REQUIRE_FALSE(RangeProofBatchVerifier::Verify(state, status));
		REQUIRE_FALSE(RangeProofBatchVerifier::VerifyParallel(state, status, 4));
	}
	fs::remove_all(dir);
}

// Synthetic same-state benchmark. It is not a full-chain profile: it repeats the
// genesis range proof to exercise worker scaling without inventing consensus
// data. Run with GRINPP_RANGEPROOF_WORKERS=1/2/3/4 or unset for auto.
TEST_CASE("G49 worker benchmark same state", "[g49bench][.]")
{
	const auto dir = fs::temp_directory_path() / "g49-worker-benchmark";
	fs::remove_all(dir);
	fs::create_directories(dir / "output");
	fs::create_directories(dir / "rangeproof");
	const auto& genesis = Global::GetGenesisBlock().GetOutputs().front();
	auto outputs = OutputPMMR::Load(dir);
	auto proofs = RangeProofPMMR::Load(dir);
	for (size_t i = 0; i < 10000; ++i) {
		outputs->Append(OutputIdentifier::FromOutput(genesis));
		proofs->Append(RangeProof(genesis.GetRangeProof()));
	}
	{
		TxHashSet state(nullptr, outputs, proofs, nullptr);
		SyncStatus status;
		const auto selection = RangeProofBatchVerifier::ConfiguredWorkerSelection();
		const auto start = std::chrono::steady_clock::now();
		RangeProofBatchVerifier::Stats stats;
		const bool ok = RangeProofBatchVerifier::Verify(state, status, &stats);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
		const uint64_t pmmrSize = outputs->GetSize();
		std::cout << "G49BENCH workers=" << selection.Workers
			<< " auto=" << (selection.Automatic ? 1 : 0)
			<< " pmmr_size=" << pmmrSize
			<< " proofs_verified=" << stats.VerifiedProofs
			<< " proofs_submitted=" << stats.SubmittedProofs
			<< " ms=" << elapsed
			<< " ok=" << (ok ? 1 : 0) << std::endl;
		REQUIRE(ok);
	}
	outputs.reset();
	proofs.reset();
	std::error_code cleanupError;
	fs::remove_all(dir, cleanupError);
}


