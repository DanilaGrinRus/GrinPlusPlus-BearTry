#include "StateSyncer.h"
#include "../ArchivePublication.h"
#include "../Messages/TxHashSetRequestMessage.h"
#include "../PIBD/PibdArchiveServer.h"
#include "../PIBD/PibdSync.h"
#include <P2P/TxHashSetArchive.h>
#include "../../PMMR/Zip/TxHashSetZip.h"
#include <atomic>
#include <fstream>

#include <Common/Util/FileUtil.h>
#include <Common/Util/HexUtil.h>
#include <Consensus.h>
#include <Common/Logger.h>
#include <Core/File/FileRemover.h>
#include <Core/File/FileSync.h>
#include <Core/Global.h>
#include <Core/Models/FullBlock.h>
#include <P2P/Capabilities.h>
#include <P2P/ConnectedPeer.h>
#include <P2P/Peer.h>
#include <P2P/SyncStatus.h>

namespace
{
	fs::path RecoveryFailedMarker()
	{
		return Global::GetConfig().GetTxHashSetPath() / "pibd_state.recovery_failed";
	}

	bool IsRecoveryBlocked(const Hash& horizonHash)
	{
		try {
			std::vector<uint8_t> bytes;
			if (!FileUtil::ReadFile(RecoveryFailedMarker(), bytes)) {
				return false;
			}

			const std::string content(bytes.begin(), bytes.end());
			return ArchivePublication::SameRecoveryTarget(
				content,
				HexUtil::ConvertToHex(horizonHash.GetData()));
		}
		catch (...) {
			return false;
		}
	}

	void ClearRecoveryBlocked()
	{
		std::error_code error;
		fs::remove(RecoveryFailedMarker(), error);
	}


	// A recovery attempt that cannot persist its bound marker would retry on every
	// restart forever. Probe the marker location before attempting an expensive
	// snapshot/rewind; refuse the attempt if the bound cannot be recorded.
	bool IsRecoveryMarkerWritable()
	{
		return StateSyncer::IsRecoveryMarkerPathWritable(RecoveryFailedMarker());
	}

	void RecordRecoveryFailure(const Hash& horizonHash, const std::string& error)
	{
		try {
			FileUtil::WriteTextToFile(
				RecoveryFailedMarker(),
				HexUtil::ConvertToHex(horizonHash.GetData()) + "\n" + error + "\n");
			FileSync::Flush(RecoveryFailedMarker());
		}
		catch (...) {
			// The failure marker is diagnostic only; never mask the original failure.
		}
	}

	// An existing final archive is only "ready" if its exact header roots/sizes
	// validate. Container-only readiness would suppress regeneration of a
	// complete-but-wrong-root archive.
	bool IsReadyArchiveForHeader(
		const fs::path& archivePath,
		const BlockHeaderPtr& pHeader,
		std::string* pError)
	{
		std::string readinessError;
		if (!ArchivePublication::IsReadyArchive(archivePath, &readinessError)) {
			if (pError != nullptr) {
				*pError = readinessError;
			}
			return false;
		}

		static std::atomic<uint64_t> s_validationId{ 0 };
		const fs::path validationRoot = fs::temp_directory_path() /
			("g09-readiness-validate-" + pHeader->ShortHash() + "-" +
				std::to_string(s_validationId.fetch_add(1)));
		FileRemover validationRemover(validationRoot);

		TxHashSetZip archiveZip(Global::GetConfig());
		if (!archiveZip.Extract(archivePath, *pHeader, validationRoot)) {
			if (pError != nullptr) {
				*pError = "existing archive extraction failed";
			}
			return false;
		}

		std::string validationError;
		if (PibdArchiveServer::LoadAndValidate(
				validationRoot, pHeader, &validationError) == nullptr) {
			if (pError != nullptr) {
				*pError = validationError.empty()
					? "existing archive root validation failed"
					: validationError;
			}
			return false;
		}

		return true;
	}
}

bool StateSyncer::ValidateExistingArchiveForHeader(
	const fs::path& archivePath,
	const BlockHeaderPtr& pHeader,
	std::string* pError)
{
	return IsReadyArchiveForHeader(archivePath, pHeader, pError);
}

