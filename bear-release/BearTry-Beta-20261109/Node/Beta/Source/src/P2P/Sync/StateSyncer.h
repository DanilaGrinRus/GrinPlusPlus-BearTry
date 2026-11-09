#pragma once

#include "../ConnectionManager.h"

#include <BlockChain/BlockChain.h>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_set>

// Forward Declarations
class SyncStatus;
class PibdSync;

class StateSyncer
{
public:
	StateSyncer(
		const std::weak_ptr<ConnectionManager>& pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<PibdSync> pPibdSync = nullptr
	)
		: m_pConnectionManager(pConnectionManager),
		m_pBlockChain(pBlockChain),
		m_pPibdSync(std::move(pPibdSync))
	{
		m_timeRequested = std::chrono::system_clock::now();
		m_requestedHeight = 0;
		m_pPeer = nullptr;
	}

	bool SyncState(SyncStatus& syncStatus);

	// Testable readiness wrapper: container-valid archive must also validate the
	// exact target header roots/sizes before it can suppress regeneration.
	static bool ValidateExistingArchiveForHeader(
		const fs::path& archivePath,
		const BlockHeaderPtr& pHeader,
		std::string* pError = nullptr);
	static bool FlushedMatchesConfirmed(
		const BlockHeaderPtr& pFlushed,
		const BlockHeaderPtr& pConfirmedTip,
		std::string* pReason = nullptr);
	static bool IsRecoveryMarkerPathWritable(const fs::path& markerPath);

	enum class EPibdResult
	{
		NotAttempted,   // fall back to txhashset.zip
		InProgress,     // keep waiting
		Completed       // state installed, block sync can proceed
	};

private:
	bool IsStateSyncDue(const SyncStatus& syncStatus) const;
	bool RequestState(SyncStatus& syncStatus);

	// PIBD path: starts the segment download, feeds requests to a capable peer and
	// installs the state once it has been written and activated.
	// Returns true while PIBD is in progress, false to fall back to txhashset.zip.
	EPibdResult TryPibd(SyncStatus& syncStatus);
	void DisablePibd();
	void SendPibdRequests(const std::chrono::steady_clock::time_point now);
	// Saves a snapshot of the newly activated state at the archive horizon so it
	// can be served later without a historical rewind (G-52). Validation and
	// atomic publication happen before the archive becomes visible; returns false
	// with pError on failure.
	bool ArchivePibdState(const Hash& horizonHash, std::string* pError = nullptr);
	// Fail-closed preflight before any snapshot/rewind: every block from the
	// target state through the confirmed flushed tip must have a body record and
	// every input commitment must be present in the canonical SPENT_OUTPUTS map.
	bool PreflightArchiveRewind(const BlockHeaderPtr& pTarget, std::string* pError = nullptr);
	// If a previous activation left pibd_state.complete but no archive (e.g. the
	// snapshot failed), retry once on startup.
	void TryRecoverArchive();
	PeerPtr FindPibdPeer() const;
	PeerPtr FindPibdPeer(const bool excludeTried) const;
	// All eligible PIBD peers, best height first. The first element is still the
	// primary peer used for status/fallback; requests are distributed across all.
	std::vector<PeerPtr> FindPibdPeers(const bool excludeTried) const;
	// Chooses an untried PIBD-capable peer after a failure. Returns false when
	// every capable peer has already been tried for this horizon.
	bool RotatePibdPeer();
	// Rotates to an untried peer, starts another full round with exponential
	// backoff after the peer set is exhausted, and only then disables PIBD.
	EPibdResult RetryOrDisablePibd();

	std::chrono::time_point<std::chrono::system_clock> m_timeRequested;
	uint64_t m_requestedHeight;
	PeerPtr m_pPeer;

	std::weak_ptr<ConnectionManager> m_pConnectionManager;
	IBlockChain::Ptr m_pBlockChain;
	std::shared_ptr<PibdSync> m_pPibdSync;
	std::chrono::system_clock::time_point m_pibdDisabledUntil = std::chrono::system_clock::time_point::min();
	Hash m_pibdHorizonHash;
	PeerPtr m_pibdPeer;
	std::vector<PeerPtr> m_pibdPeers;
	size_t m_pibdPeerCursor = 0;
	std::unordered_set<std::string> m_pibdPeersTried;
	uint32_t m_pibdRound = 0;
	std::chrono::system_clock::time_point m_pibdRetryAt = std::chrono::system_clock::time_point::min();
	bool m_archiveRecoveryChecked = false;
};

