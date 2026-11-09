#pragma once

#include "../../PMMR/KernelMMR.h"
#include "../../PMMR/OutputPMMR.h"
#include "../../PMMR/RangeProofPMMR.h"
#include "../../PMMR/TxHashSetImpl.h"
#include "../../PMMR/Zip/TxHashSetZip.h"
#include "../ArchivePublication.h"
#include "PibdParams.h"

#include <BlockChain/BlockChain.h>
#include <Common/Logger.h>
#include <Consensus.h>
#include <Core/Config.h>
#include <Core/Global.h>
#include <Crypto/Models/Hash.h>
#include <P2P/TxHashSetArchive.h>

#include <filesystem.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

// Bounded, single-flight serving of a persisted TxHashSet archive at the
// deterministic archive horizon (G-09 contract:
// docs/modernization/G09_ARCHIVE_SERVING.md).
//
// The ordinary node already writes TXHASHSET/archive/TxHashSet.<H>.<hash>.zip
// after a PIBD activation (StateSyncer::ArchivePibdState). This component turns
// such a file into an immutable, root-checked serving snapshot:
//
//   absent -> preparing -> verifying -> ready -> retiring
//
// Rules enforced here:
//  * horizon H = round_down(confirmed_head - STATE_SYNC_THRESHOLD, interval),
//    exactly grin 5.5.2 chain/src/chain.rs txhashset_archive_header();
//  * one preparation at a time, one ready snapshot (+ readers' leases);
//  * staging lives under TXHASHSET/archive/.staging-<short-hash>, is bounded by
//    zip/extracted-size and free-space caps, and is removed on failure or after
//    the last lease;
//  * nothing is published before extract + MMR-size + ValidateRoots (which also
//    covers the merged output/UBMT root for header v3+);
//  * readers acquire only by exact block hash; a hash mismatch is a refusal, and
//    an old horizon is never served under a new hash;
//  * capability advertisement is a separate, explicit query (ShouldAdvertise)
//    that is true only while a validated ready snapshot matches the expected
//    horizon. The feature flag defaults to OFF until the ordinary-binary live
//    acceptance is recorded.
//
// The lifecycle is split into:
//  * Registry  - pure state machine, no chain/filesystem access (unit-testable);
//  * Snapshot  - immutable loaded archive + lease object;
//  * the server - worker thread, archive load, resource limits, capability view.
//
// Header-only on purpose: src/P2P/CMakeLists.txt globs only P2P/*.cpp,
// Seed/*.cpp, Sync/*.cpp and Pipeline/*.cpp, so a new PIBD/*.cpp would not be
// compiled without touching a shared CMake file.
class PibdArchiveServer : public std::enable_shared_from_this<PibdArchiveServer>
{
public:
	// Flip to true only after the ordinary repo binary passes the G-09 live
	// acceptance scenarios (docs/modernization/G09_ARCHIVE_SERVING.md §6). While
	// false: no PIBD capability is advertised and the archive serving branch is
	// not entered; live flushed-state serving is unchanged.
	static constexpr bool ENABLED_BY_DEFAULT = false;

	// Resource limits. The zip/live sizes are deliberately conservative and must
	// be calibrated from a measured archive before enabling (§3.7 of the
	// contract). They are review placeholders, not validated numbers.
	static constexpr uint64_t MAX_ARCHIVE_ZIP_BYTES = 8ull * 1024ull * 1024ull * 1024ull;
	static constexpr uint64_t MAX_ARCHIVE_EXTRACTED_BYTES = 16ull * 1024ull * 1024ull * 1024ull;
	static constexpr uint64_t MIN_FREE_BYTES_AFTER_EXTRACT = 4ull * 1024ull * 1024ull * 1024ull;

	// Global serving budget: four times the per-connection limit. Protects the
	// single worker/CPU path from many peers asking for the same expensive
	// bitmap rebuild. Excess requests are dropped without a ban.
	static constexpr size_t MAX_GLOBAL_PIBD_REQUESTS_PER_MINUTE = 2000;

	static constexpr std::chrono::seconds WORKER_INTERVAL{ 5 };
	static constexpr std::chrono::seconds FAILURE_RETRY{ 60 };

	enum class EState
	{
		Disabled,
		Absent,
		Preparing,
		Ready,
		Failed
	};