bool StateSyncer::SyncState(SyncStatus& syncStatus)
{
	TryRecoverArchive();
	if (IsStateSyncDue(syncStatus))
	{
		// Prefer PIBD when a capable peer is connected. On success the state is
		// installed and block sync may proceed; otherwise use txhashset.zip.
		const EPibdResult pibd = TryPibd(syncStatus);
		if (pibd == EPibdResult::InProgress) {
			return true;
		}

		if (pibd == EPibdResult::Completed) {
			return false;
		}

		syncStatus.UpdateStatus(ESyncStatus::SYNCING_TXHASHSET);
		RequestState(syncStatus);

		return true;
	}

	// If state sync is still in progress, return true to delay block sync.
	if (syncStatus.GetBlockHeight() < Consensus::GetHorizonHeight(syncStatus.GetHeaderHeight()))
	{
		return true;
	}

	return false;
}

// NOTE: This doesn't handle re-orgs beyond the horizon.
bool StateSyncer::IsStateSyncDue(const SyncStatus& syncStatus) const
{
	const uint64_t headerHeight = syncStatus.GetHeaderHeight();
	const uint64_t blockHeight = syncStatus.GetBlockHeight();

	const ESyncStatus status = syncStatus.GetStatus();
	if (status == ESyncStatus::PROCESSING_TXHASHSET)
	{
		return false;
	}

	// One transfer owns the temp archive at a time. Do not rotate peers while a
	// download is active; the pipe watchdog terminates a genuinely stalled
	// transfer and clears this flag, after which a new request is allowed.
	if (syncStatus.IsTxHashSetDownloading())
	{
		return false;
	}

	if (status == ESyncStatus::TXHASHSET_SYNC_FAILED)
	{
		// Transition warning: once per failed-request epoch, not every 10 ms.
		if (m_logGate.ShouldLogSyncFailed()) {
			LOG_WARNING("TxHashSet sync failed.");
		}
		return true;
	}

	// For the first week, there's no reason to request TxHashSet, since we can just download full blocks.
	if (headerHeight < Consensus::CUT_THROUGH_HORIZON)
	{
		return false;
	}

	// If block height is within threshold, just rely on block sync.
	if (blockHeight > (headerHeight - Consensus::CUT_THROUGH_HORIZON))
	{
		return false;
	}

	if (m_requestedHeight == 0)
	{
		// Logged once when the request is actually sent in RequestState().
		return true;
	}

	// If TxHashSet download timed out, request it from another peer.
	if ((m_timeRequested + std::chrono::minutes(120)) < std::chrono::system_clock::now())
	{
		if (m_logGate.ShouldLogDownloadTimeout()) {
			LOG_WARNING("Download timed out (120 minutes).");
		}
		return true;
	}

	// Give the peer a bounded start window before rotating. Ten seconds was too
	// aggressive for public peers and caused overlapping requests; late answers
	// are now ignored without bans, but avoiding the overlap is cheaper.
	if ((m_timeRequested + std::chrono::seconds(120)) < std::chrono::system_clock::now())
	{
		const uint64_t downloaded = syncStatus.GetDownloaded();
		if (downloaded == 0)
		{
			if (m_logGate.ShouldLogNoProgress()) {
				LOG_WARNING("120 seconds elapsed and download still not started.");
			}
			return true;
		}
	}

	if (m_pPeer != nullptr && !m_pConnectionManager.lock()->IsConnected(m_pPeer->GetIPAddress()))
	{
		// The sync peer is gone, but the 30 s re-request guard can keep this
		// condition true for many iterations; log the transition once per epoch.
		if (m_logGate.ShouldLogPeerLost()) {
			LOG_WARNING("Sync peer no longer connected.");
		}
		return true;
	}

	return false;
}

