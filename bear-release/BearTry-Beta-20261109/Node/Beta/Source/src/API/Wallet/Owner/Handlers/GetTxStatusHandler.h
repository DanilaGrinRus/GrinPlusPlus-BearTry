#pragma once

#include <Core/Util/JsonUtil.h>
#include <Wallet/WalletManager.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <API/Wallet/Owner/Models/Errors.h>
#include <API/Wallet/Owner/Models/TxLifecycle.h>
#include <limits>
#include <optional>

//
// Owner API: get_tx_status
//
// Params: {"session_token": "...", "tx_id": 123}
// Reply:  {"tx_id":123,"type":"SENDING_FINALIZED","lifecycle":"posted",
//          "broadcast":"posted","finalized":true,"confirmed":false,
//          "tx_slate_id":"...","confirmed_height":null}
//
// The lifecycle is derived from the persisted wallet tx status plus the
// in-process broadcast outcome. After a restart, a finalized-but-unconfirmed
// transaction is reported as "unknown" until it is confirmed or reposted.
//
class GetTxStatusHandler : public RPCMethod
{
public:
	GetTxStatusHandler(const IWalletManagerPtr& pWalletManager)
		: m_pWalletManager(pWalletManager) { }
	virtual ~GetTxStatusHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value())
		{
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isObject())
		{
			return request.BuildError(RPC::ErrorCode::INVALID_PARAMS, "Expected object params");
		}

		const SessionToken token = SessionToken::FromBase64(
			JsonUtil::GetRequiredString(params, "session_token")
		);

		const uint64_t txIdRaw = JsonUtil::ConvertToUInt64(JsonUtil::GetRequiredField(params, "tx_id"));
		if (txIdRaw > (uint64_t)(std::numeric_limits<uint32_t>::max)())
		{
			return request.BuildError(RPC::ErrorCode::INVALID_PARAMS, "tx_id is out of range");
		}
		const uint32_t txId = (uint32_t)txIdRaw;

		const auto wallet = m_pWalletManager->GetWallet(token);
		const std::unique_ptr<WalletTx> pWalletTx = wallet.Read()->GetTransactionById(txId);
		if (pWalletTx == nullptr)
		{
			return request.BuildError(RPC::Errors::TX_NOT_FOUND);
		}

		const bool confirmed =
			pWalletTx->GetConfirmationHeight().has_value()
			|| pWalletTx->GetConfirmationTime().has_value();

		std::string slateId;
		std::optional<EBroadcastResult> broadcastOpt;
		std::string broadcastError;
		if (pWalletTx->GetSlateId().has_value())
		{
			slateId = uuids::to_string(pWalletTx->GetSlateId().value());
			const std::optional<TxBroadcastRegistry::Entry> entry = TxBroadcastRegistry::Find(slateId);
			if (entry.has_value())
			{
				broadcastOpt = entry.value().result;
				broadcastError = entry.value().error;
			}
		}

		const ETxLifecycleState lifecycle = TxLifecycle::FromWalletTx(
			pWalletTx->GetType(),
			confirmed,
			broadcastOpt
		);

		Json::Value result;
		result["tx_id"] = (Json::UInt64)txId;
		result["type"] = WalletTxType::ToKey(pWalletTx->GetType());
		result["lifecycle"] = TxLifecycle::ToString(lifecycle);
		result["broadcast"] = broadcastOpt.has_value()
			? Json::Value(TxLifecycle::ToString(broadcastOpt.value()))
			: Json::Value("unknown");
		result["finalized"] =
			pWalletTx->GetType() == EWalletTxType::SENDING_FINALIZED
			|| pWalletTx->GetType() == EWalletTxType::SENT
			|| pWalletTx->GetType() == EWalletTxType::SENT_CANCELED;
		result["confirmed"] = confirmed;

		if (!slateId.empty())
		{
			result["tx_slate_id"] = slateId;
		}

		if (pWalletTx->GetConfirmationHeight().has_value())
		{
			result["confirmed_height"] = (Json::UInt64)pWalletTx->GetConfirmationHeight().value();
		}
		else
		{
			result["confirmed_height"] = Json::nullValue;
		}

		if (!broadcastError.empty())
		{
			result["broadcast_error"] = broadcastError;
		}

		return request.BuildResult(result);
	}

	// Requests carry the session token and the reply may reveal tx metadata.
	bool ContainsSecrets() const noexcept final { return true; }

private:
	IWalletManagerPtr m_pWalletManager;
};
