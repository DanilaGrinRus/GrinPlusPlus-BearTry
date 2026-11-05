#pragma once

#include <Core/Util/JsonUtil.h>
#include <Net/Clients/RPC/RPC.h>

#include <string>

//
// Parses the JSON-RPC response of a transaction submission endpoint.
//
// grin-wallet / easy-jsonrpc-mw nodes wrap the method result as
//   {"result": {"Ok": ...}}  on success
//   {"result": {"Err": ...}} on a pool/validation rejection (HTTP 200)
// while the Grin++ node uses a JSON-RPC "error" object on rejection.
//
// Treating "a result is present" as success (the previous behaviour) reports a
// rejected transaction as published. This parser fails closed: only an explicit
// Ok counts as POSTED; Err, a JSON-RPC error, a missing result and an unknown
// result shape are all rejections.
//
enum class EPostTransactionStatus
{
	POSTED = 0,
	REJECTED,
	UNKNOWN
};

struct PostTransactionOutcome
{
	EPostTransactionStatus status;
	std::string error;
};

inline PostTransactionOutcome ParsePostTransactionResponse(const RPC::Response& response)
{
	if (response.GetError().has_value())
	{
		const RPC::Error& error = response.GetError().value();
		return PostTransactionOutcome{ EPostTransactionStatus::REJECTED, error.GetMsg() };
	}

	if (!response.GetResult().has_value())
	{
		return PostTransactionOutcome{ EPostTransactionStatus::REJECTED, "RPC response does not contain a result" };
	}

	const Json::Value& result = response.GetResult().value();
	if (result.isObject())
	{
		if (result.isMember("Err"))
		{
			const Json::Value& err = result["Err"];
			return PostTransactionOutcome{
				EPostTransactionStatus::REJECTED,
				err.isString() ? err.asString() : JsonUtil::WriteCondensed(err)
			};
		}

		if (result.isMember("Ok"))
		{
			return PostTransactionOutcome{ EPostTransactionStatus::POSTED, "" };
		}
	}

	return PostTransactionOutcome{
		EPostTransactionStatus::UNKNOWN,
		"Unrecognised submit response (missing Ok/Err): " + JsonUtil::WriteCondensed(result)
	};
}