bool StateSyncer::RequestState(SyncStatus& syncStatus)
{
	// Never send a second request while the first one is still outstanding:
	// the server treats repeated TxHashSet requests within two hours as abuse.
	if (m_requestedHeight != 0 &&
		std::chrono::system_clock::now() - m_timeRequested < std::chrono::seconds(30)) {
		return false;
	}

	// A slow or unresponsive peer is rotated, never banned: a real bad
	// txhashset is detected and banned by the txhashset pipeline itself.
	if (m_pPeer != nullptr)
	{
		LOG_INFO_F("Rotating txhashset sync peer: {}", m_pPeer);
		m_pPeer = nullptr;
	}

	if (Global::IsRunning())
	{
		if (m_requestedHeight == 0) {
			LOG_INFO("Requesting TxHashSet for the first time.");
		}

		const uint64_t headerHeight = syncStatus.GetHeaderHeight();
		uint64_t requestedHeight = headerHeight > Consensus::STATE_SYNC_THRESHOLD
			? headerHeight - Consensus::STATE_SYNC_THRESHOLD
			: 0;
		// The archive only exists at a multiple of TXHASHSET_ARCHIVE_INTERVAL
		// (same rule as the PIBD horizon and grin txhashset_archive_header).
		requestedHeight -= requestedHeight % Consensus::TXHASHSET_ARCHIVE_INTERVAL;
		Hash hash = m_pBlockChain->GetBlockHeaderByHeight(requestedHeight, EChainType::CANDIDATE)->GetHash();
		const std::string requestedHashHex = HexUtil::ConvertToHex(hash.GetData());

		const TxHashSetRequestMessage txHashSetRequestMessage(std::move(hash), requestedHeight);
		m_pPeer = m_pConnectionManager.lock()->SendMessageToMostWorkPeer(
			txHashSetRequestMessage,
			[&syncStatus, &requestedHashHex, requestedHeight](const PeerPtr& peer) {
				syncStatus.RecordTxHashSetRequest(
					peer->GetIPAddress().Format(), requestedHashHex, requestedHeight);
			});

		if (m_pPeer != nullptr)
		{
			m_timeRequested = std::chrono::system_clock::now();
			m_requestedHeight = requestedHeight;
			// A request was actually sent: start a new log epoch so the next
			// transition (peer lost / failed / no progress) is reported again.
			m_logGate.OnRequestSent();
		}
	}

	return m_pPeer != nullptr;
}

