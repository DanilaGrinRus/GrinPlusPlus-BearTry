#pragma once

#include <cstdint>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include <Common/Logger.h>

enum class ESyncStatus
{
	SYNCING_HEADERS,
	SYNCING_TXHASHSET,
	PROCESSING_TXHASHSET,
	TXHASHSET_SYNC_FAILED,
	SYNCING_BLOCKS,
	WAITING_FOR_PEERS,
	NOT_SYNCING
};

// Height and total difficulty from one header-tip snapshot. Both fields are
// updated and read together so advertisement code cannot mix two tips.
struct HeaderTip
{
	uint64_t Height = 0;
	uint64_t TotalDifficulty = 0;
};

class SyncStatus
{
public:
	SyncStatus()
		: m_syncStatus(ESyncStatus::SYNCING_HEADERS),
		m_numActiveConnections(0),
		m_networkHeight(0),
		m_networkDifficulty(0),
		m_headerHeight(0),
		m_headerDifficulty(0),
		m_blockHeight(0),
		m_blockDifficulty(0),
		m_lastBlockTime(0),
		m_txHashSetDownloaded(0),
		m_txHashSetTotalSize(0),
		m_txHashSetProcessingStatus(0),
		m_pibdReceived(0),
		m_pibdRequired(0),
		m_pibdStarted(0)
	{

	}
	SyncStatus(const SyncStatus& other)
		: m_syncStatus(other.m_syncStatus.load()),
		m_numActiveConnections(other.m_numActiveConnections.load()),
		m_networkHeight(other.m_networkHeight.load()),
		m_networkDifficulty(other.m_networkDifficulty.load()),
		m_headerHeight(0),
		m_headerDifficulty(0),
		m_blockHeight(other.m_blockHeight.load()),
		m_blockDifficulty(other.m_blockDifficulty.load()),
		m_lastBlockTime(other.m_lastBlockTime.load()),
		m_txHashSetDownloaded(other.m_txHashSetDownloaded.load()),
		m_txHashSetTotalSize(other.m_txHashSetTotalSize.load()),
		m_txHashSetProcessingStatus(other.m_txHashSetProcessingStatus.load()),
		m_blockRequests(other.m_blockRequests.load()),
		m_blockTimeouts(other.m_blockTimeouts.load()),
		m_blockReassignments(other.m_blockReassignments.load()),
		m_blockCooldownReassignments(other.m_blockCooldownReassignments.load()),
		m_blockRetries(other.m_blockRetries.load()),
		m_bansFraudHeight(other.m_bansFraudHeight.load()),
		m_bansInvalidData(other.m_bansInvalidData.load()),
		m_pibdReceived(other.m_pibdReceived.load()),
		m_pibdRequired(other.m_pibdRequired.load()),
		m_pibdStarted(other.m_pibdStarted.load())
	{
		const HeaderTip headerTip = other.GetHeaderTip();
		m_headerHeight = headerTip.Height;
		m_headerDifficulty = headerTip.TotalDifficulty;
		std::lock_guard<std::mutex> lock(other.m_reasonMutex);
		m_waitingReason = other.m_waitingReason;
		m_pibdPeer = other.m_pibdPeer;
	}

	bool IsSyncing() const { return m_syncStatus != ESyncStatus::NOT_SYNCING; }
	ESyncStatus GetStatus() const { return m_syncStatus; }
	uint64_t GetNumActiveConnections() const { return m_numActiveConnections; }
	uint64_t GetNetworkHeight() const { return m_networkHeight; }
	uint64_t GetNetworkDifficulty() const { return m_networkDifficulty; }
	HeaderTip GetHeaderTip() const
	{
		std::lock_guard<std::mutex> lock(m_headerTipMutex);
		return HeaderTip{ m_headerHeight, m_headerDifficulty };
	}
	uint64_t GetHeaderHeight() const { return GetHeaderTip().Height; }
	uint64_t GetHeaderDifficulty() const { return GetHeaderTip().TotalDifficulty; }
	uint64_t GetBlockHeight() const { return m_blockHeight; }
	uint64_t GetBlockDifficulty() const { return m_blockDifficulty; }
	time_t GetLastBlockTime() const noexcept { return m_lastBlockTime; }
	uint64_t GetDownloaded() const { return m_txHashSetDownloaded; }
	uint64_t GetDownloadSize() const { return m_txHashSetTotalSize; }
	uint8_t GetProcessingStatus() const { return m_txHashSetProcessingStatus; }

