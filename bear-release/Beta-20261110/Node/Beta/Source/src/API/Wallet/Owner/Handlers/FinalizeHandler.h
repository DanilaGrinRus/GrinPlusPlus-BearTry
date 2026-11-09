#pragma once

#include <Core/Config.h>
#include <Wallet/WalletManager.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <Net/Tor/TorProcess.h>
#include <API/Wallet/Owner/Models/Errors.h>
#include <API/Wallet/Owner/Models/TxLifecycle.h>
#include <Common/Logger.h>
#include <Common/Util/FileUtil.h>
#include <optional>

class FinalizeHandler : public RPCMethod
{
public:
	FinalizeHandler(const TorProcess::Ptr& pTorProcess, const IWalletManagerPtr& pWalletManager)
		: m_pTorProcess(pTorProcess), m_pWalletManager(pWalletManager) { }
	~FinalizeHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value())
		{
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		struct SlatepackDecryptor : public ISlatepackDecryptor
		{
			SlatepackDecryptor(const IWalletManagerPtr& pWalletManager_)
				: pWalletManager(pWalletManager_){ }

			IWalletManagerPtr pWalletManager;

			SlatepackMessage Decrypt(const SessionToken& token, const std::string& armored) const final
			{
				return pWalletManager->GetWallet(token).Read()->DecryptSlatepack(armored);
			}
		};

		FinalizeCriteria criteria = FinalizeCriteria::FromJSON(request.GetParams().value(), SlatepackDecryptor{ m_pWalletManager });

		Slate slate;
		try
		{
			slate = m_pWalletManager->Finalize(criteria, m_pTorProcess);
		}
		catch (const TxBroadcastException& e)
		{
			// Finalization succeeded and the state is persisted, but publishing
			// failed. Return a coded error carrying the lifecycle so the caller
			// can retry with repost_tx instead of resending a new slate.
			Json::Value data;
			data["type"] = "TX_BROADCAST_FAILED";
			data["lifecycle"] = TxLifecycle::ToString(ETxLifecycleState::FINALIZED_NOT_BROADCAST);
			data["broadcast"] = TxLifecycle::ToString(EBroadcastResult::FAILED);
			data["finalized"] = true;
			data["tx_slate_id"] = e.GetSlateId();
			data["broadcast_error"] = e.GetBroadcastError();

			WALLET_ERROR_F("Slate {} finalized but failed to broadcast: {}", e.GetSlateId(), e.GetBroadcastError());
			return RPC::Response::BuildError(
				request.GetId(),
				RPC::Errors::TX_BROADCAST_FAILED.GetCode(),
				e.what(),
				data
			);
		}

		std::vector<SlatepackAddress> recipients;
		if (criteria.GetSlatepack().has_value() && !criteria.GetSlatepack().value().m_sender.IsNull()) {
			recipients.push_back(criteria.GetSlatepack().value().m_sender);
		}

		SlatepackAddress address = m_pWalletManager->GetWallet(criteria.GetToken()).Read()->GetSlatepackAddress();
		const bool postRequested = criteria.GetPostMethod().has_value();

		Json::Value result;
		result["status"] = "FINALIZED";
		result["slate"] = slate.ToJSON();
		result["slatepack"] = Armor::Pack(address, slate, recipients);
		result["finalized"] = true;
		result["tx_slate_id"] = uuids::to_string(slate.GetId());
		result["broadcast"] = TxLifecycle::ToString(
			postRequested ? EBroadcastResult::POSTED : EBroadcastResult::NOT_REQUESTED
		);
		result["lifecycle"] = TxLifecycle::ToString(
			postRequested ? ETxLifecycleState::POSTED : ETxLifecycleState::FINALIZED
		);
		return request.BuildResult(result);
	}

	// Requests carry the session token; replies carry the finalized slate.
	bool ContainsSecrets() const noexcept final { return true; }

private:
	TorProcess::Ptr m_pTorProcess;
	IWalletManagerPtr m_pWalletManager;
};
