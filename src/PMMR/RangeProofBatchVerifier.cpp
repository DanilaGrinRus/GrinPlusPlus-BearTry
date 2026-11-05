#include "RangeProofBatchVerifier.h"

// TxHashSetImpl.h must come first: it supplies FullBlock before the PMMR headers
// that use it in inline genesis accessors.
#include "TxHashSetImpl.h"
#include "../Crypto/Bulletproofs.h"

#include <Common/Logger.h>
#include <Crypto/Crypto.h>
#include <P2P/SyncStatus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cctype>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#else
#include <unistd.h>
#endif

namespace
{
	using Batch = RangeProofBatchVerifier::Batch;
	using Stats = RangeProofBatchVerifier::Stats;

	constexpr size_t BATCH_SIZE = 1000;
	constexpr int MAX_EXPERIMENTAL_WORKERS = 4;
	constexpr uint64_t AUTO_MIN_AVAILABLE_MB = 1536;
	constexpr uint64_t AUTO_PER_WORKER_MB = 256;

	long long G49NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::string ReadEnvironment(const char* name)
	{
#ifdef _WIN32
		char* buffer = nullptr;
		size_t size = 0;
		if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
			std::string value(buffer);
			free(buffer);
			return value;
		}
		return std::string();
#else
		const char* value = std::getenv(name);
		return value == nullptr ? std::string() : std::string(value);
#endif
	}

	struct Enumeration
	{
		uint64_t Proofs = 0;
		uint64_t Batches = 0;
		uint64_t LastBatch = 0;
		uint64_t LastLeafPosition = 0;
	};

	// One sequential reader. The staging PMMRs are immutable for the duration of
	// validation (nothing writes them before activation), so the reader never
	// races a writer. `onBatch` takes ownership of each batch and returns false to
	// stop; a missing rangeproof or an exception is a hard failure.
	bool EnumerateBatches(
		TxHashSet& txHashSet,
		const std::function<bool(Batch&&)>& onBatch,
		Enumeration& enumeration)
	{
		Batch current;
		const uint64_t outputMmrSize = txHashSet.GetOutputPMMR()->GetSize();
		for (LeafIndex leafIdx = LeafIndex::At(0); leafIdx.GetPosition() < outputMmrSize; leafIdx++)
		{
			std::unique_ptr<OutputIdentifier> pOutput = txHashSet.GetOutputPMMR()->GetAt(leafIdx);
			if (pOutput == nullptr) {
				if (txHashSet.GetOutputPMMR()->IsUnpruned(leafIdx)) {
					LOG_ERROR_F("Unreadable unspent output at {}", leafIdx);
					return false;
				}
				continue;
			}

			std::unique_ptr<RangeProof> pRangeProof = txHashSet.GetRangeProofPMMR()->GetAt(leafIdx);
			if (pRangeProof == nullptr) {
				LOG_ERROR_F("No rangeproof found at leaf index ({})", leafIdx);
				return false;
			}

			if (current.Proofs.empty()) {
				current.FirstLeafPosition = leafIdx.GetPosition();
			}
			current.LastLeafPosition = leafIdx.GetPosition();
			current.Proofs.emplace_back(pOutput->GetCommitment(), *pRangeProof);
			++enumeration.Proofs;
			enumeration.LastLeafPosition = leafIdx.GetPosition();

			if (current.Proofs.size() >= BATCH_SIZE)
			{
				enumeration.LastBatch = current.Proofs.size();
				++enumeration.Batches;
				if (!onBatch(std::move(current))) {
					return false;
				}
				current = Batch();
			}

		}

		if (!current.Proofs.empty())
		{
			enumeration.LastBatch = current.Proofs.size();
			++enumeration.Batches;
			if (!onBatch(std::move(current))) {
				return false;
			}
		}

		return true;
	}

	void UpdateProgress(
		SyncStatus& syncStatus,
		const uint64_t verifiedProofs,
		const uint64_t outputMmrSize)
	{
		// Conservative progress: verified unspent proofs divided by the output MMR
		// size (an upper bound). It can never claim more than actually verified.
		double fraction = 0.0;
		if (outputMmrSize != 0) {
			fraction = static_cast<double>(verifiedProofs) / static_cast<double>(outputMmrSize);
		}
		double progress = 40.0 + (30.0 * fraction);
		if (progress > 70.0) {
			progress = 70.0;
		}
		syncStatus.UpdateProcessingStatus(static_cast<uint8_t>(progress));
	}

	long long MedianMs(std::vector<long long> values)
	{
		if (values.empty()) {
			return 0;
		}
		std::sort(values.begin(), values.end());
		return values[values.size() / 2];
	}

	void LogBatchSummary(
		const char* mode,
		const int workers,
		const Enumeration& enumeration,
		const uint64_t verifiedBatches,
		const uint64_t verifiedProofs,
		const uint64_t maxQueueDepth,
		const uint64_t queueCapacity,
		const uint64_t workerBusyMs,
		const std::vector<long long>& batchDurations,
		const long long elapsedMs)
	{
		long long minBatchMs = 0;
		long long maxBatchMs = 0;
		long long totalBatchMs = 0;
		for (const long long duration : batchDurations) {
			if (minBatchMs == 0 || duration < minBatchMs) {
				minBatchMs = duration;
			}
			if (duration > maxBatchMs) {
				maxBatchMs = duration;
			}
			totalBatchMs += duration;
		}
		const long long medianBatchMs = MedianMs(batchDurations);
		const double proofsPerSecond = elapsedMs > 0
			? (static_cast<double>(verifiedProofs) * 1000.0) / static_cast<double>(elapsedMs)
			: 0.0;

		LOG_INFO_F(
			"G49T phase=range_proofs mode={} workers={} batches={} verified_batches={} "
			"proofs={} verified_proofs={} last_batch={} last_leaf_position={} ms={} "
			"proofs_per_s={:.1f} batch_ms_min={} batch_ms_median={} batch_ms_avg={:.1f} batch_ms_max={} "
			"worker_busy_ms={} max_queue_depth={} queue_capacity={}",
			mode,
			workers,
			enumeration.Batches,
			verifiedBatches,
			enumeration.Proofs,
			verifiedProofs,
			enumeration.LastBatch,
			enumeration.LastLeafPosition,
			elapsedMs,
			proofsPerSecond,
			minBatchMs,
			medianBatchMs,
			verifiedBatches == 0 ? 0.0 : static_cast<double>(totalBatchMs) / static_cast<double>(verifiedBatches),
			maxBatchMs,
			workerBusyMs,
			maxQueueDepth,
			queueCapacity);
	}
}

