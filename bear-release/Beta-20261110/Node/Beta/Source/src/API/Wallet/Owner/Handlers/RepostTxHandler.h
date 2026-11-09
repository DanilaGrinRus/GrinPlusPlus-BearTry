#pragma once

#include <Wallet/WalletManager.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <Net/Tor/TorProcess.h>
#include <API/Wallet/Owner/Models/Errors.h>
#include <API/Wallet/Owner/Models/RepostTxCriteria.h>
#include <API/Wallet/Owner/Models/TxLifecycle.h>
#include <optional>

class RepostTxHandler : public RPCMethod
{
public:
	RepostTxHandler(const TorProcess::Ptr& pTorProcess, const IWalletManagerPtr& pWalletManager)
		: m_pTorProcess(pTorProcess), m_pWalletManager(pWalletManager) { }
	~RepostTxHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value())
		{
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		RepostTxCriteria criteria = RepostTxCriteria::FromJSON(request.GetParams().value());

		const bool reposted = m_pWalletManager->RepostTx(criteria, m_pTorProcess);

		Json::Value result;
		// Legacy field kept unchanged for UI v1.2.8.
		result["status"] = reposted ? "SUCCESS" : "FAILED";
		result["tx_id"] = (Json::UInt64)criteria.GetTxId();
		result["broadcast"] = TxLifecycle::ToString(
			reposted ? EBroadcastResult::POSTED : EBroadcastResult::FAILED
		);
		result["lifecycle"] = TxLifecycle::ToString(
			reposted ? ETxLifecycleState::POSTED : ETxLifecycleState::FINALIZED_NOT_BROADCAST
		);
		return request.BuildResult(result);
	}

	// Requests carry the session token.
	bool ContainsSecrets() const noexcept final { return true; }

private:
	TorProcess::Ptr m_pTorProcess;
	IWalletManagerPtr m_pWalletManager;
};
