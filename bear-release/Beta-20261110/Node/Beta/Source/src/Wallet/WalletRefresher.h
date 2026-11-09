#pragma once

#include <Core/Config.h>
#include <Wallet/WalletTx.h>
#include <Wallet/NodeClient.h>
#include <Wallet/WalletDB/WalletDB.h>
#include <Wallet/WalletDB/Models/OutputDataEntity.h>
#include <Common/Secure.h>
#include <Crypto/Models/SecretKey.h>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward Declarations
class INodeClient;
class Wallet;

class WalletRefresher
{
public:
	//
	// Number of blocks to rescan (in addition to the current tip window) after a
	// reorg is detected. Mirrors the reference wallet's REORG_RESCAN_WINDOW
	// (24 * 60 * 2 blocks, ~2 days at one-minute blocks).
	//
	static constexpr uint64_t REORG_RESCAN_WINDOW = 2880;

	WalletRefresher(const Config& config, const INodeClientConstPtr& pNodeClient)
		: m_config(config), m_pNodeClient(pNodeClient) { }

	std::vector<OutputDataEntity> Refresh(
		const SecureVector& masterSeed,
		Locked<IWalletDB> walletDB,
		const bool fromGenesis
	);

	//
	// Pure decision helper for an output that is absent from the node's unspent
	// set. Public so regression tests can pin the W-09 status policy:
	//   * never SPENT when the output cannot have been spent yet;
	//   * kernel evidence distinguishes "spent" (kernel on chain) from "reverted"
	//     (kernel gone);
	//   * without kernel data, SPENT is only chosen when the chain demonstrably
	//     advanced since the last successful refresh; a same-height / partial
	//     response yields REVERTED instead of a sticky SPENT.
	// Returns the current status when no safe update can be made.
	//
	static EOutputStatus DetermineMissingOutputStatus(
		const EOutputStatus currentStatus,
		const std::optional<uint64_t>& outputBlockHeight,
		const uint64_t nodeHeight,
		const uint64_t refreshBlockHeight,
		const EKernelLookupStatus kernelStatus
	);

private:
	std::optional<NodeTip> TryGetTip() const;

	std::vector<OutputDataEntity> FindNewOutputs(
		const SecureVector& masterSeed,
		Writer<IWalletDB> pBatch,
		std::vector<OutputDataEntity>& walletOutputs,
		const bool fromGenesis
	);

	void RefreshOutputStatuses(
		const SecureVector& masterSeed,
		Writer<IWalletDB> pBatch,
		const std::vector<WalletTx>& walletTransactions,
		std::vector<OutputDataEntity>& walletOutputs,
		const uint64_t nodeHeight,
		const uint64_t refreshBlockHeight,
		std::optional<uint64_t>& rewindHeightOpt
	);

	EKernelLookupStatus LookupOutputKernel(
		const OutputDataEntity& output,
		const std::unordered_map<uint32_t, WalletTx>& walletTransactions,
		std::unordered_map<uint32_t, EKernelLookupStatus>& kernelStatusByTx
	) const;

	void RewindRestoreCursor(
		Writer<IWalletDB> pBatch,
		const uint64_t rewindHeight
	);

	void RefreshTransactions(
		const SecureVector& masterSeed,
		Writer<IWalletDB> pBatch,
		const std::vector<OutputDataEntity>& walletOutputs,
		std::vector<WalletTx>& walletTransactions
	);

	std::optional<std::chrono::system_clock::time_point> GetBlockTime(const OutputDataEntity& output) const;

	std::unique_ptr<OutputDataEntity> FindOutput(
		const std::vector<OutputDataEntity>& walletOutputs,
		const Commitment& commitment
	) const;

	const Config& m_config;
	INodeClientConstPtr m_pNodeClient;
};