	// Header-free identity of a validated archive state. This lets the state
	// machine, root checks and zip checks be unit-tested without constructing a
	// full PoW-bearing block header.
	struct ArchiveKey
	{
		uint16_t version = 3;
		Hash hash;
		uint64_t height = 0;
		uint64_t outputMmrSize = 0;
		uint64_t kernelMmrSize = 0;
		Hash outputRoot;      // header output_root (merged with UBMT for v3+)
		Hash rangeProofRoot;
		Hash kernelRoot;

		static ArchiveKey FromHeader(const BlockHeader& header)
		{
			ArchiveKey key;
			key.version = header.GetVersion();
			key.hash = header.GetHash();
			key.height = header.GetHeight();
			key.outputMmrSize = header.GetOutputMMRSize();
			key.kernelMmrSize = header.GetKernelMMRSize();
			key.outputRoot = header.GetOutputRoot();
			key.rangeProofRoot = header.GetRangeProofRoot();
			key.kernelRoot = header.GetKernelRoot();
			return key;
		}
	};

	// One immutable loaded archive. Readers hold a shared_ptr; the last release
	// removes the staging directory. txHashSet must be reset before the directory
	// can be removed on Windows (the PMMR files are mapped).
	struct Snapshot
	{
		ArchiveKey key;
		BlockHeaderPtr header;
		std::shared_ptr<TxHashSet> txHashSet;
		fs::path stagingRoot;
		uint64_t zipBytes = 0;
		uint64_t extractedBytes = 0;
		std::function<void(const fs::path&)> onDestroy;

		Snapshot() = default;
		Snapshot(const Snapshot&) = delete;
		Snapshot& operator=(const Snapshot&) = delete;

		~Snapshot()
		{
			txHashSet.reset();
			if (stagingRoot.empty()) {
				return;
			}

			if (onDestroy) {
				onDestroy(stagingRoot);
				return;
			}

			std::error_code error;
			fs::remove_all(stagingRoot, error);
		}
	};

	// Pure lifecycle state machine: no chain reads, no filesystem writes.
	// Thread-safe; every field is guarded by m_mutex.
	class Registry
	{
	public:
		explicit Registry(const bool enabled)
			: m_enabled(enabled),
			m_state(enabled ? EState::Absent : EState::Disabled)
		{
		}

		bool IsEnabled() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_enabled;
		}