StateSyncer::EPibdResult StateSyncer::TryPibd(SyncStatus& syncStatus)
{
	if (m_pPibdSync == nullptr) {
		return EPibdResult::NotAttempted;
	}

	if (!Global::GetConfig().IsPibdEnabled()) {
		return EPibdResult::NotAttempted;
	}

	if (std::chrono::system_clock::now() < m_pibdDisabledUntil) {
		return EPibdResult::NotAttempted;
	}

	if (std::chrono::system_clock::now() < m_pibdRetryAt) {
		return EPibdResult::InProgress;
	}

	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	const PibdSync::EStatus status = m_pPibdSync->GetStatus();

	if (status == PibdSync::EStatus::Failed) {
		LOG_WARNING_F("PIBD failed: {}", m_pPibdSync->GetError());
		syncStatus.SetPibdPeer("");
		syncStatus.SetPibdProgress(0, 0);
		syncStatus.SetPibdStarted(0);
		m_pPibdSync->Reset();
		return RetryOrDisablePibd();
	}

	if (status == PibdSync::EStatus::Idle) {
		if (m_pibdPeer == nullptr) {
			m_pibdPeers = FindPibdPeers(true);
			m_pibdPeer = m_pibdPeers.empty() ? nullptr : m_pibdPeers.front();
		}

		if (m_pibdPeer == nullptr) {
			LOG_DEBUG("No untried PIBD-capable peer connected; using txhashset.zip");
			return EPibdResult::NotAttempted;
		}

		const BlockHeaderPtr pTip = m_pBlockChain->GetTipBlockHeader(EChainType::CANDIDATE);
		if (pTip == nullptr) {
			return EPibdResult::NotAttempted;
		}

		// PIBD horizon is deterministic and must match the peer segmenter header:
		// archive_height = (header_head - STATE_SYNC_THRESHOLD) rounded down to a
		// multiple of TXHASHSET_ARCHIVE_INTERVAL (grin txhashset_archive_header_header_only).
		uint64_t archiveHeight = pTip->GetHeight();
		archiveHeight = archiveHeight > Consensus::STATE_SYNC_THRESHOLD
			? archiveHeight - Consensus::STATE_SYNC_THRESHOLD
			: 0;
		archiveHeight -= archiveHeight % Consensus::TXHASHSET_ARCHIVE_INTERVAL;

		const BlockHeaderPtr pHorizon =
			m_pBlockChain->GetBlockHeaderByHeight(archiveHeight, EChainType::CANDIDATE);
		if (pHorizon == nullptr) {
			return EPibdResult::NotAttempted;
		}

		PibdDownload::Horizon horizon;
		horizon.BlockHash = pHorizon->GetHash();
		horizon.Height = pHorizon->GetHeight();
		horizon.OutputMmrSize = pHorizon->GetOutputMMRSize();
		horizon.KernelMmrSize = pHorizon->GetKernelMMRSize();
		horizon.OutputRoot = pHorizon->GetOutputRoot();
		horizon.RangeProofRoot = pHorizon->GetRangeProofRoot();
		horizon.KernelRoot = pHorizon->GetKernelRoot();
		if (m_pibdHorizonHash != horizon.BlockHash) {
			// New horizon: allow every capable peer to be tried again. The peer
			// must be re-picked for the new horizon, otherwise the pointer stays
			// null and dereferencing it here crashes (regression caught live).
			m_pibdHorizonHash = horizon.BlockHash;
			m_pibdPeersTried.clear();
			m_pibdPeers.clear();
			m_pibdPeerCursor = 0;
			m_pibdPeer = nullptr;
		}

		if (m_pibdPeer == nullptr) {
			m_pibdPeers = FindPibdPeers(true);
			m_pibdPeer = m_pibdPeers.empty() ? nullptr : m_pibdPeers.front();
		}

		if (m_pibdPeer == nullptr) {
			LOG_DEBUG("No untried PIBD-capable peer connected; using txhashset.zip");
			return EPibdResult::NotAttempted;
		}

		if (!m_pPibdSync->Start(horizon, now)) {
			return EPibdResult::NotAttempted;
		}

		m_pibdPeersTried.insert(m_pibdPeer->GetIPAddress().Format());
		LOG_INFO_F(
			"Starting PIBD state sync at height {} (header hash {}), {} capable peer(s), primary {}.",
			horizon.Height,
			horizon.BlockHash,
			m_pibdPeers.size(),
			m_pibdPeer);
		LoggerAPI::LogSyncEvent("event=pibd_started height=" + std::to_string(horizon.Height) +
			" peers=" + std::to_string(m_pibdPeers.size()));
		syncStatus.UpdateStatus(ESyncStatus::SYNCING_TXHASHSET);
		syncStatus.SetPibdPeer(m_pibdPeer->GetIPAddress().Format());
		syncStatus.SetPibdStarted(std::time(nullptr));
		syncStatus.SetPibdProgress(0, 0);
		SendPibdRequests(now);
		return EPibdResult::InProgress;
	}

	if (status == PibdSync::EStatus::Downloading) {
		SendPibdRequests(now);
		const PibdSync::Progress progress = m_pPibdSync->GetProgress();
		syncStatus.SetPibdProgress(progress.Received, progress.Required);

		const PibdSync::EStatus updated = m_pPibdSync->GetStatus();
		if (updated == PibdSync::EStatus::Failed) {
			LOG_WARNING_F("PIBD failed: {}", m_pPibdSync->GetError());
			syncStatus.SetPibdPeer("");
			syncStatus.SetPibdProgress(0, 0);
			syncStatus.SetPibdStarted(0);
			m_pPibdSync->Reset();
			return RetryOrDisablePibd();
		}

		if (updated != PibdSync::EStatus::Completed) {
			return EPibdResult::InProgress;
		}
	}

	// Publish the final segment count after the completion transition.
	const PibdSync::Progress finalProgress = m_pPibdSync->GetProgress();
	syncStatus.SetPibdProgress(finalProgress.Received, finalProgress.Required);
	LoggerAPI::LogSyncEvent("event=pibd_download_completed received_segments=" + std::to_string(finalProgress.Received) +
		" required_segments=" + std::to_string(finalProgress.Required));

	// Completed: release the open txhashset files, write staging, activate
	// atomically and then install and validate the new state.
	m_pBlockChain->CloseTxHashSet();
	{
		ScopedSyncPhase activationPhase("pibd_write_activation");
		if (!m_pPibdSync->TryComplete()) {
			m_pBlockChain->OpenTxHashSet();
			LOG_WARNING_F(
				"PIBD write/activation failed: {}. Falling back to txhashset.zip",
				m_pPibdSync->GetError());
			m_pPibdSync->Reset();
			DisablePibd();
			return EPibdResult::NotAttempted;
		}
		activationPhase.Success();
	}

	// Keep the PIBD validation stage visible to status clients, as in the ZIP path.
	// Restore the prior sync state if validation throws so the sync loop can retry.
	syncStatus.UpdateProcessingStatus(0);
	syncStatus.UpdateStatus(ESyncStatus::PROCESSING_TXHASHSET);
	EBlockChainStatus chainStatus;
	try {
		chainStatus = m_pBlockChain->ProcessExistingTxHashSet(m_pibdHorizonHash, syncStatus);
	} catch (...) {
		syncStatus.UpdateStatus(ESyncStatus::SYNCING_TXHASHSET);
		throw;
	}
	syncStatus.UpdateStatus(ESyncStatus::SYNCING_TXHASHSET);
	m_pPibdSync->Reset();

	if (chainStatus == EBlockChainStatus::SUCCESS) {
		LOG_INFO("PIBD state sync completed and validated.");
		std::string archiveError;
		if (!ArchivePibdState(m_pibdHorizonHash, &archiveError)) {
			RecordRecoveryFailure(m_pibdHorizonHash, archiveError);
			LOG_WARNING_F(
				"Archiving the activated TxHashSet failed (bounded; no retry for this target): {}",
				archiveError);
		}
		syncStatus.SetPibdPeer("");
		syncStatus.SetPibdProgress(0, 0);
		syncStatus.SetPibdStarted(0);
		m_pibdPeer = nullptr;
		m_pibdPeers.clear();
		m_pibdPeerCursor = 0;
		m_pibdPeersTried.clear();
		m_pibdRound = 0;
		m_pibdRetryAt = std::chrono::system_clock::time_point::min();
		return EPibdResult::Completed;
	}

	LOG_WARNING("PIBD state failed chain validation; falling back to txhashset.zip");
	syncStatus.SetPibdPeer("");
	syncStatus.SetPibdProgress(0, 0);
	syncStatus.SetPibdStarted(0);
	DisablePibd();
	return EPibdResult::NotAttempted;
}