	// Block download observability (REQ_G38_FIX.md §3.8).
	uint64_t GetBlockRequests() const { return m_blockRequests; }
	uint64_t GetBlockTimeouts() const { return m_blockTimeouts; }
	uint64_t GetBlockReassignments() const { return m_blockReassignments; }
	uint64_t GetBlockCooldownReassignments() const { return m_blockCooldownReassignments; }
	uint64_t GetBlockRetries() const { return m_blockRetries; }
	uint64_t GetBansFraudHeight() const { return m_bansFraudHeight; }
	uint64_t GetBansInvalidData() const { return m_bansInvalidData; }
	uint64_t GetPibdReceived() const { return m_pibdReceived; }
	uint64_t GetPibdRequired() const { return m_pibdRequired; }
	time_t GetPibdStarted() const { return m_pibdStarted; }
	std::string GetPibdPeer() const { std::lock_guard<std::mutex> lock(m_reasonMutex); return m_pibdPeer; }
	void SetPibdPeer(const std::string& peer) const { std::lock_guard<std::mutex> lock(m_reasonMutex); m_pibdPeer = peer; }
	void SetPibdProgress(const uint64_t received, const uint64_t required) const { m_pibdReceived = received; m_pibdRequired = required; }
	void SetPibdStarted(const time_t started) const { m_pibdStarted = started; }
	void IncrementBlockRequests(const uint64_t count = 1) const { m_blockRequests += count; }
	void IncrementBlockTimeouts(const uint64_t count = 1) const { m_blockTimeouts += count; }
	void IncrementBlockReassignments(const uint64_t count = 1) const { m_blockReassignments += count; }
	void IncrementBlockCooldownReassignments(const uint64_t count = 1) const { m_blockCooldownReassignments += count; }
	void IncrementBlockRetries(const uint64_t count = 1) const { m_blockRetries += count; }
	void IncrementBansFraudHeight(const uint64_t count = 1) const { m_bansFraudHeight += count; }
	void IncrementBansInvalidData(const uint64_t count = 1) const { m_bansInvalidData += count; }

	// Human-readable reason for the WAITING_FOR_PEERS state.
	void SetWaitingReason(const std::string& reason) const
	{
		std::lock_guard<std::mutex> lock(m_reasonMutex);
		m_waitingReason = reason;
	}

	std::string GetWaitingReason() const
	{
		std::lock_guard<std::mutex> lock(m_reasonMutex);
		return m_waitingReason;
	}

	// Waiting reasons are historical outside the waiting state.
	std::string GetCurrentWaitingReason() const
	{
		return GetStatus() == ESyncStatus::WAITING_FOR_PEERS ? GetWaitingReason() : "";
	}

	void EnableStageLogging()
	{
		std::lock_guard<std::mutex> lock(m_stageMutex);
		m_stageLogging = true;
		m_stageStart = std::chrono::steady_clock::now();
		try { LoggerAPI::LogSyncEvent(std::string("event=stage_begin stage=") + StatusName(m_syncStatus.load()) +
			" height=" + std::to_string(m_blockHeight.load()) + " network_height=" + std::to_string(m_networkHeight.load())); } catch (...) { }
	}

