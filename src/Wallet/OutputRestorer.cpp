#include "OutputRestorer.h"

#include <Common/Math.h>
#include <Consensus.h>
#include <Wallet/Keychain/KeyChain.h>
#include <Wallet/WalletUtil.h>
#include <Common/Logger.h>

#include <limits>

// Number of unspent outputs requested per page. The node may scan more PMMR
// positions than this (pruned/spent leaves are skipped), but it returns at most
// this many outputs per call.
static const uint64_t NUM_OUTPUTS_PER_BATCH = 1000;

// Hard bound on the number of pages processed in one restore pass. A conforming
// node always makes progress; this protects the wallet from a broken node or a
// corrupt cursor.
static const uint64_t MAX_OUTPUT_BATCHES = 100000;

std::vector<OutputDataEntity> OutputRestorer::FindAndRewindOutputs(const std::shared_ptr<IWalletDB>& pBatch, const bool fromGenesis) const
{
    // Restore cursor units: 0-based output-PMMR node position of the next node to
    // scan. This is the same unit the node get_unspent_outputs API uses and is
    // shared by the local and remote backends.
    //
    // Historical note: before the units were unified, the local backend stored a
    // 0-based leaf ordinal here. An old local cursor is always <= the matching
    // PMMR position, so interpreting it as a position only rescans an earlier
    // part of the MMR; duplicates are discarded by commitment further down. An
    // old remote cursor already used PMMR positions and remains valid.
    uint64_t nextPosition = fromGenesis ? 0 : pBatch->GetRestoreLeafIndex();
    uint64_t nextIndex = nextPosition == 0 ? 0 : nextPosition + 1;

    uint64_t highestIndex = 0;
    bool highestKnown = false;
    uint64_t lastRetrieved = nextPosition;

    std::vector<OutputDataEntity> walletOutputs;
    for (uint64_t batch = 0; batch < MAX_OUTPUT_BATCHES; ++batch)
    {
        std::unique_ptr<OutputRange> pOutputRange = m_pNodeClient->GetOutputsByPosition(nextIndex, NUM_OUTPUTS_PER_BATCH);
        if (pOutputRange == nullptr)
        {
            // "Not ready" is not "no outputs": do not move the cursor and do not
            // let the caller interpret an empty result as a completed scan.
            throw NodeUnavailableException("Node returned no output range while scanning outputs.");
        }

        const uint64_t pageLast = pOutputRange->GetLastRetrievedIndex();

        // Never lose outputs collected on earlier pages, even if this page turns
        // out to be empty (a page that starts with pruned leaves, or the final
        // page after the UTXO set was fully scanned).
        for (const OutputDTO& output : pOutputRange->GetOutputs())
        {
            std::unique_ptr<OutputDataEntity> pOutputDataEntity = GetWalletOutput(output);
            if (pOutputDataEntity != nullptr)
            {
                walletOutputs.emplace_back(*pOutputDataEntity);
            }
        }

        if (!highestKnown)
        {
            // Cache the first page's highest index: the chain may keep growing
            // during restore, and chasing it would tie up this thread indefinitely.
            highestIndex = pOutputRange->GetHighestIndex();
            highestKnown = true;
        }

        if (pageLast == (std::numeric_limits<uint64_t>::max)())
        {
            throw NodeException("Output scan index overflow.");
        }

        const uint64_t previousIndex = nextIndex;
        lastRetrieved = pageLast;
        nextIndex = pageLast + 1;

        if (nextIndex > highestIndex)
        {
            // The end of the scan window advertised by the first page was reached.
            break;
        }

        if (nextIndex <= previousIndex)
        {
            // A conforming node always advances the cursor (the start position is
            // at most pageLast + 1). Stop instead of looping forever.
            throw NodeException("Output scan did not make progress.");
        }
    }

    pBatch->UpdateRestoreLeafIndex(lastRetrieved);

    return walletOutputs;
}

std::unique_ptr<OutputDataEntity> OutputRestorer::GetWalletOutput(const OutputDTO& output) const
{
    EBulletproofType type = EBulletproofType::ORIGINAL;

    std::unique_ptr<RewoundProof> pRewoundProof = nullptr;
    if (Consensus::GetHeaderVersion(saturated_sub(output.GetBlockHeight(), Consensus::WEEKS(2))) == 1) {
        pRewoundProof = m_keyChain.RewindRangeProof(output.GetCommitment(), output.GetRangeProof(), EBulletproofType::ORIGINAL);
    }

    if (pRewoundProof == nullptr) {
        type = EBulletproofType::ENHANCED;
        pRewoundProof = m_keyChain.RewindRangeProof(output.GetCommitment(), output.GetRangeProof(), EBulletproofType::ENHANCED);
    }

    if (pRewoundProof) {
        return std::make_unique<OutputDataEntity>(BuildWalletOutput(output, *pRewoundProof, type));
    }

    return nullptr;
}

OutputDataEntity OutputRestorer::BuildWalletOutput(
    const OutputDTO& output,
    const RewoundProof& rewoundProof,
    EBulletproofType type) const
{
    WALLET_INFO_F(
        "Found own output: {} with value {}",
        output.GetCommitment(),
        rewoundProof.GetAmount()
    );

    KeyChainPath keyChainPath(rewoundProof.ToKeyIndices(type));

    SecretKey secret_key;
    if (rewoundProof.GetBlindingFactor() != nullptr && type == EBulletproofType::ORIGINAL) {
        secret_key = *rewoundProof.GetBlindingFactor();
    } else {
        secret_key = m_keyChain.DerivePrivateKey(keyChainPath, rewoundProof.GetAmount());
    }

    return OutputDataEntity{
        KeyChainPath{ rewoundProof.ToKeyIndices(type) },
        std::move(secret_key),
        output.ToTxOutput(),
        rewoundProof.GetAmount(),
        output.IsSpent() ? EOutputStatus::SPENT : EOutputStatus::SPENDABLE,
        output.GetMMRPosition(),
        output.GetBlockHeight(),
        std::nullopt,
        std::vector<std::string>{}
    };
}
