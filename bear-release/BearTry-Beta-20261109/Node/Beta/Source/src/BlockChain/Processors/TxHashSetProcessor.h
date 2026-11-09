#pragma once

#include "../ChainState.h"

#include <PMMR/TxHashSet.h>
#include <Core/Config.h>
#include <Crypto/Models/Hash.h>
#include <P2P/SyncStatus.h>
#include <filesystem.h>
#include <string>

// Forward Declarations
class IBlockChain;
class BlockHeader;

class TxHashSetProcessor
{
public:
	// Why a txhashset was refused before its batch was committed. Used by callers
	// to avoid banning a peer for purely local incompleteness.
	enum class EFailureKind
	{
		None = 0,
		// The state could not be verified locally (missing header/kernel data or
		// an internally inconsistent kernel count). No evidence against the peer.
		LocalUnverifiable,
		// All prerequisites were available and the NRD history still violates the
		// relative-height rules. Rust grin 5.5.2 rejects such a txhashset write.
		InvalidState
	};

	TxHashSetProcessor(const Config& config, IBlockChain& blockChain, std::shared_ptr<Locked<ChainState>> pChainState);

	bool ProcessTxHashSet(const Hash& blockHash, const fs::path& path, SyncStatus& syncStatus);

	// Validates and installs a txhashset that is already on disk at the configured
	// path (PIBD writes and activates the files before calling this).
	bool ProcessExistingTxHashSet(const Hash& blockHash, SyncStatus& syncStatus);

	EFailureKind GetLastFailureKind() const noexcept { return m_lastFailureKind; }

	// Rebuild the in-memory NRD recent-kernel index and classify the result.
	// Must run before the ChainState batch that installs the txhashset is
	// committed. Returns Ok only when the index was rebuilt completely.
	static EFailureKind VerifyNrdIndexBeforeCommit(
		ITxHashSet& txHashSet,
		const std::shared_ptr<const IBlockDB>& pBlockDB,
		const BlockHeader& header);

private:
	bool UpdateConfirmedChain(Writer<ChainState> pLockedState, const BlockHeader& blockHeader);

	const Config& m_config;
	IBlockChain& m_blockChain;
	std::shared_ptr<Locked<ChainState>> m_pChainState;
	EFailureKind m_lastFailureKind = EFailureKind::None;
};