void StateSyncer::TryRecoverArchive()
{
	if (m_archiveRecoveryChecked) {
		return;
	}
	m_archiveRecoveryChecked = true;

	try {
		const fs::path marker = Global::GetConfig().GetTxHashSetPath() / "pibd_state.complete";
		if (!fs::exists(marker)) {
			return;
		}

		std::ifstream file(marker);
		std::string hashHex;
		if (!std::getline(file, hashHex) || hashHex.empty()) {
			return;
		}

		const Hash horizonHash = Hash::FromHex(hashHex);
		const BlockHeaderPtr pHeader = m_pBlockChain->GetBlockHeaderByHash(horizonHash);
		if (pHeader == nullptr) {
			return;
		}

		const fs::path archivePath = TxHashSetArchive::Path(
			Global::GetConfig().GetTxHashSetPath(), pHeader);
		std::string archiveValidationError;
		if (IsReadyArchiveForHeader(archivePath, pHeader, &archiveValidationError)) {
			ClearRecoveryBlocked();
			return;
		}
		LOG_WARNING_F(
			"Existing archive at {} failed exact-header readiness; regenerating: {}",
			archivePath.string(), archiveValidationError);

		// Fail-closed and bounded: a persisted failure marker is only honored for
		// the same target hash. A different activation/horizon gets one fresh try.
		if (IsRecoveryBlocked(horizonHash)) {
			LOG_DEBUG("TxHashSet archive recovery for this target already failed; skipping rewind retry.");
			return;
		}

		if (!IsRecoveryMarkerWritable()) {
			LOG_WARNING(
				"TxHashSet archive recovery skipped: failure marker is not writable, "
				"so the retry bound cannot be persisted.");
			return;
		}

		LOG_INFO_F("Recovering the missing TxHashSet archive at height {}.", pHeader->GetHeight());
		std::string error;
		if (!ArchivePibdState(horizonHash, &error)) {
			RecordRecoveryFailure(horizonHash, error);
			LOG_WARNING_F(
				"TxHashSet archive recovery failed (bounded; no retry for this target): {}",
				error);
		}
	}
	catch (std::exception& e) {
		LOG_WARNING_F("TxHashSet archive recovery failed: {}", e.what());
	}
}

bool StateSyncer::FlushedMatchesConfirmed(
	const BlockHeaderPtr& pFlushed,
	const BlockHeaderPtr& pConfirmedTip,
	std::string* pReason)
{
	if (pFlushed == nullptr || pConfirmedTip == nullptr) {
		if (pReason != nullptr) *pReason = "flushed/confirmed header is missing";
		return false;
	}

	if (pConfirmedTip->GetHash() != pFlushed->GetHash() ||
		pConfirmedTip->GetHeight() != pFlushed->GetHeight()) {
		if (pReason != nullptr) *pReason = "flushed TxHashSet is not the confirmed tip";
		return false;
	}

	return true;
}