namespace
{
	int DetectPhysicalCores()
	{
#if defined(_WIN32)
		DWORD length = 0;
		if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) != FALSE || length == 0) {
			return 0;
		}
		std::vector<unsigned char> buffer(length);
		if (GetLogicalProcessorInformationEx(RelationProcessorCore,
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length) == FALSE) {
			return 0;
		}
		int cores = 0;
		size_t offset = 0;
		while (offset < buffer.size()) {
			const auto* pInfo = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
			if (pInfo->Relationship == RelationProcessorCore) {
				++cores;
			}
			if (pInfo->Size == 0) {
				break;
			}
			offset += pInfo->Size;
		}
		return cores;
#else
		const long logical = sysconf(_SC_NPROCESSORS_ONLN);
		if (logical <= 0) {
			return 0;
		}
		// No portable physical-core API here; keep SMT conservative.
		return std::max(1, static_cast<int>(logical) / 2);
#endif
	}

	uint64_t DetectAvailableMb()
	{
#if defined(_WIN32)
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (GlobalMemoryStatusEx(&status) != FALSE) {
			return static_cast<uint64_t>(status.ullAvailPhys / (1024 * 1024));
		}
		return 0;
#else
		const long pages = sysconf(_SC_AVPHYS_PAGES);
		const long pageSize = sysconf(_SC_PAGESIZE);
		if (pages <= 0 || pageSize <= 0) {
			return 0;
		}
		return static_cast<uint64_t>((static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize)) / (1024 * 1024));
