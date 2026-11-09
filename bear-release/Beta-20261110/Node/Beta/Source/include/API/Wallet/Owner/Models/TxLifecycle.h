#pragma once

#include <Core/Exceptions/WalletException.h>
#include <Wallet/WalletTxType.h>

#include <json/json.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

//
// Additive transaction lifecycle model for the owner API.
//
// The on-disk wallet status (EWalletTxType) is intentionally NOT changed here:
// WalletDB, WalletRefresher and the transaction state machine are owned by
// other workstreams. Instead the lifecycle is derived from the persisted
// EWalletTxType plus the in-process broadcast outcome recorded by
// WalletManager::Finalize/RepostTx.
//
// UI compatibility: the existing "status" strings ("SENT"/"FINALIZED") are
// unchanged. The lifecycle is exposed as extra JSON fields only.
//
enum class ETxLifecycleState
{
	UNKNOWN = 0,
	NOT_FINALIZED,
	FINALIZED,
	POSTED,
	FINALIZED_NOT_BROADCAST,
	ACCEPTED,
	CONFIRMED
};

// Outcome of the last broadcast attempt for one slate. NOT_REQUESTED means the
// caller did not ask for a broadcast (offline slatepack flow).
enum class EBroadcastResult
{
	UNKNOWN = 0,
	NOT_REQUESTED,
	POSTED,
	ACCEPTED,
	FAILED
};

namespace TxLifecycle
{
	inline const char* ToString(const ETxLifecycleState state) noexcept
	{
		switch (state)
		{
			case ETxLifecycleState::NOT_FINALIZED: return "not_finalized";
			case ETxLifecycleState::FINALIZED: return "finalized";
			case ETxLifecycleState::POSTED: return "posted";
			case ETxLifecycleState::FINALIZED_NOT_BROADCAST: return "finalized_not_broadcast";
			case ETxLifecycleState::ACCEPTED: return "accepted";
			case ETxLifecycleState::CONFIRMED: return "confirmed";
			case ETxLifecycleState::UNKNOWN:
			default: return "unknown";
		}
	}

	inline const char* ToString(const EBroadcastResult result) noexcept
	{
		switch (result)
		{
			case EBroadcastResult::NOT_REQUESTED: return "not_requested";
			case EBroadcastResult::POSTED: return "posted";
			case EBroadcastResult::ACCEPTED: return "accepted";
			case EBroadcastResult::FAILED: return "failed";
			case EBroadcastResult::UNKNOWN:
			default: return "unknown";
		}
	}

	inline Json::Value ToJSON(const ETxLifecycleState state)
	{
		return Json::Value(ToString(state));
	}

	//
	// Derives the lifecycle from the persisted wallet tx status and the
	// broadcast outcome recorded for this process (if any). After a restart the
	// registry is empty and a finalized-but-unconfirmed transaction is reported
	// as "unknown" - which is exactly the crash-between-commit-and-post state.
	//
	inline ETxLifecycleState FromWalletTx(
		const EWalletTxType type,
		const bool confirmed,
		const std::optional<EBroadcastResult>& broadcastOpt) noexcept
	{
		if (confirmed)
		{
			return ETxLifecycleState::CONFIRMED;
		}

		switch (type)
		{
			case EWalletTxType::SENDING_STARTED:
			{
				return ETxLifecycleState::NOT_FINALIZED;
			}
			case EWalletTxType::SENDING_FINALIZED:
			{
				if (!broadcastOpt.has_value())
				{
					return ETxLifecycleState::UNKNOWN;
				}

				switch (broadcastOpt.value())
				{
					case EBroadcastResult::POSTED: return ETxLifecycleState::POSTED;
					case EBroadcastResult::ACCEPTED: return ETxLifecycleState::ACCEPTED;
					case EBroadcastResult::FAILED: return ETxLifecycleState::FINALIZED_NOT_BROADCAST;
					case EBroadcastResult::NOT_REQUESTED: return ETxLifecycleState::FINALIZED;
					case EBroadcastResult::UNKNOWN:
					default: return ETxLifecycleState::UNKNOWN;
				}
			}
			case EWalletTxType::SENT:
			case EWalletTxType::RECEIVED:
			case EWalletTxType::COINBASE:
			{
				// These statuses are only reached after the transaction has been
				// observed on chain by WalletRefresher.
				return ETxLifecycleState::CONFIRMED;
			}
			case EWalletTxType::RECEIVING_IN_PROGRESS:
			{
				return ETxLifecycleState::NOT_FINALIZED;
			}
			case EWalletTxType::SENT_CANCELED:
			case EWalletTxType::RECEIVED_CANCELED:
			default:
			{
				// Canceled transactions have no reliable on-chain state anymore.
				return ETxLifecycleState::UNKNOWN;
			}
		}
	}

