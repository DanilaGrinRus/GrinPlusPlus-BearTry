#include "WalletRefresher.h"
#include "OutputRestorer.h"
#include <Wallet/Keychain/KeyChain.h>

#include <Common/Logger.h>
#include <Wallet/WalletUtil.h>
#include <Wallet/NodeClient.h>
#include <Wallet/WalletDB/WalletDB.h>
#include <unordered_map>

// TODO: Rewrite this && use cache - Shouldn't refresh when block height = same
// 0. Initial login after upgrade - for every output, find matching WalletTx and update OutputDataEntity TxId. If none found, create new WalletTx.
//
// 1. Refresh statuses of existing outputs (a detected reorg rewinds the restore cursor first).
// 2. Check for own outputs in new blocks (and in reorg-replaced blocks).
// 3. For each new output, create WalletTx and add the output & tx to the database.
// 4. Update matching WalletTx status.
// 5. Only commit if the node tip stayed stable for the whole refresh.

std::optional<NodeTip> WalletRefresher::TryGetTip() const
{
	try
	{
		return m_pNodeClient->GetTip();
	}
	catch (const NodeUnavailableException& e)
	{
		WALLET_WARNING_F("Unable to get node tip: {}", e.what());
		return std::nullopt;
	}
}

EOutputStatus WalletRefresher::DetermineMissingOutputStatus(
	const EOutputStatus currentStatus,
	const std::optional<uint64_t>& outputBlockHeight,
	const uint64_t nodeHeight,
	const uint64_t refreshBlockHeight,
	const EKernelLookupStatus kernelStatus)
{
	// NO_CONFIRMATIONS: the output has not been seen on chain yet, an omission is expected.
	// SPENT / CANCELED: terminal / locally final states, nothing to update here.
	if (currentStatus == EOutputStatus::NO_CONFIRMATIONS ||
		currentStatus == EOutputStatus::SPENT ||
		currentStatus == EOutputStatus::CANCELED)
	{
		return currentStatus;
	}

	// A reverted output does not exist in the canonical UTXO set, so it cannot be
	// spent there. Only evidence that its transaction made it on chain (kernel
	// found) turns it into SPENT; otherwise it stays REVERTED until it reappears.
	if (currentStatus == EOutputStatus::REVERTED)
	{
		return kernelStatus == EKernelLookupStatus::FOUND
			? EOutputStatus::SPENT
			: EOutputStatus::REVERTED;
	}

	// A spend (or a reorg replacement) requires the chain to have advanced past
	// the output's confirmation height. Below/at that height the output cannot
	// have been spent, so an omission means the confirming block is no longer
	// canonical (or the answer is inconsistent). Never report SPENT here.
	if (outputBlockHeight.has_value() && nodeHeight <= outputBlockHeight.value())
	{
		return EOutputStatus::REVERTED;
	}

	switch (kernelStatus)
	{
	case EKernelLookupStatus::FOUND:
		// The transaction is on the canonical chain: the output existed, and its
		// absence from the unspent set means it was spent later.
		return EOutputStatus::SPENT;

	case EKernelLookupStatus::NOT_FOUND:
		// The transaction kernel is gone: the confirming block was reorged out.
		return EOutputStatus::REVERTED;

	case EKernelLookupStatus::UNAVAILABLE:
	default:
		break;
	}

	// No kernel information available (e.g. a received output restored from the
	// chain). Only mark SPENT when the chain demonstrably advanced since the last
	// successful refresh; at the same height an omission is either a same-height
	// reorg or a partial/inconsistent response, and a sticky false SPENT would
	// hide spendable funds.
	if (nodeHeight > refreshBlockHeight)
	{
		return EOutputStatus::SPENT;
	}

	return EOutputStatus::REVERTED;
}

