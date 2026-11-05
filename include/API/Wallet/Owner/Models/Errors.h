#pragma once

#include <Net/Clients/RPC/RPC.h>

namespace RPC
{
	class Errors
	{
	private:
		static Json::Value GetJSON(const std::string& type)
		{
			Json::Value errorData;
			errorData["type"] = type;
			return errorData;
		}

	public:
		static inline RPC::Error RECEIVER_UNREACHABLE = RPC::Error(-101, "Failed to establish TOR connection", GetJSON("RECEIVER_UNREACHABLE"));
		static inline RPC::Error SLATE_VERSION_MISMATCH = RPC::Error(-102, "Unsupported slate version", GetJSON("SLATE_VERSION_MISMATCH"));

		static inline RPC::Error PARAMS_MISSING = RPC::Error(-150, "'params' missing", GetJSON("PARAMS_MISSING"));

		// send/finalize (-160 -> -169)
		static inline RPC::Error INVALID_CHANGE_OUTPUTS = RPC::Error(-160, "Invalid change_outputs (expected 1..255; 0 only when sending the entire balance)", GetJSON("INVALID_CHANGE_OUTPUTS"));
		static inline RPC::Error INVALID_AMOUNT = RPC::Error(-161, "Invalid amount", GetJSON("INVALID_AMOUNT"));
		static inline RPC::Error TX_BROADCAST_FAILED = RPC::Error(-162, "Transaction finalized but could not be broadcast; use repost_tx", GetJSON("TX_BROADCAST_FAILED"));

		// receive/finalize slate validation (-170 -> -179)
		static inline RPC::Error SLATE_STATE_INVALID = RPC::Error(-170, "Slate is not in a receivable state", GetJSON("SLATE_STATE_INVALID"));
		static inline RPC::Error SLATE_TTL_EXPIRED = RPC::Error(-171, "Slate TTL expired", GetJSON("SLATE_TTL_EXPIRED"));
		static inline RPC::Error SLATE_AMOUNT_INVALID = RPC::Error(-172, "Slate amount is invalid", GetJSON("SLATE_AMOUNT_INVALID"));


		// create_wallet (-200 -> -209)
		static inline RPC::Error USER_ALREADY_EXISTS = RPC::Error(-200, "User already exists", GetJSON("USER_ALREADY_EXISTS"));
		static inline RPC::Error PASSWORD_CRITERIA_NOT_MET = RPC::Error(-201, "Password doesn't mean requirements", GetJSON("PASSWORD_CRITERIA_NOT_MET"));
		static inline RPC::Error INVALID_NUM_SEED_WORDS = RPC::Error(-202, "Invalid number of seed words", GetJSON("INVALID_NUM_SEED_WORDS"));

		// login (-210 -> -219)
		static inline RPC::Error USER_DOESNT_EXIST = RPC::Error(-210, "User doesn't exist", GetJSON("USER_DOESNT_EXIST"));
		static inline RPC::Error INVALID_PASSWORD = RPC::Error(-211, "Invalid Password", GetJSON("INVALID_PASSWORD"));

		// list_txs (-240 -> -249)
		static inline RPC::Error INVALID_STATUSES = RPC::Error(-240, "Invalid statuses", GetJSON("INVALID_STATUSES"));

		// cancel_tx (-250 -> -259)
		static inline RPC::Error TX_ID_MISSING = RPC::Error(-250, "'tx_id' missing", GetJSON("TX_ID_MISSING"));
		static inline RPC::Error TX_NOT_CANCELABLE = RPC::Error(-251, "Transaction cannot be canceled (missing or already confirmed)", GetJSON("TX_NOT_CANCELABLE"));

		// get_tx_status (-260 -> -269)
		static inline RPC::Error TX_NOT_FOUND = RPC::Error(-260, "Transaction not found", GetJSON("TX_NOT_FOUND"));
	};
}