	void UpdateStatus(const ESyncStatus syncStatus)
	{
		if (m_syncStatus.load() == syncStatus) return;
		std::lock_guard<std::mutex> lock(m_stageMutex);
		const ESyncStatus previous = m_syncStatus.load();
		if (previous == syncStatus) return;
		m_syncStatus = syncStatus;
		if (m_stageLogging) {
			const auto now = std::chrono::steady_clock::now();
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stageStart).count();
			m_stageStart = now;
			try {
				LoggerAPI::LogSyncEvent(std::string("event=stage_transition previous=") + StatusName(previous) +
					" stage=" + StatusName(syncStatus) + " previous_duration_ms=" + std::to_string(ms) +
					" height=" + std::to_string(m_blockHeight.load()) + " network_height=" + std::to_string(m_networkHeight.load()));
			} catch (...) { }
		}
	}

	void EndStageLogging()
	{
		std::lock_guard<std::mutex> lock(m_stageMutex);
		if (!m_stageLogging) return;
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - m_stageStart).count();
		try { LoggerAPI::LogSyncEvent(std::string("event=stage_end stage=") + StatusName(m_syncStatus.load()) +
			" duration_ms=" + std::to_string(ms) + " reason=shutdown"); } catch (...) { }
		m_stageLogging = false;
	}

	void UpdateNetworkStatus(const uint64_t numActiveConnections, const uint64_t networkHeight, const uint64_t networkDifficulty)
	{
		m_numActiveConnections = numActiveConnections;
		if (networkHeight != NULL) m_networkHeight = networkHeight;
		if (networkDifficulty != NULL) m_networkDifficulty = networkDifficulty;
	}

	void UpdateHeaderStatus(const uint64_t headerHeight, const uint64_t headerDifficulty)
	{
		std::lock_guard<std::mutex> lock(m_headerTipMutex);
		m_headerHeight = headerHeight;
		m_headerDifficulty = headerDifficulty;
	}

	void UpdateBlockStatus(const uint64_t blockHeight, const uint64_t blockDifficulty, const time_t lastBlockTime)
	{
		m_blockHeight = blockHeight;
		m_blockDifficulty = blockDifficulty;
		m_lastBlockTime = lastBlockTime;
	}

	void UpdateDownloaded(const uint64_t downloaded) { m_txHashSetDownloaded = downloaded; }
	void UpdateDownloadSize(const uint64_t downloadSize) { m_txHashSetTotalSize = downloadSize; }
	void UpdateProcessingStatus(const uint8_t processingStatus) { m_txHashSetProcessingStatus = processingStatus; }
	// A failed or abandoned archive transfer must not leave the size of the
	// failed attempt visible as state.download_size while downloaded is 0 (the
	// task-64 stand observed a stale 64 MiB after an interrupted transfer).
	// Clear only the transfer progress counters; the sync status and the request
	// identity are set/kept separately so retry/ban logic is unchanged.
	void ClearTxHashSetProgress() noexcept
	{
		m_txHashSetDownloaded = 0;
		m_txHashSetTotalSize = 0;
		m_txHashSetProcessingStatus = 0;
	}
	void SetTxHashSetDownloading(const bool downloading) const { m_txHashSetDownloading = downloading; }
	bool IsTxHashSetDownloading() const { return m_txHashSetDownloading.load(); }

	// Request identity for late/duplicate TxHashSet responses. The protocol has
	// no request id, so peer + archive header hash (and height when available) is
	// the strongest correlation available. A late response for a previously
	// requested archive is not abuse.
	void RecordTxHashSetRequest(
		const std::string& peer,
		const std::string& blockHash,
		const uint64_t height) const
	{
		std::lock_guard<std::mutex> lock(m_txHashSetRequestMutex);
		m_currentTxHashSetPeer = peer;
		m_currentTxHashSetHash = blockHash;
		m_currentTxHashSetHeight = height;
		m_txHashSetRequestHistory.emplace_back(peer, blockHash);
		if (m_txHashSetRequestHistory.size() > 8) {
			m_txHashSetRequestHistory.erase(m_txHashSetRequestHistory.begin());
		}
	}

	bool IsCurrentTxHashSetRequest(
		const std::string& peer,
		const std::string& blockHash) const
	{
		std::lock_guard<std::mutex> lock(m_txHashSetRequestMutex);
		return !m_currentTxHashSetHash.empty() &&
			m_currentTxHashSetPeer == peer &&
			m_currentTxHashSetHash == blockHash;
	}

	bool IsCurrentTxHashSetRequest(
		const std::string& peer,
		const std::string& blockHash,
		const uint64_t height) const
	{
		std::lock_guard<std::mutex> lock(m_txHashSetRequestMutex);
		return !m_currentTxHashSetHash.empty() &&
			m_currentTxHashSetPeer == peer &&
			m_currentTxHashSetHash == blockHash &&
			m_currentTxHashSetHeight == height;
	}

	bool WasTxHashSetRequested(
		const std::string& peer,
		const std::string& blockHash) const
	{
		std::lock_guard<std::mutex> lock(m_txHashSetRequestMutex);
		for (const auto& entry : m_txHashSetRequestHistory) {
			if (entry.first == peer && entry.second == blockHash) {
				return true;
			}
		}
		return false;
	}

