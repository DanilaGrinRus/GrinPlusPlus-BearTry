#pragma once

#include <Core/Util/JsonUtil.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Clients/RPC/RPCException.h>
#include <Net/Tor/TorAddress.h>
#include <Net/Tor/TorConnection.h>
#include <Net/Tor/TorProcess.h>
#include <Wallet/Models/Slate/Slate.h>

class ReceiveTxClient
{
public:
	static Slate Call(const TorProcess::Ptr& pTorProcess, const TorAddress& torAddress, const Slate& slate)
	{
		Json::Value params;
		params.append(slate.ToJSON());
		params.append(Json::nullValue); // Account path not currently supported
		params.append(Json::nullValue);
		RPC::Request receiveTxRequest = RPC::Request::BuildRequest("receive_tx", params);
		// Never log the full slate (amounts, addresses, partial signatures,
		// payment proof). Only the id/stage is useful for diagnostics.
		LOG_INFO_F(
			"receive_tx REQUEST for slate {} (stage={}).",
			uuids::to_string(slate.GetId()),
			slate.stage.ToString()
		);

		TorConnectionPtr pTorConnection = pTorProcess->Connect(torAddress);
		RPC::Response receiveTxResponse = pTorConnection->Invoke(receiveTxRequest, "/v2/foreign");

		if (receiveTxResponse.GetError().has_value()) {
			RPC::Error rpc_error = receiveTxResponse.GetError().value();
			LOG_ERROR_F("receive_tx failed for slate {}: {}", uuids::to_string(slate.GetId()), rpc_error.GetMsg());
			throw RPC_EXCEPTION(rpc_error.GetMsg(), std::nullopt);
		}

		Json::Value okJson = JsonUtil::GetRequiredField(receiveTxResponse.GetResult().value(), "Ok");

		return Slate::FromJSON(okJson);
	}
};