bool StateSyncer::IsRecoveryMarkerPathWritable(const fs::path& markerPath)
{
	std::error_code error;
	fs::create_directories(markerPath.parent_path(), error);
	if (error) {
		return false;
	}

	// If the marker already exists (file or directory), opening it in append
	// mode proves writability without overwriting the diagnostic content.
	if (fs::exists(markerPath, error)) {
		if (error) {
			return false;
		}
		std::ofstream existing(markerPath, std::ios::app);
		return existing.good();
	}

	// Nonexistent marker: probe a sibling name in the same parent directory.
	const fs::path probe = markerPath.parent_path() /
		(markerPath.filename().string() + ".write_probe");
	{
		std::ofstream out(probe, std::ios::trunc);
		if (!out.good()) {
			return false;
		}
	}
	fs::remove(probe, error);
	return true;
}

bool StateSyncer::PreflightArchiveRewind(const BlockHeaderPtr& pTarget, std::string* pError)
{
	std::string localError;
	std::string& error = pError != nullptr ? *pError : localError;

	if (pTarget == nullptr) {
		error = "archive preflight: target header is missing";
		return false;
	}

	// The snapshot/Rewind starts from the TxHashSet flushed header, not from the
	// confirmed chain tip. Require the two views to be identical before using
	// confirmed headers to validate a rewind range.
	const std::shared_ptr<ITxHashSet> pTxHashSet = m_pBlockChain->GetTxHashSet();
	if (pTxHashSet == nullptr) {
		error = "archive preflight: live TxHashSet is missing";
		return false;
	}

	const BlockHeaderPtr pFlushed = pTxHashSet->GetFlushedBlockHeader();
	if (pFlushed == nullptr) {
		error = "archive preflight: TxHashSet flushed header is missing";
		return false;
	}

	const BlockHeaderPtr pConfirmedTip =
		m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
	if (pConfirmedTip == nullptr) {
		error = "archive preflight: confirmed tip header is missing";
		return false;
	}

	if (!FlushedMatchesConfirmed(pFlushed, pConfirmedTip, &error)) {
		return false;
	}

	const uint64_t targetHeight = pTarget->GetHeight();
	const uint64_t flushedHeight = pFlushed->GetHeight();
	const std::string targetHashHex =
		HexUtil::ConvertToHex(pTarget->GetHash().GetData());

	// The target body/spent record is not required: Rewind unwinds only
	// heights flushedTip..target+1 and stops at the target header. Target hash
	// equality is still checked exactly.
	const auto viewAtHeight = [this, targetHeight](const uint64_t height) {
		ArchivePublication::RewindPreflightView view;
		const BlockHeaderPtr pHeader =
			m_pBlockChain->GetBlockHeaderByHeight(height, EChainType::CONFIRMED);
		if (pHeader == nullptr) {
			return view;
		}

		view.headerPresent = true;
		view.headerHashHex = HexUtil::ConvertToHex(pHeader->GetHash().GetData());
		if (height == targetHeight) {
			return view;
		}

		const std::unique_ptr<FullBlock> pBlock = m_pBlockChain->GetBlockByHeight(height);
		if (pBlock == nullptr) {
			return view;
		}

		view.bodyPresent = true;
		const std::vector<Commitment> inputs = pBlock->GetInputCommitments();
		view.inputCount = inputs.size();
		size_t mappedCount = 0;
		view.spentComplete = m_pBlockChain->AreSpentPositionsComplete(
			pHeader->GetHash(), inputs, mappedCount);
		view.mappedCount = mappedCount;
		return view;
	};

	return ArchivePublication::ValidateRewindPreflight(
		targetHashHex,
		targetHeight,
		flushedHeight,
		viewAtHeight,
		&error);
}