#endif
	}

	int DetectAllowedLogicalCores()
	{
#if defined(_WIN32)
		DWORD_PTR processMask = 0;
		DWORD_PTR systemMask = 0;
		if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) != FALSE) {
			int count = 0;
			for (DWORD_PTR mask = processMask; mask != 0; mask >>= 1) {
				count += static_cast<int>(mask & 1);
			}
			return count;
		}
		return 0;
#else
		const long logical = sysconf(_SC_NPROCESSORS_ONLN);
		return logical > 0 ? static_cast<int>(logical) : 0;
#endif
	}

	// Current committed memory of all processes in the current Job. Returns
	// false when the list cannot be queried completely; that is UNKNOWN, not an
	// absent limit.
	bool GetJobCommittedBytes(uint64_t& usedBytes)
	{
#if defined(_WIN32)
		std::vector<unsigned char> buffer(64 * 1024);
		DWORD returned = 0;
		if (QueryInformationJobObject(nullptr, JobObjectBasicProcessIdList,
			buffer.data(), static_cast<DWORD>(buffer.size()), &returned) == FALSE) {
			return false;
		}
		const auto* pList = reinterpret_cast<const JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
		if (pList->NumberOfAssignedProcesses > pList->NumberOfProcessIdsInList) {
			return false;
		}
		uint64_t total = 0;
		for (DWORD i = 0; i < pList->NumberOfProcessIdsInList; ++i) {
			const DWORD pid = static_cast<DWORD>(pList->ProcessIdList[i]);
			HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (hProcess == nullptr) {
				return false;
			}
			PROCESS_MEMORY_COUNTERS_EX memory{};
			const BOOL ok = GetProcessMemoryInfo(hProcess,
				reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
			CloseHandle(hProcess);
			if (ok == FALSE) {
				return false;
			}
			total += static_cast<uint64_t>(memory.PagefileUsage);
		}
		usedBytes = total;
		return true;
#else
		(void)usedBytes;
		return false;
#endif
	}

	RangeProofBatchVerifier::JobMemoryStatus DetectJobMemory()
	{
#if defined(_WIN32)
		BOOL inJob = FALSE;
		if (IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) == FALSE) {
			return {};
		}
		if (inJob == FALSE) {
			return { true, false, 0 };
		}

		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
			&limits, sizeof(limits), nullptr) == FALSE) {
			return {};
		}

		PROCESS_MEMORY_COUNTERS_EX processMemory{};
		if (GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&processMemory),
			sizeof(processMemory)) == FALSE) {
			return {};
		}

		uint64_t jobUsed = 0;
		const bool jobUsedKnown = GetJobCommittedBytes(jobUsed);
		if (!jobUsedKnown) {
			return {};
		}

		return RangeProofBatchVerifier::EvaluateJobMemory(
			true,
			true,
			limits.BasicLimitInformation.LimitFlags,
			static_cast<uint64_t>(limits.ProcessMemoryLimit),
			static_cast<uint64_t>(limits.JobMemoryLimit),
			static_cast<uint64_t>(processMemory.PagefileUsage),
			jobUsed);
#else
		return {};
#endif
	}

	std::string Lowercase(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return value;
	}
}

