#include "TxHashSetProcessor.h"

#include <Common/Logger.h>
#include <Database/BlockDb.h>
#include <Core/Models/BlockHeader.h>
#include <Core/Global.h>
#include <BlockChain/BlockChain.h>
#include <Common/Util/StringUtil.h>
#include <Common/Util/HexUtil.h>
#include <Consensus.h>
#include "../../PMMR/TxHashSetImpl.h"
#include <PMMR/TxHashSetStaging.h>
#include <fstream>

namespace
{
	const char* FailureKindName(const TxHashSetProcessor::EFailureKind kind)
	{
		switch (kind) {
		case TxHashSetProcessor::EFailureKind::None:
			return "none";
		case TxHashSetProcessor::EFailureKind::LocalUnverifiable:
			return "local-unverifiable";
		case TxHashSetProcessor::EFailureKind::InvalidState:
			return "invalid-nrd-history";
		}

		return "unknown";
	}
}

TxHashSetProcessor::EFailureKind TxHashSetProcessor::VerifyNrdIndexBeforeCommit(
	ITxHashSet& txHashSet,
	const std::shared_ptr<const IBlockDB>& pBlockDB,
	const BlockHeader& header)
{
	// TxHashSet is currently the only ITxHashSet implementation. Treat an
	// unexpected implementation as locally unverifiable, never as peer malice.
	TxHashSet* pConcrete = dynamic_cast<TxHashSet*>(&txHashSet);
	if (pConcrete == nullptr) {
		LOG_ERROR("NRD pre-commit verification: unexpected ITxHashSet implementation");
		return EFailureKind::LocalUnverifiable;
	}

	const uint64_t headerKernelCount = header.GetNumKernels();
	const bool kernelCountsMatch = txHashSet.GetNumKernels() == headerKernelCount;

	// The rebuild reads the header chain inside the two-week window. If any
	// header is missing locally the state cannot be verified; that is local
	// incompleteness, not evidence against the peer.
	const uint64_t cutoff = header.GetHeight() > 2 * Consensus::WEEK_HEIGHT
		? header.GetHeight() - 2 * Consensus::WEEK_HEIGHT
		: 0;

	bool headersAvailable = (pBlockDB != nullptr);
	uint64_t previousKernelCount = 0;
	if (headersAvailable) {
		BlockHeaderPtr pCurrent = std::make_shared<BlockHeader>(header);
		while (pCurrent->GetHeight() > cutoff) {
			const BlockHeaderPtr pPrevious = pBlockDB->GetBlockHeader(pCurrent->GetPreviousHash());
			if (pPrevious == nullptr || pPrevious->GetHeight() + 1 != pCurrent->GetHeight()) {
				headersAvailable = false;
				break;
			}

			pCurrent = pPrevious;
		}

		if (headersAvailable) {
			if (pCurrent->GetHeight() != cutoff) {
				headersAvailable = false;
			} else if (cutoff > 0) {
				const BlockHeaderPtr pBeforeCutoff = pBlockDB->GetBlockHeader(pCurrent->GetPreviousHash());
				if (pBeforeCutoff == nullptr || pBeforeCutoff->GetHeight() + 1 != cutoff) {
					headersAvailable = false;
				} else {
					previousKernelCount = pBeforeCutoff->GetNumKernels();
				}
			}
		}
	}

	bool kernelLeavesAvailable = kernelCountsMatch && headersAvailable;
	if (kernelLeavesAvailable) {
		for (uint64_t leaf = previousKernelCount; leaf < headerKernelCount; ++leaf) {
			if (txHashSet.GetKernelByLeafIndex(leaf) == nullptr) {
				kernelLeavesAvailable = false;
				break;
			}
		}
	}

	// Always run the real rebuild: it leaves the index incomplete on any
	// failure, which is the fail-closed state until the caller decides.
	pConcrete->RebuildRecentKernelIndex(pBlockDB, header);

	if (pConcrete->IsRecentNrdIndexComplete()) {
		return EFailureKind::None;
	}

	// A local read error during the rebuild is never peer evidence, even when the
	// prereq checks happened to read successfully before the failure. Keep this
	// before the InvalidState classification so an IO error cannot become a ban.
	if (pConcrete->DidLastNrdRebuildThrow()) {
		LOG_ERROR_F(
			"NRD recent-kernel index rebuild failed with a local read error at height {}; treating as locally unverifiable (no peer-ban evidence)",
			header.GetHeight());
		return EFailureKind::LocalUnverifiable;
	}

	if (!kernelLeavesAvailable) {
		LOG_WARNING_F(
			"NRD recent-kernel index cannot be verified locally at height {} (kernels match header: {}, headers available: {}, kernel leaves available: {}); no peer-ban evidence",
			header.GetHeight(),
			kernelCountsMatch ? "yes" : "no",
			headersAvailable ? "yes" : "no",
			kernelLeavesAvailable ? "yes" : "no");
		return EFailureKind::LocalUnverifiable;
	}

	LOG_ERROR_F(
		"NRD recent-kernel index rebuild found a historical relative-height violation at height {} (all prerequisites locally available)",
		header.GetHeight());
	return EFailureKind::InvalidState;
}