		bool ShouldAdvertise() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_enabled && m_state == EState::Ready && m_hasExpected &&
				m_ready != nullptr && m_ready->key.hash == m_expectedHash;
		}

		// Exact-hash lookup only. Returns a lease (shared ownership) or nullptr.
		std::shared_ptr<const Snapshot> Acquire(const Hash& blockHash) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_enabled || m_state != EState::Ready || !m_hasExpected ||
				m_ready == nullptr || m_ready->key.hash != blockHash ||
				m_ready->key.hash != m_expectedHash) {
				return nullptr;
			}

			return m_ready;
		}

		// Called for every horizon the worker computes. A different hash retires
		// the old ready snapshot (existing leases keep it alive).
		void SetExpected(const Hash& blockHash, const uint64_t height)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_hasExpected || m_expectedHash != blockHash) {
				m_expectedHash = blockHash;
				m_expectedHeight = height;
				m_hasExpected = true;
				m_ready.reset();
				if (m_state == EState::Ready || m_state == EState::Failed) {
					m_state = m_enabled ? EState::Absent : EState::Disabled;
				}
				return;
			}

			m_expectedHeight = height;
		}

		bool IsReadyForExpected() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_ready != nullptr && m_hasExpected &&
				m_ready->key.hash == m_expectedHash;
		}

		void MarkReady()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_ready != nullptr && m_hasExpected &&
				m_ready->key.hash == m_expectedHash) {
				m_state = EState::Ready;
			}
		}

		bool CanPrepare(const std::chrono::steady_clock::time_point& now) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_enabled || m_state == EState::Preparing) {
				return false;
			}
			if (m_ready != nullptr && m_hasExpected &&
				m_ready->key.hash == m_expectedHash) {
				return false;
			}
			if (m_state == EState::Failed && now < m_retryAfter) {
				return false;
			}

			return true;
		}

		void BeginPreparation()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_state = EState::Preparing;
		}

		// Publishes only a snapshot that matches the expected horizon. A stale
		// snapshot (horizon changed during the load) is rejected and dropped by
		// the caller.
		bool Publish(const std::shared_ptr<Snapshot>& pSnapshot)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_enabled || !m_hasExpected || pSnapshot == nullptr ||
				pSnapshot->key.hash != m_expectedHash) {
				return false;
			}

			m_ready = pSnapshot;
			m_state = EState::Ready;
			return true;
		}

		void SetAbsent()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_hasExpected = false;
			m_expectedHeight = 0;
			m_ready.reset();
			m_state = m_enabled ? EState::Absent : EState::Disabled;
		}

		void Fail(const std::chrono::steady_clock::time_point& now)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_state = EState::Failed;
			m_retryAfter = now + FAILURE_RETRY;
		}

		EState GetState() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_state;
		}

		uint64_t GetReadyHeight() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_ready == nullptr) {
				return 0;
			}
			return m_ready->key.height;
		}

		Hash GetExpectedHash() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_expectedHash;
		}

	private:
		mutable std::mutex m_mutex;
		bool m_enabled;
		EState m_state;
		std::shared_ptr<const Snapshot> m_ready;
		Hash m_expectedHash;
		uint64_t m_expectedHeight = 0;
		bool m_hasExpected = false;
		std::chrono::steady_clock::time_point m_retryAfter{};
	};

	//
	// Process-wide owner. Initialize() is called once by the P2P server with the
	// chain it should observe; Shutdown() is called on teardown. Both are
	// idempotent. The instance is created even when disabled so callers can keep
	// a single code path.
	//
	static void Initialize(const IBlockChain::Ptr& pBlockChain, const bool enabled)
	{
		// Constructed here rather than with std::make_shared: the constructor is
		// private and std::make_shared is not a friend.
		std::shared_ptr<PibdArchiveServer> pServer(
			new PibdArchiveServer(pBlockChain, enabled));

		{
			std::lock_guard<std::mutex> lock(s_instanceMutex);
			if (s_instance != nullptr) {
				return;
			}
			s_instance = pServer;
		}

		pServer->Start();
	}

	static void Shutdown()
	{
		std::shared_ptr<PibdArchiveServer> pServer;
		{
			std::lock_guard<std::mutex> lock(s_instanceMutex);
			pServer = std::move(s_instance);
		}

		if (pServer != nullptr) {
			pServer->Stop();
		}
	}

	static std::shared_ptr<PibdArchiveServer> Instance()
	{
		std::lock_guard<std::mutex> lock(s_instanceMutex);
		return s_instance;
	}

	//
	// Pure helpers (also used by the executable unit tests).
	//
	static uint64_t ComputeHorizonHeight(const uint64_t confirmedTip) noexcept
	{
		return PibdParams::ComputeArchiveHorizonHeight(
			confirmedTip,
			(uint64_t)Consensus::STATE_SYNC_THRESHOLD,
			(uint64_t)Consensus::TXHASHSET_ARCHIVE_INTERVAL);
	}

	static bool IsServableHorizon(const uint64_t height) noexcept
	{
		return PibdParams::IsServableArchiveHorizon(
			height, (uint64_t)Consensus::TXHASHSET_ARCHIVE_INTERVAL);
	}

	// Isolated-test opt-in: GRINPP_PIBD_ARCHIVE_SERVING=1 enables archive
	// serving for one process without flipping ENABLED_BY_DEFAULT. Parsing is a
	// pure function so it can be unit-tested without touching process env.
	static bool IsTruthyOptInValue(const std::string_view value)
	{
		size_t begin = 0;
		size_t end = value.size();
		while (begin < end && std::isspace((unsigned char)value[begin]) != 0) {
			++begin;
		}
		while (end > begin && std::isspace((unsigned char)value[end - 1]) != 0) {
			--end;
		}
		if (begin == end) {
			return false;
		}

		std::string lowered(value.substr(begin, end - begin));
		for (char& c : lowered) {
			c = (char)std::tolower((unsigned char)c);
		}

		return lowered == "1" || lowered == "true" ||
			lowered == "on" || lowered == "yes";
	}

	// The production constant is a hard default; the environment can only opt
	// in. The mainnet refusal is enforced by the caller (Seeder), which has the
	// Global context. Windows uses _dupenv_s: the tree does not define
	// _CRT_SECURE_NO_WARNINGS, so plain getenv would fail /W4 /WX with C4996.
	static bool ResolveServingOptIn(const bool defaultEnabled)
	{
		if (defaultEnabled) {
			return true;
		}

#ifdef _WIN32
		char* pValue = nullptr;
		size_t valueLength = 0;
		if (_dupenv_s(&pValue, &valueLength, "GRINPP_PIBD_ARCHIVE_SERVING") != 0 ||
			pValue == nullptr) {
			return false;
		}

		const std::string value(pValue);
		std::free(pValue);
		return IsTruthyOptInValue(std::string_view(value));
#else
		const char* raw = std::getenv("GRINPP_PIBD_ARCHIVE_SERVING");
		return raw != nullptr && IsTruthyOptInValue(std::string_view(raw));
#endif
	}

	// Pure serving policy gate. Archive serving is a capability advertisement to
	// peers, so mainnet must stay disabled even if some future default or an
	// operator opt-in resolves to true. Kept pure (no Global access) so the
	// refusal is pinned by a unit test and cannot silently regress.
	static bool IsServingAllowed(const bool optIn, const bool isMainnet) noexcept
	{
		return optIn && !isMainnet;
	}

	// Saturated free-space requirement for one extraction. Returns
	// numeric_limits<uint64_t>::max() when the required amount would overflow.
	static uint64_t ComputeRequiredSpace(const uint64_t archiveBytes) noexcept
	{
		const uint64_t maxValue = (std::numeric_limits<uint64_t>::max)();
		if (archiveBytes > (maxValue - MIN_FREE_BYTES_AFTER_EXTRACT) / 2ull) {
			return maxValue;
		}

		return archiveBytes * 2ull + MIN_FREE_BYTES_AFTER_EXTRACT;
	}

	static bool HasSpaceFor(const fs::path& directory, const uint64_t archiveBytes)
	{
		const uint64_t required = ComputeRequiredSpace(archiveBytes);
		if (required == (std::numeric_limits<uint64_t>::max)()) {
			return false;
		}

		std::error_code error;
		const auto spaceInfo = fs::space(directory, error);
		if (error) {
			// Fail closed when the free-space probe fails.
			return false;
		}

		return (uint64_t)spaceInfo.available >= required;
	}

	// Removes every <archiveDir>/.staging-* directory. Returns the number of
	// directories removed. Crash leftovers are never reusable.
	static uint64_t CleanupStagingDirs(const fs::path& archiveDir)
	{
		uint64_t removed = 0;
		std::error_code error;
		if (!fs::exists(archiveDir, error) || error) {
			return removed;
		}

		for (const fs::directory_entry& entry : fs::directory_iterator(archiveDir, error))
		{
			if (error) {
				break;
			}

			const std::string name = entry.path().filename().string();
			if (name.rfind(".staging-", 0) != 0) {
				continue;
			}

			std::error_code removeError;
			fs::remove_all(entry.path(), removeError);
			if (!removeError) {
				++removed;
			}
			else {
				LOG_WARNING_F(
					"PIBD archive: cannot remove stale staging {}: {}",
					entry.path().string(),
					removeError.message());
			}
		}

		return removed;
	}

	// Removes the given deferred staging directories. Returns the number removed.
	// Public/static so the deferred-cleanup executor is unit-testable without a
	// chain or a running worker.
	static uint64_t RemoveGarbageDirs(std::deque<fs::path>& pending)
	{
		uint64_t removed = 0;
		for (const fs::path& path : pending)
		{
			std::error_code error;
			fs::remove_all(path, error);
			if (!error) {
				++removed;
			}
			else {
				LOG_WARNING_F(
					"PIBD archive: cannot remove staging {}: {}",
					path.string(),
					error.message());
			}
		}

		pending.clear();
		return removed;
	}

	// Loads an extracted archive directory and verifies it against the header:
	// MMR sizes must equal the header sizes and TxHashSet::ValidateRoots must
	// pass (kernel, rangeproof, and the merged output/UBMT root for v3+).
	// Returns nullptr and fills pError on any rejection.
	static std::shared_ptr<TxHashSet> LoadAndValidate(
		const fs::path& stagingRoot,
		const BlockHeaderPtr& header,
		std::string* pError = nullptr)
	{
		const auto fail = [pError](const std::string& message) -> std::shared_ptr<TxHashSet> {
			if (pError != nullptr) {
				*pError = message;
			}
			return nullptr;
		};

		if (header == nullptr) {
			return fail("archive header is missing");
		}

		try {
			std::shared_ptr<OutputPMMR> pOutputs = OutputPMMR::Load(stagingRoot);
			std::shared_ptr<RangeProofPMMR> pProofs = RangeProofPMMR::Load(stagingRoot);
			std::shared_ptr<KernelMMR> pKernels = KernelMMR::Load(stagingRoot);
			if (pOutputs == nullptr || pProofs == nullptr || pKernels == nullptr) {
				return fail("archive PMMRs could not be loaded");
			}

			if (pOutputs->GetSize() != header->GetOutputMMRSize() ||
				pProofs->GetSize() != header->GetOutputMMRSize() ||
				pKernels->GetSize() != header->GetKernelMMRSize()) {
				return fail("archive MMR sizes do not match the header");
			}

			auto pTxHashSet = std::make_shared<TxHashSet>(pKernels, pOutputs, pProofs, header);
			if (!pTxHashSet->ValidateRoots(*header)) {
				return fail("archive roots do not match the header");
			}

			return pTxHashSet;
		}
		catch (const std::exception& e) {
			return fail(std::string("archive validation failed: ") + e.what());
		}
		catch (...) {
			return fail("archive validation failed: unknown error");
		}
	}

	//
	// Instance API used by the message processor and the handshake.
	//
	bool IsEnabled() const noexcept { return m_enabled; }

	bool ShouldAdvertise() const { return m_registry.ShouldAdvertise(); }

	std::shared_ptr<const Snapshot> Acquire(const Hash& blockHash) const
	{
		return m_registry.Acquire(blockHash);
	}

	// Global token bucket for expensive archive segment generation.
	bool TryAcquireRequestSlot()
	{
		std::lock_guard<std::mutex> lock(m_rateMutex);
		const auto now = std::chrono::steady_clock::now();
		while (!m_requestTimes.empty() &&
			m_requestTimes.front() + std::chrono::seconds(60) <= now) {
			m_requestTimes.pop_front();
		}

		if (m_requestTimes.size() >= MAX_GLOBAL_PIBD_REQUESTS_PER_MINUTE) {
			return false;
		}

		m_requestTimes.push_back(now);
		return true;
	}

	EState GetState() const { return m_registry.GetState(); }

	uint64_t GetReadyHeight() const { return m_registry.GetReadyHeight(); }

	// Visible for tests: prepares once for the current horizon if an archive file
	// exists. A test can drive a full cycle without starting the worker thread,
	// but it needs a chain implementation.
	void TickForTest() { Tick(); }