RangeProofBatchVerifier::JobMemoryStatus RangeProofBatchVerifier::EvaluateJobMemory(
	const bool queryOk,
	const bool inJob,
	const uint32_t limitFlags,
	const uint64_t processLimitBytes,
	const uint64_t jobLimitBytes,
	const uint64_t processUsedBytes,
	const uint64_t jobUsedBytes)
{
	JobMemoryStatus status;
	if (!queryOk) {
		return status; // Known=false: unknown, never "no limit"
	}
	status.Known = true;
	if (!inJob) {
		return status; // known: no job memory limit
	}

	constexpr uint32_t PROCESS_MEMORY_FLAG = 0x00000100; // JOB_OBJECT_LIMIT_PROCESS_MEMORY
	constexpr uint32_t JOB_MEMORY_FLAG = 0x00000200;     // JOB_OBJECT_LIMIT_JOB_MEMORY
	const bool processLimited = (limitFlags & PROCESS_MEMORY_FLAG) != 0 && processLimitBytes > 0;
	const bool jobLimited = (limitFlags & JOB_MEMORY_FLAG) != 0 && jobLimitBytes > 0;
	if (!processLimited && !jobLimited) {
		return status; // known: no memory limit flag is set
	}

	status.Limited = true;
	uint64_t remaining = UINT64_MAX;
	if (processLimited) {
		remaining = processUsedBytes >= processLimitBytes ? 0 : processLimitBytes - processUsedBytes;
	}
	if (jobLimited) {
		const uint64_t jobRemaining = jobUsedBytes >= jobLimitBytes ? 0 : jobLimitBytes - jobUsedBytes;
		remaining = std::min(remaining, jobRemaining);
	}
	status.RemainingBytes = remaining;
	return status;
}

RangeProofBatchVerifier::WorkerSelection RangeProofBatchVerifier::ResolveWorkerPolicy(
	const int physicalCores,
	const uint64_t availableMb,
	const std::string& overrideValue,
	const int allowedLogicalCores)
{
	WorkerSelection selection;
	selection.PhysicalCores = physicalCores;
	selection.AllowedLogicalCores = allowedLogicalCores;
	selection.AvailableMb = availableMb;

	const int effectivePhysicalCores = allowedLogicalCores > 0
		? std::min(physicalCores, allowedLogicalCores)
		: physicalCores;

	const std::string value = Lowercase(overrideValue);
	if (value.empty()) {
		// Safe compatibility default: one worker. Auto is opt-in until a
		// full-state 1/2/3/4/auto comparison selects a release default.
		selection.Workers = 1;
		selection.Automatic = false;
		selection.Reason = "default one worker; set GRINPP_RANGEPROOF_WORKERS=auto to opt in";
		return selection;
	}
	if (value != "auto") {
		char* end = nullptr;
		const long parsed = std::strtol(value.c_str(), &end, 10);
		if (end != nullptr && *end == '\0' && parsed >= 1) {
			int manual = static_cast<int>(std::min<long>(parsed, MAX_EXPERIMENTAL_WORKERS));
			if (allowedLogicalCores > 0 && manual > allowedLogicalCores) {
				manual = allowedLogicalCores;
			}
			selection.Workers = std::max(1, manual);
			selection.Automatic = false;
			selection.Reason = parsed > MAX_EXPERIMENTAL_WORKERS
				? "manual override clamped to four workers"
				: "manual override";
			if (allowedLogicalCores > 0 && manual < parsed) {
				selection.Reason += " (capped by process affinity)";
			}
			return selection;
		}
		selection.Reason = "invalid manual override; using automatic";
	} else {
		selection.Reason = "automatic";
	}

	selection.Automatic = true;
	int workers = std::max(1, effectivePhysicalCores);
	if (workers > 2) {
		// Leave one physical core for the node, network and API threads.
		workers -= 1;
	}
	workers = std::min(workers, MAX_EXPERIMENTAL_WORKERS);

	if (availableMb == 0) {
		workers = 1;
		selection.Reason += " (available memory unknown; one worker)";
	} else {
		while (workers > 1 &&
			availableMb < AUTO_MIN_AVAILABLE_MB + AUTO_PER_WORKER_MB * static_cast<uint64_t>(workers - 1)) {
			--workers;
		}
		if (workers == 1 && availableMb < AUTO_MIN_AVAILABLE_MB) {
			selection.Reason += " (low available memory; one worker)";
		}
	}

	selection.Workers = std::max(1, workers);
	return selection;
}