bool StateSyncer::ArchivePibdState(const Hash& horizonHash, std::string* pError)
{
	std::string localError;
	std::string& error = pError != nullptr ? *pError : localError;
	fs::path staged;
	try {
		const BlockHeaderPtr pHeader = m_pBlockChain->GetBlockHeaderByHash(horizonHash);
		if (pHeader == nullptr) {
			error = "archive header is missing";
			return false;
		}

		const fs::path archivePath = TxHashSetArchive::Path(
			Global::GetConfig().GetTxHashSetPath(),
			pHeader);
		// Readiness must validate the archive against the exact target header, not
		// merely check container entries. A complete-but-wrong-root archive must
		// not suppress regeneration.
		if (IsReadyArchiveForHeader(archivePath, pHeader, &error)) {
			ClearRecoveryBlocked();
			return true;
		}

		// Refuse before any copy/rewrite when the canonical DB cannot support the
		// rewind. This does not mutate the chain or the live txhashset, and a
		// failure is persisted by the caller so restart does not retry forever.
		if (!PreflightArchiveRewind(pHeader, &error)) {
			return false;
		}

		const fs::path zipPath = m_pBlockChain->SnapshotTxHashSet(pHeader);
		if (zipPath.empty()) {
			error = "PIBD activation produced an empty archive snapshot";
			return false;
		}
		FileRemover zipRemover(zipPath);

		if (!ArchivePublication::StageSnapshot(zipPath, archivePath, &error)) {
			return false;
		}
		staged = ArchivePublication::StagingPath(archivePath);

		// Validate MMR sizes + roots against the target header before publishing.
		// This extracts to a private temp directory; it never touches the live
		// txhashset or the confirmed chain.
		static std::atomic<uint64_t> s_validationId{ 0 };
		const fs::path validationRoot = fs::temp_directory_path() /
			("g09-archive-validate-" + pHeader->ShortHash() + "-" +
				std::to_string(s_validationId.fetch_add(1)));
		FileRemover validationRemover(validationRoot);
		TxHashSetZip archiveZip(Global::GetConfig());
		if (!archiveZip.Extract(staged, *pHeader, validationRoot)) {
			error = "archive extraction failed before publication";
			std::error_code removeError;
			fs::remove(staged, removeError);
			return false;
		}

		std::string validationError;
		if (PibdArchiveServer::LoadAndValidate(validationRoot, pHeader, &validationError) == nullptr) {
			error = validationError.empty() ? "archive validation failed before publication" : validationError;
			std::error_code removeError;
			fs::remove(staged, removeError);
			return false;
		}

		if (!ArchivePublication::PublishStaged(staged, archivePath, &error)) {
			return false;
		}

		ClearRecoveryBlocked();
		LOG_INFO_F(
			"Archived TxHashSet at height {} for serving ({})",
			pHeader->GetHeight(),
			archivePath.string());
		return true;
	}
	catch (std::exception& e) {
		error = e.what();
	}
	catch (...) {
		error = "unknown error";
	}

	if (!staged.empty()) {
		std::error_code removeError;
		fs::remove(staged, removeError);
	}
	LOG_WARNING_F("Failed to archive the activated TxHashSet: {}", error);
	return false;
}

void StateSyncer::DisablePibd()
{
	// Do not immediately retry PIBD; let the legacy state sync have a full attempt.
	m_pibdDisabledUntil = std::chrono::system_clock::now() + std::chrono::minutes(120);
	LOG_INFO("PIBD disabled for 120 minutes after a failure.");
}