std::vector<OutputDataEntity> WalletRefresher::Refresh(const SecureVector& masterSeed, Locked<IWalletDB> walletDB, const bool fromGenesis)
{
	// Anchor every decision below to a single observed tip. If the node cannot
	// provide one, leave the wallet untouched.
	std::optional<NodeTip> pTip = TryGetTip();
	if (!pTip.has_value())
	{
		return walletDB.Read()->GetOutputs(masterSeed);
	}

	const uint64_t refreshBlockHeight = walletDB.Read()->GetRefreshBlockHeight();

	if (pTip->height < refreshBlockHeight)
	{
		// The node is behind the wallet (resync or a rewound chain). Do not mutate
		// statuses. Rewind the restore cursor so blocks that replace the old tip
		// are rescanned once the node catches up.
		WALLET_TRACE("Skipping refresh since node is resyncing.");

		try
		{
			auto pBatch = walletDB.BatchWrite();
			const uint64_t rewindHeight = pTip->height > REORG_RESCAN_WINDOW
				? pTip->height - REORG_RESCAN_WINDOW
				: 0;
			RewindRestoreCursor(pBatch, rewindHeight);
			pBatch->Commit();
		}
		catch (const NodeException& e)
		{
			WALLET_WARNING_F("Failed to rewind the restore cursor: {}", e.what());
		}

		// Return the current (unchanged) wallet state instead of an empty vector:
		// callers must not interpret "refresh skipped" as "wallet is empty".
		return walletDB.Read()->GetOutputs(masterSeed);
	}

	std::vector<OutputDataEntity> refreshedOutputs;
	bool committed = false;

	try
	{
		auto pBatch = walletDB.BatchWrite();

		// Re-read the refresh height under the write lock: a concurrent refresh may
		// have advanced it after the optimistic read above. Applying statuses from
		// an older tip could resurrect/rewind durable state.
		const uint64_t batchRefreshBlockHeight = pBatch->GetRefreshBlockHeight();
		if (pTip->height < batchRefreshBlockHeight) {
			throw NodeUnavailableException("Node tip is behind the wallet refresh height.");
		}

		std::vector<OutputDataEntity> walletOutputs = pBatch->GetOutputs(masterSeed);
		std::vector<WalletTx> walletTransactions = pBatch->GetTransactions(masterSeed);

		// 1. Refresh statuses first. A detected reorg rewinds the restore cursor,
		//    so the scan below re-reads replaced blocks in the same pass.
		std::optional<uint64_t> rewindHeightOpt;
		RefreshOutputStatuses(
			masterSeed,
			pBatch,
			walletTransactions,
			walletOutputs,
			pTip->height,
			batchRefreshBlockHeight,
			rewindHeightOpt
		);

		if (rewindHeightOpt.has_value())
		{
			RewindRestoreCursor(pBatch, rewindHeightOpt.value());
		}

		// 2. Check for outputs we had not seen before (or that reappeared after a reorg).
		std::vector<OutputDataEntity> newOutputs = FindNewOutputs(masterSeed, pBatch, walletOutputs, fromGenesis);

		// 3. Create a new WalletTx for each newly received output, and add the output & tx to the database.
		for (OutputDataEntity& newOutput : newOutputs)
		{
			WALLET_INFO_F("Restoring unknown output: {}", newOutput);

			auto blockTimeOpt = GetBlockTime(newOutput);

			const uint32_t walletTxId = pBatch->GetNextTransactionId();
			WalletTx walletTx(
				walletTxId,
				EWalletTxType::RECEIVED,
				std::nullopt,
				std::nullopt,
				std::nullopt,
				blockTimeOpt.value_or(std::chrono::system_clock::now()),
				blockTimeOpt,
				newOutput.GetBlockHeight(),
				newOutput.GetAmount(),
				0,
				std::nullopt,
				std::nullopt,
				std::nullopt
			);

			newOutput.SetWalletTxId(walletTxId);
			pBatch->AddOutputs(masterSeed, std::vector<OutputDataEntity>({ newOutput }));
			pBatch->AddTransaction(masterSeed, walletTx);

			walletOutputs.push_back(newOutput);
			walletTransactions.emplace_back(std::move(walletTx));
		}

		// 4. For all wallet outputs, update matching WalletTx status.
		RefreshTransactions(masterSeed, pBatch, walletOutputs, walletTransactions);

		// 5. Commit only if the node tip did not move while the outputs and
		//    statuses were collected. A response from a chain that is no longer
		//    the tip must not be turned into durable SPENT/REVERTED statuses.
		std::optional<NodeTip> pTipAfter = TryGetTip();
		if (!pTipAfter.has_value() ||
			pTipAfter->height != pTip->height ||
			!(pTipAfter->hash == pTip->hash))
		{
			throw NodeUnavailableException("Chain tip changed during refresh.");
		}

		pBatch->UpdateRefreshBlockHeight(pTip->height);
		pBatch->Commit();

		refreshedOutputs = walletOutputs;
		committed = true;
	}
	catch (const NodeUnavailableException& e)
	{
		// Batch is destroyed (rolled back) before this handler runs, so no partial
		// status/cursor update is persisted.
		WALLET_WARNING_F("Refresh skipped: {}", e.what());
	}

	if (!committed)
	{
		return walletDB.Read()->GetOutputs(masterSeed);
	}

	return refreshedOutputs;
}