private:
	PibdArchiveServer(const IBlockChain::Ptr& pBlockChain, const bool enabled)
		: m_pBlockChain(pBlockChain),
		m_enabled(enabled),
		m_registry(enabled)
	{
	}

	void Start()
	{
		if (!m_enabled) {
			return;
		}

		std::lock_guard<std::mutex> lock(m_workerMutex);
		if (m_worker.joinable()) {
			return;
		}

		m_stop.store(false);
		m_worker = std::thread(&PibdArchiveServer::Thread_Worker, this);
	}

	void Stop()
	{
		{
			std::lock_guard<std::mutex> lock(m_workerMutex);
			m_stop.store(true);
		}
		m_workerCv.notify_all();

		if (m_worker.joinable()) {
			m_worker.join();
		}

		m_registry.SetAbsent();
		DrainGarbage();
	}

	void Thread_Worker()
	{
		LoggerAPI::SetThreadName("PIBD_ARCHIVE");
		CleanupStaleStaging();

		while (!m_stop.load())
		{
			if (!Global::IsRunning()) {
				return;
			}

			// Always drain deferred staging removals, even on turns where the
			// ready snapshot is current and Tick() returns early. Readers release
			// leases on connection threads; this is where their retired staging
			// directories are actually removed (bounded by one worker interval).
			DrainGarbage();

			try {
				Tick();
			}
			catch (const std::exception& e) {
				LOG_WARNING_F("PIBD archive worker: {}", e.what());
			}
			catch (...) {
				LOG_WARNING("PIBD archive worker: unknown error");
			}

			std::unique_lock<std::mutex> lock(m_workerMutex);
			m_workerCv.wait_for(lock, WORKER_INTERVAL, [this] { return m_stop.load(); });
		}
	}

	void Tick()
	{
		if (!m_enabled) {
			return;
		}

		BlockHeaderPtr pHorizon;
		uint64_t height = 0;
		try {
			const uint64_t tip = m_pBlockChain->GetHeight(EChainType::CONFIRMED);
			height = ComputeHorizonHeight(tip);
			if (height != 0) {
				pHorizon = m_pBlockChain->GetBlockHeaderByHeight(height, EChainType::CONFIRMED);
			}
		}
		catch (const std::exception& e) {
			LOG_WARNING_F("PIBD archive: chain read failed: {}", e.what());
			return;
		}

		if (pHorizon == nullptr || !IsServableHorizon(height)) {
			m_registry.SetAbsent();
			return;
		}

		const Hash expectedHash = pHorizon->GetHash();
		m_registry.SetExpected(expectedHash, height);
		if (m_registry.IsReadyForExpected()) {
			m_registry.MarkReady();
			return;
		}

		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		if (!m_registry.CanPrepare(now)) {
			return;
		}

		std::error_code error;
		const fs::path archivePath = TxHashSetArchive::Path(
			Global::GetConfig().GetTxHashSetPath(), *pHorizon);
		const auto bytes = fs::file_size(archivePath, error);
		const auto mtime = fs::last_write_time(archivePath, error);
		if (error) {
			m_registry.SetAbsent();
			return;
		}

		const std::string archiveSignature =
			archivePath.string() + "|" + std::to_string(bytes) + "|" +
			std::to_string(mtime.time_since_epoch().count());

		// Readiness validates the container; a partial/truncated archive is never
		// extracted or advertised. Cache the invalid signature so the worker does
		// not re-open a broken zip every interval.
		std::string archiveError;
		if (!ArchivePublication::IsReadyArchive(archivePath, &archiveError)) {
			if (archiveSignature != m_invalidArchiveSignature) {
				m_invalidArchiveSignature = archiveSignature;
				LOG_WARNING_F(
					"PIBD archive: ignoring unready archive {}: {}",
					archivePath.filename().string(),
					archiveError);
			}
			m_registry.SetAbsent();
			return;
		}
		m_invalidArchiveSignature.clear();

		m_registry.BeginPreparation();
		LOG_INFO_F(
			"PIBD archive: preparing serving snapshot at height {} ({})",
			height,
			archivePath.filename().string());

		std::shared_ptr<Snapshot> pSnapshot;
		std::string failure;
		try {
			pSnapshot = LoadSnapshot(archivePath, pHorizon);
		}
		catch (const std::exception& e) {
			failure = e.what();
		}
		catch (...) {
			failure = "unknown error";
		}

		if (pSnapshot == nullptr) {
			Fail(failure);
			return;
		}

		// Re-read the tip after the (potentially slow) load: a reorg or a moved
		// horizon must not publish a snapshot for a stale key.
		try {
			const uint64_t tipNow = m_pBlockChain->GetHeight(EChainType::CONFIRMED);
			if (ComputeHorizonHeight(tipNow) != height) {
				pSnapshot.reset();
				m_registry.SetAbsent();
				return;
			}
		}
		catch (const std::exception& e) {
			pSnapshot.reset();
			Fail(std::string("chain recheck failed: ") + e.what());
			return;
		}

		if (!m_registry.Publish(pSnapshot)) {
			// The horizon moved between SetExpected and Publish.
			pSnapshot.reset();
			m_registry.SetAbsent();
			return;
		}

		LOG_INFO_F(
			"PIBD archive: ready at height {} (zip {} B, extracted {} B)",
			height,
			pSnapshot->zipBytes,
			pSnapshot->extractedBytes);
		DrainGarbage();
	}

	void Fail(const std::string& error)
	{
		m_registry.Fail(std::chrono::steady_clock::now());
		LOG_WARNING_F("PIBD archive: preparation failed: {}", error);
		DrainGarbage();
	}

	std::shared_ptr<Snapshot> LoadSnapshot(
		const fs::path& archivePath,
		const BlockHeaderPtr& header)
	{
		const Config& config = Global::GetConfig();
		const fs::path archiveDir = archivePath.parent_path();
		const fs::path stagingRoot = archiveDir / (".staging-" + header->ShortHash());

		std::error_code error;
		fs::create_directories(archiveDir, error);
		if (error) {
			throw std::runtime_error("cannot create archive directory: " + error.message());
		}

		const auto archiveBytes = fs::file_size(archivePath, error);
		if (error) {
			throw std::runtime_error("cannot stat archive: " + error.message());
		}
		if (archiveBytes == 0 || (uint64_t)archiveBytes > MAX_ARCHIVE_ZIP_BYTES) {
			throw std::runtime_error("archive size is outside the allowed range");
		}
		if (!HasSpaceFor(archiveDir, (uint64_t)archiveBytes)) {
			throw std::runtime_error(
				"not enough free space for archive extraction (need " +
				std::to_string(ComputeRequiredSpace((uint64_t)archiveBytes)) + " B)");
		}

		fs::remove_all(stagingRoot, error);
		error.clear();
		if (!fs::create_directories(stagingRoot, error) || error) {
			throw std::runtime_error(
				"cannot create archive staging directory: " + error.message());
		}

		auto pSnapshot = std::make_shared<Snapshot>();
		pSnapshot->header = header;
		pSnapshot->key = ArchiveKey::FromHeader(*header);
		pSnapshot->stagingRoot = stagingRoot;
		pSnapshot->zipBytes = (uint64_t)archiveBytes;

		std::weak_ptr<PibdArchiveServer> pWeakSelf = weak_from_this();
		pSnapshot->onDestroy = [pWeakSelf](const fs::path& path) {
			const std::shared_ptr<PibdArchiveServer> pSelf = pWeakSelf.lock();
			if (pSelf != nullptr) {
				pSelf->EnqueueGarbage(path);
				return;
			}

			std::error_code cleanupError;
			fs::remove_all(path, cleanupError);
		};

		if (!TxHashSetZip(config).Extract(archivePath, *header, stagingRoot)) {
			throw std::runtime_error("archive extraction failed");
		}

		uint64_t extractedBytes = 0;
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(stagingRoot, error))
		{
			if (error) {
				break;
			}

			std::error_code entryError;
			if (entry.is_regular_file(entryError) && !entryError) {
				const auto entryBytes = entry.file_size(entryError);
				if (!entryError) {
					extractedBytes += (uint64_t)entryBytes;
				}
			}

			if (extractedBytes > MAX_ARCHIVE_EXTRACTED_BYTES) {
				throw std::runtime_error("archive extraction exceeds the size cap");
			}
		}
		if (error) {
			throw std::runtime_error("cannot enumerate archive staging: " + error.message());
		}
		if (extractedBytes == 0) {
			throw std::runtime_error("archive extraction produced no files");
		}

		std::string validationError;
		std::shared_ptr<TxHashSet> pTxHashSet = LoadAndValidate(stagingRoot, header, &validationError);
		if (pTxHashSet == nullptr) {
			throw std::runtime_error(
				validationError.empty() ? "archive validation failed" : validationError);
		}

		pSnapshot->txHashSet = std::move(pTxHashSet);
		pSnapshot->extractedBytes = extractedBytes;
		return pSnapshot;
	}

	void CleanupStaleStaging() const
	{
		try {
			const fs::path archiveDir = TxHashSetArchive::Dir(
				Global::GetConfig().GetTxHashSetPath());
			const uint64_t removed = CleanupStagingDirs(archiveDir);
			if (removed != 0) {
				LOG_INFO_F("PIBD archive: removed {} stale staging director(ies)", removed);
			}
		}
		catch (const std::exception& e) {
			LOG_WARNING_F("PIBD archive: staging cleanup failed: {}", e.what());
		}
	}

	void EnqueueGarbage(const fs::path& path)
	{
		std::lock_guard<std::mutex> lock(m_garbageMutex);
		m_garbage.push_back(path);
	}

	void DrainGarbage()
	{
		std::deque<fs::path> pending;
		{
			std::lock_guard<std::mutex> lock(m_garbageMutex);
			pending.swap(m_garbage);
		}

		RemoveGarbageDirs(pending);
	}

	IBlockChain::Ptr m_pBlockChain;
	const bool m_enabled;
	Registry m_registry;

	std::thread m_worker;
	std::mutex m_workerMutex;
	std::condition_variable m_workerCv;
	std::atomic<bool> m_stop{ false };

	std::mutex m_rateMutex;
	std::deque<std::chrono::steady_clock::time_point> m_requestTimes;

	std::mutex m_garbageMutex;
	std::deque<fs::path> m_garbage;

	// Worker-only cache: avoids reopening the same invalid archive every interval.
	std::string m_invalidArchiveSignature;

	inline static std::mutex s_instanceMutex;
	inline static std::shared_ptr<PibdArchiveServer> s_instance;
};