RangeProofBatchVerifier::WorkerSelection RangeProofBatchVerifier::ConfiguredWorkerSelection()
{
	uint64_t availableMb = DetectAvailableMb();
	const JobMemoryStatus job = DetectJobMemory();
	if (!job.Known) {
		// An unknown Job limit is not evidence of an absent limit. Be
		// conservative and do not report the machine's free memory as usable.
		availableMb = 0;
	} else if (job.Limited) {
		const uint64_t jobRemainingMb = job.RemainingBytes / (1024 * 1024);
		availableMb = std::min(availableMb, jobRemainingMb);
	}

	const WorkerSelection selection = ResolveWorkerPolicy(
		DetectPhysicalCores(), availableMb,
		ReadEnvironment("GRINPP_RANGEPROOF_WORKERS"),
		DetectAllowedLogicalCores());
	WorkerSelection result = selection;
	result.JobMemoryKnown = job.Known;
	result.JobMemoryLimited = job.Limited;
	result.JobRemainingMb = job.Limited ? (job.RemainingBytes / (1024 * 1024)) : 0;
	LOG_INFO_F(
		"G49W worker_selection mode={} workers={} physical_cores={} allowed_logical={} available_mb={} job_memory_known={} job_memory_limited={} job_remaining_mb={} reason={}",
		result.Automatic ? "auto" : "manual",
		result.Workers,
		result.PhysicalCores,
		result.AllowedLogicalCores,
		result.AvailableMb,
		result.JobMemoryKnown ? 1 : 0,
		result.JobMemoryLimited ? 1 : 0,
		result.JobRemainingMb,
		result.Reason);
	return result;
}

int RangeProofBatchVerifier::ConfiguredWorkerCount()
{
	return ConfiguredWorkerSelection().Workers;
}

bool RangeProofBatchVerifier::Verify(TxHashSet& txHashSet, SyncStatus& syncStatus, Stats* pStats)
{
	const int workers = ConfiguredWorkerCount();
	if (workers <= 1) {
		return VerifySequential(txHashSet, syncStatus, pStats);
	}
	return VerifyParallel(txHashSet, syncStatus, workers, pStats);
}

bool RangeProofBatchVerifier::VerifySequential(TxHashSet& txHashSet, SyncStatus& syncStatus, Stats* pStats)
{
	const long long start = G49NowMs();
	const uint64_t outputMmrSize = txHashSet.GetOutputPMMR()->GetSize();
	Enumeration enumeration;
	std::vector<long long> batchDurations;
	uint64_t verifiedBatches = 0;
	uint64_t verifiedProofs = 0;

	const auto onBatch = [&](Batch&& batch) -> bool {
		const long long batchStart = G49NowMs();
		if (!Crypto::VerifyRangeProofs(batch.Proofs)) {
			LOG_ERROR("Failed to verify rangeproofs");
			return false;
		}
		batchDurations.push_back(G49NowMs() - batchStart);
		++verifiedBatches;
		verifiedProofs += static_cast<uint64_t>(batch.Proofs.size());
		UpdateProgress(syncStatus, batch.LastLeafPosition, outputMmrSize);
		return true;
	};

	bool success = false;
	try {
		success = EnumerateBatches(txHashSet, onBatch, enumeration);
	}
	catch (const std::exception& e) {
		LOG_ERROR_F("Rangeproof reader threw: {}", e.what());
		success = false;
	}
	catch (...) {
		LOG_ERROR("Rangeproof reader threw an unknown exception");
		success = false;
	}

	long long totalBatchMs = 0;
	for (const long long duration : batchDurations) {
		totalBatchMs += duration;
	}
	LogBatchSummary(
		"sequential",
		1,
		enumeration,
		verifiedBatches,
		verifiedProofs,
		0,
		0,
		static_cast<uint64_t>(totalBatchMs),
		batchDurations,
		G49NowMs() - start);

	if (!success) {
		LOG_ERROR("G49R result=FAIL mode=sequential");
	}
	if (pStats != nullptr) {
		Stats generated;
		generated.SubmittedBatches = enumeration.Batches;
		generated.VerifiedBatches = verifiedBatches;
		generated.SubmittedProofs = enumeration.Proofs;
		generated.VerifiedProofs = verifiedProofs;
		generated.BatchDurations = batchDurations;
		*pStats = std::move(generated);
	}
	return success;
}