TxHashSetProcessor::TxHashSetProcessor(
	const Config& config,
	IBlockChain& blockChain,
	std::shared_ptr<Locked<ChainState>> pChainState)
	: m_config(config),
	m_blockChain(blockChain),
	m_pChainState(pChainState)
{

}

bool TxHashSetProcessor::ProcessTxHashSet(const Hash& blockHash, const fs::path& path, SyncStatus& syncStatus)
{
	m_lastFailureKind = EFailureKind::None;

	auto pHeader = m_pChainState->Read()->GetBlockHeaderByHash(blockHash);
	if (pHeader == nullptr)
	{
		LOG_ERROR_F("Header not found for hash {}.", blockHash);
		return false;
	}

	const fs::path livePath = m_config.GetTxHashSetPath();
	const auto pOldTip = m_pChainState->Read()->GetChainStore()->GetConfirmedChain()->GetTip();
	const std::string oldTipHash = pOldTip == nullptr ? "" : pOldTip->GetHash().ToHex();
	TxHashSetStaging::Recover(livePath, oldTipHash);
	const fs::path stagingPath = TxHashSetStaging::Staging(livePath);
	const fs::path backupPath = TxHashSetStaging::Backup(livePath);

	std::error_code ec;
	fs::remove_all(stagingPath, ec);

	const auto reopenLive = [&]() {
		const auto pTip = m_pChainState->Read()->GetChainStore()->GetConfirmedChain()->GetTip();
		if (pTip == nullptr) {
			return;
		}

		const auto pTipHeader = m_pChainState->Read()->GetBlockDB()->GetBlockHeader(pTip->GetHash());
		if (pTipHeader != nullptr) {
			m_pChainState->Write()->GetTxHashSetManager()->Open(pTipHeader);
		}
	};

	// 1. Extract and validate into a staging directory; the live state is untouched.
	m_pChainState->Write()->GetTxHashSetManager()->Close();
	ITxHashSetPtr pStaged = TxHashSetManager::LoadFromZip(m_config, path, pHeader, stagingPath);
	if (pStaged == nullptr)
	{
		LOG_ERROR_F("Failed to load {}", path);
		fs::remove_all(stagingPath, ec);
		reopenLive();
		return false;
	}

	// 2. Validate the whole staged state.
	auto pBlockSums = pStaged->ValidateTxHashSet(*pHeader, m_blockChain, syncStatus);
	if (pBlockSums == nullptr)
	{
		LOG_ERROR_F("Validation of {} failed; keeping the existing state.", path);
		pStaged.reset();
		fs::remove_all(stagingPath, ec);
		reopenLive();
		return false;
	}

	// 2b. Rebuild and verify the NRD recent-kernel index on the staged state
	// BEFORE any live file is replaced or any ChainState batch is committed.
	// Rust grin 5.5.2 verifies verify_kernel_pos_index before txhashset commit;
	// refusing here leaves the previous working state completely untouched.
	{
		const auto pBlockDBShared = m_pChainState->Read()->GetBlockDB().GetShared();
		m_lastFailureKind = VerifyNrdIndexBeforeCommit(*pStaged, pBlockDBShared, *pHeader);
		if (m_lastFailureKind != EFailureKind::None) {
			LOG_ERROR_F(
				"Rejecting txhashset {} before activation ({}): NRD recent-kernel index verification failed at tip {} height {}; keeping the existing state.",
				path,
				FailureKindName(m_lastFailureKind),
				pHeader->GetHash(),
				pHeader->GetHeight());
			pStaged.reset();
			fs::remove_all(stagingPath, ec);
			reopenLive();
			return false;
		}
	}

	// 3. Release file handles before renaming the directories.
	pStaged.reset();

	// 4. Atomic swap with a marker for crash recovery.
	fs::remove_all(backupPath, ec);
	if (ec) {
		LOG_ERROR_F("Failed to clear old TxHashSet backup: {}", ec.message());
		reopenLive();
		return false;
	}
	TxHashSetStaging::Begin(livePath, blockHash.ToHex());

	try {
		if (fs::exists(livePath, ec)) {
			fs::rename(livePath, backupPath);
		}
		fs::rename(stagingPath, livePath);
	}
	catch (std::exception& e) {
		LOG_ERROR_F("Failed to activate the staged txhashset: {}", e.what());
		TxHashSetStaging::Recover(livePath, oldTipHash);
		reopenLive();
		return false;
	}

	// 5. Open the installed state and publish block sums/output positions.
	auto pManager = m_pChainState->Write()->GetTxHashSetManager();
	ITxHashSetPtr pLive = pManager->Open(pHeader, false);
	if (pLive == nullptr) {
		LOG_ERROR_F("Failed to open the activated txhashset for {}.", path);
		TxHashSetStaging::Recover(livePath, oldTipHash);
		reopenLive();
		return false;
	}

	auto pChainStateBatch = m_pChainState->BatchWrite();
	pChainStateBatch->GetBlockDB()->AddBlockSums(pHeader->GetHash(), *pBlockSums);
	LOG_DEBUG("Saving output positions.");
	pLive->SaveOutputPositions(
		pChainStateBatch->GetChainStore()->GetCandidateChain(),
		pChainStateBatch->GetBlockDB());

	LOG_DEBUG("Using TxHashSet.");
	pChainStateBatch->GetTxHashSetManager()->SetTxHashSet(pLive);

	LOG_DEBUG("Updating confirmed chain.");
	if (!UpdateConfirmedChain(pChainStateBatch, *pHeader))
	{
		LOG_ERROR_F("Failed to update confirmed chain for {}.", path);
		pChainStateBatch->GetTxHashSetManager()->Close();
		pLive.reset();
		pChainStateBatch.Clear();
		pManager->Close();
		TxHashSetStaging::Recover(livePath, oldTipHash);
		reopenLive();
		return false;
	}

	try {
		pChainStateBatch->Commit();
	} catch (const std::exception& e) {
		LOG_ERROR_F("TxHashSet chain DB commit failed: {}; stopping the node so the checkpoint and backup can be reconciled on restart.", e.what());
		pChainStateBatch->GetTxHashSetManager()->Close();
		pLive.reset();
		pChainStateBatch.Clear();
		pManager->Close();
		// A partially completed cross-store commit must not be judged from the
		// in-memory tip. Startup first loads the durable DB checkpoint and then
		// chooses old/new files from the pending activation marker.
		Global::Shutdown();
		return false;
	}
	pChainStateBatch.Clear();
	try {
		TxHashSetStaging::Finish(livePath);
	} catch (const std::exception& e) {
		// The DB commit already succeeded. Startup recovery can finish cleanup.
		LOG_WARNING_F("TxHashSet committed but backup cleanup is pending: {}", e.what());
	}
	return true;
}
bool TxHashSetProcessor::ProcessExistingTxHashSet(const Hash& blockHash, SyncStatus& syncStatus)
{
	m_lastFailureKind = EFailureKind::None;

	auto pHeader = m_pChainState->Read()->GetBlockHeaderByHash(blockHash);
	if (pHeader == nullptr) {
		LOG_ERROR_F("Header not found for hash {}.", blockHash);
		return false;
	}

	// 1. Close the existing txhashset and open the activated files.
	m_pChainState->Write()->GetTxHashSetManager()->Close();
	ITxHashSetPtr pTxHashSet = m_pChainState->Write()->GetTxHashSetManager()->Open(pHeader);
	if (pTxHashSet == nullptr) {
		LOG_ERROR("Failed to open the activated txhashset.");
		return false;
	}

	// 2. Validate the entire txhashset against the horizon header.
	auto pBlockSums = pTxHashSet->ValidateTxHashSet(*pHeader, m_blockChain, syncStatus);
	if (pBlockSums == nullptr) {
		LOG_ERROR("Validation of the activated txhashset failed.");
		return false;
	}

	// 2b. Rebuild and verify the NRD recent-kernel index BEFORE the ChainState
	// batch is committed. On refusal the confirmed-chain state stays untouched
	// (the already-activated files are a pre-existing PIBD limitation; the
	// incomplete index keeps NRD blocks fail-closed until the next state write).
	{
		const auto pBlockDBShared = m_pChainState->Read()->GetBlockDB().GetShared();
		m_lastFailureKind = VerifyNrdIndexBeforeCommit(*pTxHashSet, pBlockDBShared, *pHeader);
		if (m_lastFailureKind != EFailureKind::None) {
			LOG_ERROR_F(
				"Refusing the activated txhashset ({}): NRD recent-kernel index verification failed at tip {} height {}; confirmed chain is not committed.",
				FailureKindName(m_lastFailureKind),
				pHeader->GetHash(),
				pHeader->GetHeight());
			return false;
		}
	}

	// 3. Store block sums and output positions, then install the txhashset.
	auto pChainStateBatch = m_pChainState->BatchWrite();
	pChainStateBatch->GetBlockDB()->AddBlockSums(pHeader->GetHash(), *pBlockSums);
	pTxHashSet->SaveOutputPositions(
		pChainStateBatch->GetChainStore()->GetCandidateChain(), pChainStateBatch->GetBlockDB());
	pChainStateBatch->GetTxHashSetManager()->SetTxHashSet(pTxHashSet);

	// 4. Update the confirmed chain to the horizon.
	if (!UpdateConfirmedChain(pChainStateBatch, *pHeader)) {
		LOG_ERROR("Failed to update the confirmed chain for the activated txhashset.");
		pChainStateBatch->GetTxHashSetManager()->Close();
		return false;
	}

	pChainStateBatch->Commit();
	return true;
}
bool TxHashSetProcessor::UpdateConfirmedChain(Writer<ChainState> pLockedState, const BlockHeader& blockHeader)
{
	std::shared_ptr<Chain> pCandidateChain = pLockedState->GetChainStore()->GetCandidateChain();
	std::shared_ptr<Chain> pConfirmedChain = pLockedState->GetChainStore()->GetConfirmedChain();
	
	std::shared_ptr<const BlockIndex> pBlockIndex = pCandidateChain->GetByHeight(blockHeader.GetHeight());
	if (pBlockIndex->GetHash() != blockHeader.GetHash())
	{
		return false;
	}

	std::shared_ptr<const BlockIndex> pCommonIndex = pLockedState->GetChainStore()->FindCommonIndex(EChainType::CANDIDATE, EChainType::CONFIRMED);
	pConfirmedChain->Rewind(pCommonIndex->GetHeight());

	uint64_t height = pCommonIndex->GetHeight() + 1;
	while (height <= pBlockIndex->GetHeight())
	{
		pConfirmedChain->AddBlock(pCandidateChain->GetHash(height), height);
		height++;
	}

	return true;
}