std::vector<OutputDataEntity> WalletRefresher::FindNewOutputs(
	const SecureVector& masterSeed,
	Writer<IWalletDB> pBatch,
	std::vector<OutputDataEntity>& walletOutputs,
	const bool fromGenesis)
{
	std::vector<OutputDataEntity> new_outputs;

	KeyChain keyChain = KeyChain::FromSeed(masterSeed);
	OutputRestorer restorer(m_pNodeClient, keyChain);
	std::vector<OutputDataEntity> restoredOutputs = restorer.FindAndRewindOutputs(pBatch.GetShared(), fromGenesis);

	// For each restored output, look for known outputs with matching commitment.
	// If no matching output is found, this must be a new output that was received by another one of the user's wallets with the same seed.
	for (OutputDataEntity& restoredOutput : restoredOutputs) {
		WALLET_INFO_F("Output found at index {}", restoredOutput.GetMMRIndex().value_or(0));

		if (restoredOutput.GetStatus() == EOutputStatus::SPENT) {
			continue;
		}

		auto pExistingOutput = FindOutput(walletOutputs, restoredOutput.GetCommitment());
		if (pExistingOutput == nullptr) {
			new_outputs.push_back(restoredOutput);
			continue;
		}

		// The node reports this output as unspent, but the wallet had it in a
		// terminal state (e.g. SPENT marked by an older build for a block that was
		// later reorged out, or CANCELED). The chain wins: resurrect the output as
		// SPENDABLE. Without this, SPENT/CANCELED outputs are not queried for
		// status (per-request commitment budget) and could never recover.
		const EOutputStatus existingStatus = pExistingOutput->GetStatus();
		if (existingStatus == EOutputStatus::SPENT || existingStatus == EOutputStatus::CANCELED)
		{
			for (OutputDataEntity& existing : walletOutputs)
			{
				if (existing.GetCommitment() == restoredOutput.GetCommitment())
				{
					WALLET_DEBUG_F("Resurrecting {} output found in the UTXO set", OutputStatus::ToString(existingStatus));
					if (restoredOutput.GetBlockHeight().has_value()) {
						existing.SetBlockHeight(restoredOutput.GetBlockHeight().value());
					}
					existing.SetStatus(EOutputStatus::SPENDABLE);
					pBatch->SaveOutput(masterSeed, existing);
					break;
				}
			}
		}
	}

	return new_outputs;
}