bool RangeProofBatchVerifier::VerifyParallel(
	TxHashSet& txHashSet,
	SyncStatus& syncStatus,
	const int workers,
	Stats* pStats)
{
	const long long start = G49NowMs();
	const uint64_t outputMmrSize = txHashSet.GetOutputPMMR()->GetSize();
	const size_t queueCapacity = std::max<size_t>(2, static_cast<size_t>(workers) * 2);

	// Independent verifier per worker: own secp256k1 context, own generators, own
	// scratch (created inside each Verify call), own cache. Upstream secp256k1.h
	// explicitly permits sharing a context between threads for concurrent
	// read-only use, but independent instances make the read-only requirement
	// structural instead of assumption-based.
	std::vector<std::unique_ptr<Bulletproofs>> verifiers;
	verifiers.reserve(static_cast<size_t>(workers));
	try {
		for (int i = 0; i < workers; ++i) {
			verifiers.push_back(std::make_unique<Bulletproofs>());
		}
	}
	catch (const std::exception& e) {
		LOG_ERROR_F("G49R failed to create worker verifiers: {}", e.what());
		return false;
	}

	Enumeration enumeration;
	Stats stats;
	const auto verifyFn = [&verifiers](const size_t workerIndex, const Batch& batch) -> bool {
		return verifiers[workerIndex]->VerifyBulletproofs(batch.Proofs);
	};
	const auto produceFn = [&](const SubmitFn& submit) -> bool {
		return EnumerateBatches(
			txHashSet,
			[&submit](Batch&& batch) -> bool { return submit(std::move(batch)); },
			enumeration);
	};
	const auto progressFn = [&syncStatus, outputMmrSize](const uint64_t verifiedProofs) {
		UpdateProgress(syncStatus, verifiedProofs, outputMmrSize);
	};

	bool success = false;
	try {
		success = Execute(workers, queueCapacity, CancelFn(), verifyFn, produceFn, progressFn, &stats);
	}
	catch (const std::exception& e) {
		LOG_ERROR_F("G49R parallel runner threw: {}", e.what());
		success = false;
	}

	if (!success && stats.ThreadStartFailed && stats.SubmittedBatches == 0) {
		// Thread creation failed before any batch was submitted. The fail-closed
		// path is a full sequential re-read; no partial state can be activated.
		LOG_WARNING("G49R parallel worker start failed; falling back to full sequential verification");
		return VerifySequential(txHashSet, syncStatus, pStats);
	}

	// Fail closed on any inconsistency between read, submitted and verified work.
	if (success) {
		success =
			stats.SubmittedBatches == enumeration.Batches &&
			stats.VerifiedBatches == stats.SubmittedBatches &&
			stats.SubmittedProofs == enumeration.Proofs &&
			stats.VerifiedProofs == enumeration.Proofs;
		if (!success) {
			LOG_ERROR_F(
				"G49R accounting mismatch: read_batches={} submitted={} verified={} read_proofs={} submitted_proofs={} verified_proofs={}",
				enumeration.Batches,
				stats.SubmittedBatches,
				stats.VerifiedBatches,
				enumeration.Proofs,
				stats.SubmittedProofs,
				stats.VerifiedProofs);
		}
	}

	LogBatchSummary(
		"parallel",
		workers,
		enumeration,
		stats.VerifiedBatches,
		stats.VerifiedProofs,
		stats.MaxQueueDepth,
		queueCapacity,
		stats.WorkerBusyMs,
		stats.BatchDurations,
		G49NowMs() - start);

	if (!success) {
		LOG_ERROR_F("G49R result=FAIL mode=parallel workers={}", workers);
	}
	if (pStats != nullptr) {
		*pStats = stats;
	}
	return success;
}

