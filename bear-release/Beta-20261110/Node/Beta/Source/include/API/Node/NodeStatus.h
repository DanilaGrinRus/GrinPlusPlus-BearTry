#pragma once

#include <BlockChain/BlockChain.h>
#include <Core/Enums/Environment.h>
#include <Core/Global.h>
#include <P2P/Common.h>
#include <P2P/P2PServer.h>
#include <json/json.h>
#include <string>

// Builds the Grin v5.5.2 `api::types::Status` JSON shape from the live C++
// chain/P2P state. This is intentionally the only builder for that schema so
// GET /v1/status (compatibility) and owner get_status cannot drift.
namespace NodeStatus
{
	inline std::string ChainShortName(const Environment environment)
	{
		switch (environment) {
			case Environment::MAINNET: return "main";
			case Environment::FLOONET: return "floonet";
			case Environment::AUTOMATED_TESTING: return "test";
		}

		return "unknown";
	}

	inline std::string SyncStatusName(const ESyncStatus status)
	{
		switch (status) {
			case ESyncStatus::SYNCING_HEADERS: return "header_sync";
			case ESyncStatus::SYNCING_TXHASHSET: return "txhashset_download";
			case ESyncStatus::PROCESSING_TXHASHSET: return "txhashset_rangeproofs_validation";
			case ESyncStatus::TXHASHSET_SYNC_FAILED: return "txhashset_sync_failed";
			case ESyncStatus::SYNCING_BLOCKS: return "body_sync";
			case ESyncStatus::WAITING_FOR_PEERS: return "awaiting_peers";
			case ESyncStatus::NOT_SYNCING: return "no_sync";
		}

		return "no_sync";
	}

	// Returns a Grin-compatible Status object, or null if the chain tip is
	// unavailable. Every field comes from the live chain/P2P snapshot; there are
	// no canned or test-only values.
	inline Json::Value Build(
		const IBlockChain::Ptr& pBlockChain,
		const IP2PServerPtr& pP2PServer,
		const BlockHeaderPtr& pSuppliedTip = BlockHeaderPtr())
	{
		// A REST caller that already read the tip must use the same immutable
		// header for both legacy chain fields and the nested Rust representation.
		const BlockHeaderPtr pTip = pSuppliedTip != nullptr ? pSuppliedTip :
			pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTip == nullptr) {
			return Json::Value(Json::nullValue);
		}

		const SyncStatusConstPtr pSyncStatus = pP2PServer->GetSyncStatus();
		const std::pair<size_t, size_t> numConnections = pP2PServer->GetNumberOfConnectedPeers();
		const ESyncStatus syncStatus = pSyncStatus->GetStatus();

		Json::Value tip;
		tip["height"] = Json::UInt64(pTip->GetHeight());
		tip["last_block_pushed"] = pTip->GetHash().ToHex();
		tip["prev_block_to_last"] = pTip->GetPreviousHash().ToHex();
		tip["total_difficulty"] = Json::UInt64(pTip->GetTotalDifficulty());

		Json::Value status;
		status["chain"] = ChainShortName(Global::GetEnv());
		status["protocol_version"] = Json::UInt(P2P::PROTOCOL_VERSION);
		status["user_agent"] = P2P::USER_AGENT;
		status["connections"] = Json::UInt(static_cast<unsigned int>(
			numConnections.first + numConnections.second));
		status["tip"] = tip;
		status["sync_status"] = SyncStatusName(syncStatus);

		Json::Value syncInfo;
		bool hasSyncInfo = false;
		switch (syncStatus)
		{
			case ESyncStatus::SYNCING_HEADERS:
				syncInfo["current_height"] = Json::UInt64(pSyncStatus->GetHeaderHeight());
				syncInfo["highest_height"] = Json::UInt64(pSyncStatus->GetNetworkHeight());
				hasSyncInfo = true;
				break;
			case ESyncStatus::SYNCING_TXHASHSET:
				syncInfo["downloaded_size"] = Json::UInt64(pSyncStatus->GetDownloaded());
				syncInfo["total_size"] = Json::UInt64(pSyncStatus->GetDownloadSize());
				hasSyncInfo = true;
				break;
			case ESyncStatus::PROCESSING_TXHASHSET:
				// C++ exposes one coarse processing percentage rather than the
				// separate Rust setup/range-proof/kernel phases. Keep the Rust
				// status name and report the actual local progress field.
				syncInfo["processing_status"] =
					Json::UInt(static_cast<unsigned int>(pSyncStatus->GetProcessingStatus()));
				hasSyncInfo = true;
				break;
			case ESyncStatus::SYNCING_BLOCKS:
				syncInfo["current_height"] = Json::UInt64(pSyncStatus->GetBlockHeight());
				syncInfo["highest_height"] = Json::UInt64(pSyncStatus->GetNetworkHeight());
				hasSyncInfo = true;
				break;
			case ESyncStatus::TXHASHSET_SYNC_FAILED:
				syncInfo["failed"] = true;
				hasSyncInfo = true;
				break;
			default:
				break;
		}

		if (hasSyncInfo) {
			status["sync_info"] = syncInfo;
		}

		return status;
	}
}