private:
	static const char* StatusName(const ESyncStatus status) noexcept
	{
		switch (status) {
			case ESyncStatus::SYNCING_HEADERS: return "headers";
			case ESyncStatus::SYNCING_TXHASHSET: return "state_download";
			case ESyncStatus::PROCESSING_TXHASHSET: return "state_validation";
			case ESyncStatus::TXHASHSET_SYNC_FAILED: return "state_failed";
			case ESyncStatus::SYNCING_BLOCKS: return "blocks";
			case ESyncStatus::WAITING_FOR_PEERS: return "waiting_for_peers";
			case ESyncStatus::NOT_SYNCING: return "fully_synced";
		}
		return "unknown";
	}
	std::mutex m_stageMutex;
	bool m_stageLogging = false;
	std::chrono::steady_clock::time_point m_stageStart;
	std::atomic<ESyncStatus> m_syncStatus;
	std::atomic<uint64_t> m_numActiveConnections;
	std::atomic<uint64_t> m_networkHeight;
	std::atomic<uint64_t> m_networkDifficulty;
	uint64_t m_headerHeight;
	uint64_t m_headerDifficulty;
	std::atomic<uint64_t> m_blockHeight;
	std::atomic<uint64_t> m_blockDifficulty;
	std::atomic<time_t> m_lastBlockTime;
	std::atomic<uint64_t> m_txHashSetDownloaded;
	std::atomic<uint64_t> m_txHashSetTotalSize;
	std::atomic<uint8_t> m_txHashSetProcessingStatus;

	mutable std::atomic<uint64_t> m_blockRequests{0};
	mutable std::atomic<uint64_t> m_blockTimeouts{0};
	mutable std::atomic<uint64_t> m_blockReassignments{0};
	mutable std::atomic<uint64_t> m_blockCooldownReassignments{0};
	mutable std::atomic<uint64_t> m_blockRetries{0};
	mutable std::atomic<uint64_t> m_bansFraudHeight{0};
	mutable std::atomic<uint64_t> m_bansInvalidData{0};
	mutable std::atomic_bool m_txHashSetDownloading{ false };
	mutable std::mutex m_headerTipMutex;
	mutable std::mutex m_reasonMutex;
	mutable std::mutex m_txHashSetRequestMutex;
	mutable std::string m_currentTxHashSetPeer;
	mutable std::string m_currentTxHashSetHash;
	mutable uint64_t m_currentTxHashSetHeight = 0;
	mutable std::vector<std::pair<std::string, std::string>> m_txHashSetRequestHistory;
	mutable std::string m_waitingReason;
	mutable std::string m_pibdPeer;
	mutable std::atomic<uint64_t> m_pibdReceived;
	mutable std::atomic<uint64_t> m_pibdRequired;
	mutable std::atomic<time_t> m_pibdStarted;
};

typedef std::shared_ptr<SyncStatus> SyncStatusPtr;
typedef std::shared_ptr<SyncStatus> SyncStatusConstPtr;