bool RangeProofBatchVerifier::Execute(
	const int workers,
	size_t queueCapacity,
	const CancelFn& cancelled,
	const BatchFn& verify,
	const ProduceFn& produce,
	const ProgressFn& progress,
	Stats* pStats)
{
	Stats stats;
	if (workers < 2) {
		LOG_ERROR_F("G49R Execute requires at least 2 workers, got {}", workers);
		if (pStats != nullptr) {
			*pStats = stats;
		}
		return false;
	}
	if (queueCapacity == 0) {
		queueCapacity = 1;
	}

	std::mutex queueMutex;
	std::condition_variable notEmpty;
	std::condition_variable notFull;
	std::deque<Batch> queue;
	bool done = false;
	bool failed = false;
	const char* failureReason = "";

	std::mutex durationsMutex;
	std::vector<long long> batchDurations;

	std::mutex progressMutex;
	uint64_t lastReportedProgress = 0;

	std::atomic<uint64_t> submittedBatches{ 0 };
	std::atomic<uint64_t> submittedProofs{ 0 };
	std::atomic<uint64_t> verifiedBatches{ 0 };
	std::atomic<uint64_t> verifiedProofs{ 0 };
	std::atomic<uint64_t> maxQueueDepth{ 0 };
	std::atomic<uint64_t> workerBusyMs{ 0 };

	const auto failNow = [&](const char* reason) {
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			if (!failed) {
				failed = true;
				failureReason = reason;
			}
		}
		notEmpty.notify_all();
		notFull.notify_all();
	};

	const auto workerLoop = [&](const size_t workerIndex) {
		LOG_INFO_F("G49R worker={} started", workerIndex);
		while (true)
		{
			Batch batch;
			{
				std::unique_lock<std::mutex> lock(queueMutex);
				notEmpty.wait(lock, [&]() { return failed || !queue.empty() || done; });
				if (failed) {
					return;
				}
				if (queue.empty()) {
					if (done) {
						return;
					}
					continue;
				}
				batch = std::move(queue.front());
				queue.pop_front();
				notFull.notify_one();
			}

			if (cancelled && cancelled()) {
				failNow("cancelled before batch verification");
				return;
			}

			const long long batchStart = G49NowMs();
			bool verified = false;
			try {
				verified = verify(workerIndex, batch);
			}
			catch (const std::exception& e) {
				LOG_ERROR_F("G49R worker={} threw: {}", workerIndex, e.what());
				verified = false;
			}
			catch (...) {
				LOG_ERROR_F("G49R worker={} threw an unknown exception", workerIndex);
				verified = false;
			}
			const long long duration = G49NowMs() - batchStart;
			{
				std::lock_guard<std::mutex> lock(durationsMutex);
				batchDurations.push_back(duration);
			}
			workerBusyMs.fetch_add(static_cast<uint64_t>(duration));

			if (!verified) {
				failNow("batch verification failed");
				return;
			}

			verifiedBatches.fetch_add(1);
			const uint64_t proofCount = static_cast<uint64_t>(batch.Proofs.size());
			const uint64_t totalVerified = verifiedProofs.fetch_add(proofCount) + proofCount;
			if (progress) {
				std::lock_guard<std::mutex> lock(progressMutex);
				if (totalVerified > lastReportedProgress) {
					lastReportedProgress = totalVerified;
					progress(totalVerified);
				}
			}
		}
	};

	std::vector<std::thread> threads;
	threads.reserve(static_cast<size_t>(workers));
	try {
		for (int i = 0; i < workers; ++i) {
			threads.emplace_back([&, i] {
				try {
					workerLoop(static_cast<size_t>(i));
				} catch (...) {
					failNow("worker aborted");
				}
			});
		}
	}
	catch (const std::exception& e) {
		LOG_ERROR_F("G49R failed to start workers: {}", e.what());
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			failed = true;
			failureReason = "worker thread creation failed";
		}
		notEmpty.notify_all();
		notFull.notify_all();
		for (std::thread& thread : threads) {
			if (thread.joinable()) {
				thread.join();
			}
		}
		stats.ThreadStartFailed = true;
		if (pStats != nullptr) {
			*pStats = stats;
		}
		return false;
	}

	bool produced = false;
	try {
		produced = produce([&](Batch&& batch) -> bool {
			if (batch.Proofs.empty()) {
				failNow("empty batch submitted");
				return false;
			}

			const size_t proofCount = batch.Proofs.size();
			std::unique_lock<std::mutex> lock(queueMutex);
			notFull.wait(lock, [&]() { return failed || queue.size() < queueCapacity; });
			if (failed) {
				return false;
			}
			if (cancelled && cancelled()) {
				lock.unlock();
				failNow("cancelled before batch submission");
				return false;
			}

			queue.emplace_back(std::move(batch));
			submittedBatches.fetch_add(1);
			submittedProofs.fetch_add(static_cast<uint64_t>(proofCount));
			const uint64_t depth = static_cast<uint64_t>(queue.size());
			uint64_t previousDepth = maxQueueDepth.load();
			while (depth > previousDepth &&
				!maxQueueDepth.compare_exchange_weak(previousDepth, depth)) {
			}
			notEmpty.notify_one();
			return true;
		});
	}
	catch (const std::exception& e) {
		LOG_ERROR_F("G49R producer threw: {}", e.what());
		failNow("producer threw");
		produced = false;
	}
	catch (...) {
		failNow("producer threw an unknown exception");
		produced = false;
	}

	{
		std::lock_guard<std::mutex> lock(queueMutex);
		done = true;
	}
	notEmpty.notify_all();
	notFull.notify_all();

	for (std::thread& thread : threads) {
		if (thread.joinable()) {
			thread.join();
		}
	}

	bool success = produced && !failed;
	if (success && cancelled && cancelled()) {
		success = false;
		LOG_ERROR("G49R cancelled after the producer finished");
	}
	if (success) {
		success =
			verifiedBatches.load() == submittedBatches.load() &&
			verifiedProofs.load() == submittedProofs.load();
		if (!success) {
			LOG_ERROR_F(
				"G49R Execute accounting mismatch: submitted_batches={} verified_batches={} submitted_proofs={} verified_proofs={}",
				submittedBatches.load(),
				verifiedBatches.load(),
				submittedProofs.load(),
				verifiedProofs.load());
		}
	}

	if (!success && !failed) {
		failNow("incomplete submission set");
	}

	stats.SubmittedBatches = submittedBatches.load();
	stats.VerifiedBatches = verifiedBatches.load();
	stats.SubmittedProofs = submittedProofs.load();
	stats.VerifiedProofs = verifiedProofs.load();
	stats.MaxQueueDepth = maxQueueDepth.load();
	stats.WorkerBusyMs = workerBusyMs.load();
	{
		std::lock_guard<std::mutex> lock(durationsMutex);
		stats.BatchDurations = batchDurations;
	}

	if (pStats != nullptr) {
		*pStats = stats;
	}

	LOG_INFO_F(
		"G49R execute workers={} queue_capacity={} submitted_batches={} verified_batches={} "
		"submitted_proofs={} verified_proofs={} max_queue_depth={} worker_busy_ms={} failed={} reason={}",
		workers,
		queueCapacity,
		stats.SubmittedBatches,
		stats.VerifiedBatches,
		stats.SubmittedProofs,
		stats.VerifiedProofs,
		stats.MaxQueueDepth,
		stats.WorkerBusyMs,
		failed ? "yes" : "no",
		failed ? failureReason : "");

	return success;
}