void WalletRefresher::RefreshOutputStatuses(
	const SecureVector& masterSeed,
	Writer<IWalletDB> pBatch,
	const std::vector<WalletTx>& walletTransactions,
	std::vector<OutputDataEntity>& walletOutputs,
	const uint64_t nodeHeight,
	const uint64_t refreshBlockHeight,
	std::optional<uint64_t>& rewindHeightOpt)
{
	std::unordered_map<uint32_t, WalletTx> walletTransactionsById;
	for (const WalletTx& walletTx : walletTransactions) {
		walletTransactionsById.insert({ walletTx.GetId(), walletTx });
	}

	// SPENT / CANCELED outputs are final for refresh purposes: no need to query
	// the node for them (and, for the remote backend, they would only consume the
	// node's per-request commitment budget).
	std::vector<Commitment> commitments;
	for (const OutputDataEntity& outputData : walletOutputs)
	{
		const EOutputStatus status = outputData.GetStatus();
		if (status == EOutputStatus::SPENT || status == EOutputStatus::CANCELED) {
			continue;
		}

		commitments.push_back(outputData.GetCommitment());
	}

	const std::map<Commitment, OutputLocation> outputLocations = m_pNodeClient->GetOutputsByCommitment(commitments);

	std::unordered_map<uint32_t, EKernelLookupStatus> kernelStatusByTx;
	for (OutputDataEntity& outputData : walletOutputs)
	{
		const EOutputStatus current_status = outputData.GetStatus();
		if (current_status == EOutputStatus::SPENT || current_status == EOutputStatus::CANCELED) {
			continue;
		}

		auto iter = outputLocations.find(outputData.GetCommitment());
		if (iter != outputLocations.cend()) {
			// Output is on chain, so unless it's being spent, then we can mark it as spendable.
			if (current_status == EOutputStatus::LOCKED) {
				continue;
			}

			// If spendable status, it's already up-to-date.
			if (current_status == EOutputStatus::SPENDABLE) {
				continue;
			}

			WALLET_DEBUG_F("Marking output as spendable: {}", outputData);
			outputData.SetBlockHeight(iter->second.GetBlockHeight());
			outputData.SetStatus(EOutputStatus::SPENDABLE);
			pBatch->SaveOutput(masterSeed, outputData);
			continue;
		}

		// The output is absent from the node's unspent set. Gather kernel evidence
		// where it is meaningful: only once the chain is above the output height.
		EKernelLookupStatus kernelStatus = EKernelLookupStatus::UNAVAILABLE;
		if (current_status != EOutputStatus::NO_CONFIRMATIONS &&
			outputData.GetBlockHeight().has_value() &&
			nodeHeight > outputData.GetBlockHeight().value())
		{
			kernelStatus = LookupOutputKernel(outputData, walletTransactionsById, kernelStatusByTx);
		}

		const EOutputStatus newStatus = DetermineMissingOutputStatus(
			current_status,
			outputData.GetBlockHeight(),
			nodeHeight,
			refreshBlockHeight,
			kernelStatus
		);

		if (newStatus == current_status) {
			continue;
		}

		if (newStatus == EOutputStatus::REVERTED)
		{
			// The confirming block may have been replaced at or after this height.
			// Remember the earliest height that must be rescanned.
			const uint64_t outputHeight = outputData.GetBlockHeight().value_or(0);
			const uint64_t rewindHeight = outputHeight > REORG_RESCAN_WINDOW
				? outputHeight - REORG_RESCAN_WINDOW
				: 0;
			if (!rewindHeightOpt.has_value() || rewindHeight < rewindHeightOpt.value()) {
				rewindHeightOpt = std::make_optional(rewindHeight);
			}
		}

		WALLET_DEBUG_F("Refreshing output status {} -> {}: {}", (int)current_status, (int)newStatus, outputData);
		outputData.SetStatus(newStatus);
		pBatch->SaveOutput(masterSeed, outputData);
	}
}

EKernelLookupStatus WalletRefresher::LookupOutputKernel(
	const OutputDataEntity& output,
	const std::unordered_map<uint32_t, WalletTx>& walletTransactions,
	std::unordered_map<uint32_t, EKernelLookupStatus>& kernelStatusByTx) const
{
	if (!output.GetWalletTxId().has_value()) {
		return EKernelLookupStatus::UNAVAILABLE;
	}

	const uint32_t walletTxId = output.GetWalletTxId().value();
	const auto cached = kernelStatusByTx.find(walletTxId);
	if (cached != kernelStatusByTx.cend()) {
		return cached->second;
	}

	EKernelLookupStatus status = EKernelLookupStatus::UNAVAILABLE;

	const auto txIter = walletTransactions.find(walletTxId);
	if (txIter != walletTransactions.cend())
	{
		const auto& transactionOpt = txIter->second.GetTransaction();
		if (transactionOpt.has_value())
		{
			bool sawKernel = false;
			bool found = false;
			bool unavailable = false;
			for (const TransactionKernel& kernel : transactionOpt.value().GetKernels())
			{
				sawKernel = true;
				KernelLookup lookup;
				try
				{
					lookup = m_pNodeClient->GetKernel(kernel.GetExcessCommitment());
				}
				catch (const NodeUnavailableException&)
				{
					lookup = KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
				}

				if (lookup.status == EKernelLookupStatus::FOUND) {
					found = true;
					break;
				}

				if (lookup.status == EKernelLookupStatus::UNAVAILABLE) {
					unavailable = true;
				}
			}

			if (found) {
				status = EKernelLookupStatus::FOUND;
			} else if (sawKernel && !unavailable) {
				status = EKernelLookupStatus::NOT_FOUND;
			}
		}
	}

	kernelStatusByTx.insert({ walletTxId, status });
	return status;
}

