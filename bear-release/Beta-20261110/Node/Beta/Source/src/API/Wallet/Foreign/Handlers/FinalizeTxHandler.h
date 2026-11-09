#pragma once

#include <Wallet/WalletManager.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <API/Wallet/Foreign/Models/FinalizeTxRequest.h>
#include <API/Wallet/Foreign/Models/FinalizeTxResponse.h>
#include <API/Wallet/Owner/Models/TxLifecycle.h>
#include <optional>

class FinalizeTxHandler : public RPCMethod
{
public:
	FinalizeTxHandler(IWalletManager& walletManager, const SessionToken& token, const TorProcess::Ptr& pTorProcess)
		: m_walletManager(walletManager), m_token(token), m_pTorProcess(pTorProcess) { }
	virtual ~FinalizeTxHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		const std::optional<Json::Value>& paramsOpt = request.GetParams();
		try
		{
			FinalizeTxRequest finalizeTxRequest = FinalizeTxRequest::FromJSON(
				paramsOpt.value_or(Json::Value(Json::nullValue))
			);

			FinalizeCriteria criteria(
				SessionToken(m_token),
				std::nullopt,
				Slate(finalizeTxRequest.GetSlate()),
				std::make_optional(PostMethodDTO(EPostMethod::STEM, std::nullopt))
			);

			Slate finalized_slate = m_walletManager.Finalize(criteria, m_pTorProcess);

			return request.BuildResult(FinalizeTxResponse(std::move(finalized_slate)));
		}
		catch (const DeserializationException& e)
		{
			return request.BuildError(
				RPC::ErrorCode::INVALID_PARAMS,
				StringUtil::Format("Failed to deserialize slate: {}", e.what())
			);
		}
		catch (const TxBroadcastException& e)
		{
			Json::Value data;
			data["type"] = "TX_BROADCAST_FAILED";
			data["lifecycle"] = TxLifecycle::ToString(ETxLifecycleState::FINALIZED_NOT_BROADCAST);
			data["broadcast"] = TxLifecycle::ToString(EBroadcastResult::FAILED);
			data["finalized"] = true;
			data["tx_slate_id"] = e.GetSlateId();
			data["broadcast_error"] = e.GetBroadcastError();
			return RPC::Response::BuildError(
				request.GetId(),
				RPC::ErrorCode::INTERNAL_ERROR,
				e.what(),
				data
			);
		}
		catch (const APIException& e)
		{
			return request.BuildError(e.GetErrorCode(), e.what());
		}
		catch (std::exception& e)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, e.what());
		}
	}

	// The request contains the full slate; it must not be written to Wallet.log.
	bool ContainsSecrets() const noexcept final { return true; }

private:
	IWalletManager& m_walletManager;
	SessionToken m_token;
	TorProcess::Ptr m_pTorProcess;
};