	//
	// A transaction may only be canceled while it is not confirmed.
	// Rust refuses cancellation of confirmed transactions; we mirror that.
	//
	inline bool IsCancelable(const EWalletTxType type, const bool confirmed) noexcept
	{
		if (confirmed)
		{
			return false;
		}

		return type == EWalletTxType::SENDING_STARTED
			|| type == EWalletTxType::SENDING_FINALIZED
			|| type == EWalletTxType::RECEIVING_IN_PROGRESS;
	}
}

//
// A transaction was finalized (and persisted) but the network/pool rejected the
// broadcast. The state is safe - repost_tx can retry - but the caller must not
// report "FINALIZED" as if the transaction had been published.
//
class TxBroadcastException : public WalletException
{
public:
	TxBroadcastException(
		const std::string& message,
		const std::string& function,
		const std::string& slateId,
		const std::string& broadcastError)
		: WalletException(message, function),
		m_slateId(slateId),
		m_broadcastError(broadcastError)
	{
	}

	const std::string& GetSlateId() const noexcept { return m_slateId; }
	const std::string& GetBroadcastError() const noexcept { return m_broadcastError; }

private:
	std::string m_slateId;
	std::string m_broadcastError;
};

class TxNotCancelableException : public WalletException
{
public:
	TxNotCancelableException(const std::string& message, const std::string& function)
		: WalletException(message, function)
	{
	}
};

#define TX_BROADCAST_EXCEPTION(msg, slateId, broadcastError) TxBroadcastException(msg, __func__, slateId, broadcastError)
#define TX_NOT_CANCELABLE_EXCEPTION(msg) TxNotCancelableException(msg, __func__)

//
// Small process-local registry of broadcast outcomes, keyed by slate UUID.
// Slate ids are globally unique, so entries cannot collide across wallets. The
// registry is intentionally not persisted: after a restart the outcome is
// unknown (see TxLifecycle::FromWalletTx).
//
namespace TxBroadcastRegistry
{
	struct Entry
	{
		EBroadcastResult result;
		std::string error;
	};

	inline std::mutex& Mutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	inline std::map<std::string, Entry>& Entries()
	{
		static std::map<std::string, Entry> entries;
		return entries;
	}

	inline void Record(const std::string& slateId, const EBroadcastResult result, const std::string& error = "")
	{
		if (slateId.empty())
		{
			return;
		}

		std::lock_guard<std::mutex> lock(Mutex());
		auto& entries = Entries();
		entries[slateId] = Entry{ result, error };

		// Bound memory for long-running processes. UUIDs are unique, so dropping
		// the oldest entries can only turn a known state back into "unknown".
		while (entries.size() > 4096)
		{
			entries.erase(entries.begin());
		}
	}

	inline std::optional<Entry> Find(const std::string& slateId)
	{
		if (slateId.empty())
		{
			return std::nullopt;
		}

		std::lock_guard<std::mutex> lock(Mutex());
		auto& entries = Entries();
		const auto iter = entries.find(slateId);
		if (iter == entries.end())
		{
			return std::nullopt;
		}

		return iter->second;
	}

	inline void Clear()
	{
		std::lock_guard<std::mutex> lock(Mutex());
		Entries().clear();
	}
}