void WalletRefresher::RewindRestoreCursor(
	Writer<IWalletDB> pBatch,
	const uint64_t rewindHeight)
{
	const uint64_t currentCursor = pBatch->GetRestoreLeafIndex();

	std::optional<uint64_t> sizeAtHeight;
	try
	{
		sizeAtHeight = m_pNodeClient->GetOutputMMRSizeAtHeight(rewindHeight);
	}
	catch (const NodeException&)
	{
		sizeAtHeight = std::nullopt;
	}

	// If the node cannot resolve the height, fall back to a full rescan instead
	// of silently skipping replaced outputs.
	const uint64_t targetCursor = sizeAtHeight.value_or(0);

	// This helper only rewinds; the forward cursor is owned by the restorer.
	if (targetCursor >= currentCursor) {
		return;
	}

	WALLET_INFO_F(
		"Rewinding restore cursor from {} to {} (rescan from block height {})",
		currentCursor,
		targetCursor,
		rewindHeight
	);
	pBatch->UpdateRestoreLeafIndex(targetCursor);
}

void WalletRefresher::RefreshTransactions(
	const SecureVector& masterSeed,
	Writer<IWalletDB> pBatch,
	const std::vector<OutputDataEntity>& refreshedOutputs,
	std::vector<WalletTx>& walletTransactions)
{
	std::unordered_map<uint32_t, WalletTx> walletTransactionsById;
	for (WalletTx& walletTx : walletTransactions) {
		walletTransactionsById.insert({ walletTx.GetId(), walletTx });
	}

	for (const OutputDataEntity& output : refreshedOutputs) {
		if (output.GetWalletTxId().has_value()) {
			auto iter = walletTransactionsById.find(output.GetWalletTxId().value());
			if (iter != walletTransactionsById.cend()) {
				WalletTx walletTx = iter->second;

				// A reverted output must not confirm its transaction: the block
				// that contained it is no longer canonical.
				if (output.GetStatus() == EOutputStatus::REVERTED) {
					continue;
				}

				if (output.GetBlockHeight().has_value()) {
					walletTx.SetConfirmedHeight(output.GetBlockHeight().value());

					if (walletTx.GetType() == EWalletTxType::RECEIVING_IN_PROGRESS) {
						WALLET_DEBUG_F("Marking transaction as received: {}", walletTx.GetId());

						walletTx.SetType(EWalletTxType::RECEIVED);
						pBatch->AddTransaction(masterSeed, walletTx);
					} else if (walletTx.GetType() == EWalletTxType::SENDING_FINALIZED) {
						WALLET_DEBUG_F("Marking transaction as sent: {}", walletTx.GetId());

						walletTx.SetType(EWalletTxType::SENT);
						pBatch->AddTransaction(masterSeed, walletTx);
					}
				}
			}
		}
	}

	for (WalletTx& walletTx : walletTransactions) {
		const auto& tx = walletTx.GetTransaction();
		if (tx.has_value() && walletTx.GetType() == EWalletTxType::SENDING_FINALIZED) {
			for (const TransactionOutput& output : tx.value().GetOutputs()) {
				auto pOutputData = FindOutput(refreshedOutputs, output.GetCommitment());
				if (pOutputData != nullptr) {
					if (pOutputData->GetStatus() == EOutputStatus::SPENT) {
						WALLET_DEBUG_F("Output is spent. Marking transaction as sent: {}", walletTx.GetId());

						walletTx.SetType(EWalletTxType::SENT);
						pBatch->AddTransaction(masterSeed, walletTx);
					}

					break;
				}
			}
		}
	}
}

std::optional<std::chrono::system_clock::time_point> WalletRefresher::GetBlockTime(const OutputDataEntity& output) const
{
	if (output.GetBlockHeight().has_value()) {
		auto pHeader = m_pNodeClient->GetBlockHeader(output.GetBlockHeight().value());
		if (pHeader != nullptr) {
			return std::make_optional(TimeUtil::ToTimePoint(pHeader->GetTimestamp() * 1000));
		}
	}

	return std::nullopt;
}

std::unique_ptr<OutputDataEntity> WalletRefresher::FindOutput(
	const std::vector<OutputDataEntity>& walletOutputs,
	const Commitment& commitment) const
{
	for (const OutputDataEntity& outputData : walletOutputs) {
		if (commitment == outputData.GetCommitment()) {
			return std::make_unique<OutputDataEntity>(outputData);
		}
	}

	return std::unique_ptr<OutputDataEntity>(nullptr);
}