void StateSyncer::SendPibdRequests(const std::chrono::steady_clock::time_point now)
{
	if (m_pPibdSync == nullptr) {
		return;
	}

	ConnectionManagerPtr pConnectionManager = m_pConnectionManager.lock();
	if (pConnectionManager == nullptr) {
		return;
	}

	std::vector<PeerPtr> peers;
	std::vector<std::string> tokens;
	for (const PeerPtr& pPeer : m_pibdPeers) {
		if (pPeer != nullptr && pConnectionManager->IsConnected(pPeer->GetIPAddress())) {
			peers.push_back(pPeer);
			tokens.push_back(pPeer->GetIPAddress().Format());
		}
	}
	if (peers.empty() && m_pibdPeer != nullptr &&
		pConnectionManager->IsConnected(m_pibdPeer->GetIPAddress())) {
		peers.push_back(m_pibdPeer);
		tokens.push_back(m_pibdPeer->GetIPAddress().Format());
	}
	if (peers.empty()) {
		peers = FindPibdPeers(false);
		for (const PeerPtr& pPeer : peers) {
			tokens.push_back(pPeer->GetIPAddress().Format());
		}
	}
	if (peers.empty()) {
		return;
	}

	// A peer that disappeared must release its assignments so those segments can
	// be reassigned instead of waiting for the full timeout.
	for (const PeerPtr& pOld : m_pibdPeers) {
		if (pOld == nullptr) {
			continue;
		}
		const std::string oldToken = pOld->GetIPAddress().Format();
		if (std::find(tokens.begin(), tokens.end(), oldToken) == tokens.end()) {
			const size_t released = m_pPibdSync->ReleasePeer(oldToken);
			LOG_DEBUG_F("PIBD: released {} assignment(s) from disconnected peer {}", released, oldToken);
		}
	}
	m_pibdPeers = peers;
	m_pibdPeer = peers.front();

	constexpr size_t PER_PEER_IN_FLIGHT = 4;
	const std::vector<PibdScheduler::AssignedRequest> requests =
		m_pPibdSync->TakeRequestsForPeers(now, tokens, PER_PEER_IN_FLIGHT);
	if (!requests.empty()) {
		LOG_DEBUG_F("Sending {} PIBD request(s) across {} capable peer(s)",
			requests.size(), peers.size());
	}

	for (const PibdScheduler::AssignedRequest& assigned : requests) {
		PeerPtr pTarget = nullptr;
		for (const PeerPtr& pPeer : peers) {
			if (pPeer->GetIPAddress().Format() == assigned.Peer) {
				pTarget = pPeer;
				break;
			}
		}
		if (pTarget == nullptr ||
			!pConnectionManager->SendMessageToPeer(*assigned.Message, pTarget)) {
			// Send failure: the request is not in flight on any peer. Release it so
			// the next TakeRequestsForPeers can reassign it immediately.
			m_pPibdSync->ReleaseAssignment(assigned);
			LOG_DEBUG_F("PIBD: send to {} failed; released {}", assigned.Peer,
				assigned.Request.Identifier.GetIdx());
		}
	}
}

PeerPtr StateSyncer::FindPibdPeer() const
{
	return FindPibdPeer(false);
}

PeerPtr StateSyncer::FindPibdPeer(const bool excludeTried) const
{
	const std::vector<PeerPtr> peers = FindPibdPeers(excludeTried);
	return peers.empty() ? nullptr : peers.front();
}

std::vector<PeerPtr> StateSyncer::FindPibdPeers(const bool excludeTried) const
{
	ConnectionManagerPtr pConnectionManager = m_pConnectionManager.lock();
	if (pConnectionManager == nullptr) {
		return {};
	}

	struct Candidate { PeerPtr Peer; uint64_t Height; };
	std::vector<Candidate> candidates;
	for (ConnectedPeer connectedPeer : pConnectionManager->GetConnectedPeers()) {
		PeerPtr pPeer = connectedPeer.GetPeer();
		if (pPeer == nullptr) {
			continue;
		}

		if (!pPeer->GetCapabilities().HasCapability(Capabilities::PIBD_HIST_1)) {
			continue;
		}

		if (excludeTried &&
			m_pibdPeersTried.find(pPeer->GetIPAddress().Format()) != m_pibdPeersTried.cend()) {
			continue;
		}

		candidates.push_back({ pPeer, connectedPeer.GetHeight() });
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		return a.Height > b.Height;
	});
	std::vector<PeerPtr> peers;
	peers.reserve(candidates.size());
	for (const Candidate& candidate : candidates) {
		peers.push_back(candidate.Peer);
	}
	return peers;
}

bool StateSyncer::RotatePibdPeer()
{
	m_pibdPeer = FindPibdPeer(true);
	if (m_pibdPeer == nullptr) {
		return false;
	}

	LOG_INFO_F("PIBD: retrying with another peer {}", m_pibdPeer);
	return true;
}

StateSyncer::EPibdResult StateSyncer::RetryOrDisablePibd()
{
	m_pibdPeer = nullptr;
	if (RotatePibdPeer()) {
		return EPibdResult::InProgress;
	}

	if (m_pibdRound < 3) {
		m_pibdRound++;
		m_pibdPeersTried.clear();
		const auto delay = std::chrono::seconds(30u << (m_pibdRound - 1));
		m_pibdRetryAt = std::chrono::system_clock::now() + delay;
		LOG_INFO_F(
			"PIBD: all capable peers tried; retry round {} in {}s",
			m_pibdRound,
			delay.count());
		return EPibdResult::InProgress;
	}

	LOG_WARNING("PIBD: all capable peers failed in 3 rounds; falling back to txhashset.zip");
	DisablePibd();
	return EPibdResult::NotAttempted;
}



