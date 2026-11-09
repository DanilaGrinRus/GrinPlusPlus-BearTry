#pragma once

#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/RangeProof.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class TxHashSet;
class SyncStatus;

// G-49 experimental parallel rangeproof verification.
//
// Invariants:
//  - one sequential reader enumerates the immutable staging PMMRs;
//  - a bounded queue holds at most a fixed number of batches;
//  - each worker verifies through an independent Bulletproofs instance
//    (own secp256k1 context, generators and scratch space);
//  - any failed batch, worker exception, read error, cancellation or
//    incomplete submission set makes the whole operation fail closed;
//  - the caller only reports success after every produced batch verified and
//    the verified counts equal the read counts.
class RangeProofBatchVerifier
{
public:
	struct Batch
	{
		uint64_t FirstLeafPosition = 0;
		uint64_t LastLeafPosition = 0;
		std::vector<std::pair<Commitment, RangeProof>> Proofs;
	};

	struct Stats
	{
		uint64_t SubmittedBatches = 0;
		uint64_t VerifiedBatches = 0;
		uint64_t SubmittedProofs = 0;
		uint64_t VerifiedProofs = 0;
		uint64_t MaxQueueDepth = 0;
		uint64_t WorkerBusyMs = 0;
		bool ThreadStartFailed = false;
		std::vector<long long> BatchDurations;
	};

	using BatchFn = std::function<bool(size_t workerIndex, const Batch& batch)>;
	using SubmitFn = std::function<bool(Batch&& batch)>;
	using ProduceFn = std::function<bool(const SubmitFn& submit)>;
	using ProgressFn = std::function<void(uint64_t verifiedProofs)>;
	using CancelFn = std::function<bool()>;

	struct JobMemoryStatus
	{
		bool Known = false;       // false: query failed; not evidence of no limit
		bool Limited = false;     // true: at least one job/process memory limit is set
		uint64_t RemainingBytes = 0;
	};

	// Pure Windows Job Object accounting model, exposed for tests. Flags use the
	// JOB_OBJECT_LIMIT_PROCESS_MEMORY / JOB_OBJECT_LIMIT_JOB_MEMORY bits.
	static JobMemoryStatus EvaluateJobMemory(
		bool queryOk,
		bool inJob,
		uint32_t limitFlags,
		uint64_t processLimitBytes,
		uint64_t jobLimitBytes,
		uint64_t processUsedBytes,
		uint64_t jobUsedBytes);

	struct WorkerSelection
	{
		int Workers = 1;
		bool Automatic = false;
		int PhysicalCores = 0;
		int AllowedLogicalCores = 0;
		uint64_t AvailableMb = 0;
		bool JobMemoryKnown = true;
		bool JobMemoryLimited = false;
		uint64_t JobRemainingMb = 0;
		std::string Reason;
	};

	// Pure policy for tests. overrideValue is the raw GRINPP_RANGEPROOF_WORKERS
	// value: empty/"auto" -> automatic, 1..4 manual, >4 clamped, invalid falls
	// back to automatic. allowedLogicalCores (0 = unknown) is the process
	// affinity mask, not the machine's logical count. The automatic policy never
	// counts SMT threads as physical cores, leaves one physical core for the node
	// when possible, caps at four workers, and reduces on low available memory.
	static WorkerSelection ResolveWorkerPolicy(
		int physicalCores,
		uint64_t availableMb,
		const std::string& overrideValue,
		int allowedLogicalCores = 0);

	// Probes the host and applies ResolveWorkerPolicy; logs G49W selection.
	static WorkerSelection ConfiguredWorkerSelection();
	static int ConfiguredWorkerCount();

	static bool Verify(TxHashSet& txHashSet, SyncStatus& syncStatus, Stats* pStats = nullptr);
	static bool VerifySequential(TxHashSet& txHashSet, SyncStatus& syncStatus, Stats* pStats = nullptr);
	static bool VerifyParallel(TxHashSet& txHashSet, SyncStatus& syncStatus, int workers, Stats* pStats = nullptr);

	// Generic bounded run loop. Shared by the real verification path and by the
	// synthetic self-tests (workers, failure injection, cancellation, queue bound).
	static bool Execute(
		int workers,
		size_t queueCapacity,
		const CancelFn& cancelled,
		const BatchFn& verify,
		const ProduceFn& produce,
		const ProgressFn& progress,
		Stats* pStats);

};





