#include "CancelTx.h"

#include <API/Wallet/Owner/Models/TxLifecycle.h>
#include <Common/Logger.h>
#include <Core/Exceptions/WalletException.h>

void CancelTx::CancelWalletTx(const SecureVector& masterSeed, const std::shared_ptr<IWalletDB>& pWalletDB, WalletTx& walletTx)
{
    const EWalletTxType type = walletTx.GetType();
    WALLET_DEBUG_F("Canceling WalletTx {} of type {}.", walletTx.GetId(), WalletTxType::ToString(type));

    // Never roll back an output set for a transaction that is already on chain:
    // otherwise change outputs are marked CANCELED and WalletRefresher never
    // revives them, making spendable coins disappear from the wallet view.
    // (Rust refuses to cancel confirmed transactions for the same reason.)
    bool confirmed = walletTx.GetConfirmationHeight().has_value() || walletTx.GetConfirmationTime().has_value();

    std::vector<OutputDataEntity> outputs = pWalletDB->GetOutputs(masterSeed);
    if (!confirmed)
    {
        for (const OutputDataEntity& output : outputs)
        {
            if (output.GetWalletTxId() == walletTx.GetId() && output.GetBlockHeight().has_value())
            {
                // The output was observed in a block by WalletRefresher, but the
                // wallet tx status may not have been persisted yet.
                confirmed = true;
                break;
            }
        }
    }

    if (confirmed)
    {
        WALLET_ERROR_F("Refusing to cancel WalletTx {}: it is already confirmed.", walletTx.GetId());
        throw TX_NOT_CANCELABLE_EXCEPTION("Transaction is already confirmed and cannot be canceled.");
    }

    if (!TxLifecycle::IsCancelable(type, false))
    {
        WALLET_ERROR("WalletTx was not in a cancelable status.");
        throw TX_NOT_CANCELABLE_EXCEPTION("WalletTx not in a cancelable status.");
    }

    if (type == EWalletTxType::RECEIVING_IN_PROGRESS) {
        walletTx.SetType(EWalletTxType::RECEIVED_CANCELED);
    } else {
        // SENDING_STARTED or SENDING_FINALIZED
        walletTx.SetType(EWalletTxType::SENT_CANCELED);
    }

    std::unordered_set<Commitment> inputCommitments;
    std::optional<Transaction> transactionOpt = walletTx.GetTransaction();
    if (walletTx.GetType() == EWalletTxType::SENT_CANCELED && transactionOpt.has_value()) {
        for (const TransactionInput& input : transactionOpt.value().GetInputs()) {
            inputCommitments.insert(input.GetCommitment());
        }
    }

    std::vector<OutputDataEntity> outputsToUpdate = GetOutputsToUpdate(outputs, inputCommitments, walletTx);

    pWalletDB->AddOutputs(masterSeed, outputsToUpdate);
    pWalletDB->AddTransaction(masterSeed, walletTx);
}

std::vector<OutputDataEntity> CancelTx::GetOutputsToUpdate(
    std::vector<OutputDataEntity>& outputs,
    const std::unordered_set<Commitment>& inputCommitments,
    const WalletTx& walletTx)
{
    std::vector<OutputDataEntity> outputsToUpdate;
    for (OutputDataEntity& output : outputs) {
        const auto iter = inputCommitments.find(output.GetOutput().GetCommitment());
        if (iter != inputCommitments.end()) {
            const EOutputStatus status = output.GetStatus();
            WALLET_DEBUG_F("Found input with status ({})", OutputStatus::ToString(status));

            if (status == EOutputStatus::NO_CONFIRMATIONS || status == EOutputStatus::SPENT) {
                output.SetStatus(EOutputStatus::CANCELED);
                outputsToUpdate.push_back(output);
            } else if (status == EOutputStatus::REVERTED) {
                // The input's confirming block is no longer canonical. Its live
                // transaction state is ambiguous, so fail closed instead of
                // resurrecting/canceling coins from a stale view.
                WALLET_ERROR_F("Can't cancel reverted input ({})", OutputStatus::ToString(status));
                throw TX_NOT_CANCELABLE_EXCEPTION("Input is reverted and cannot be canceled safely.");
            } else if (status == EOutputStatus::LOCKED) {
                output.SetStatus(EOutputStatus::SPENDABLE);
                outputsToUpdate.push_back(output);
            } else if (walletTx.GetType() != EWalletTxType::SENT_CANCELED) {
                WALLET_ERROR_F("Can't cancel output with status ({})", OutputStatus::ToString(status));
                throw WALLET_EXCEPTION("Output not in a cancelable status.");
            }
        } else if (output.GetWalletTxId() == walletTx.GetId()) {
            const EOutputStatus status = output.GetStatus();
            WALLET_DEBUG_F("Found output with status ({})", OutputStatus::ToString(status));

            if (status == EOutputStatus::NO_CONFIRMATIONS || status == EOutputStatus::SPENT) {
                output.SetStatus(EOutputStatus::CANCELED);
                outputsToUpdate.push_back(output);
            } else if (status == EOutputStatus::REVERTED) {
                WALLET_ERROR_F("Can't cancel reverted output ({})", OutputStatus::ToString(status));
                throw TX_NOT_CANCELABLE_EXCEPTION("Output is reverted and cannot be canceled safely.");
            } else if (status != EOutputStatus::CANCELED) {
                WALLET_ERROR_F("Can't cancel output with status ({})", OutputStatus::ToString(status));
                throw WALLET_EXCEPTION("Output not in a cancelable status.");
            }
        }
    }

    return outputsToUpdate;
}