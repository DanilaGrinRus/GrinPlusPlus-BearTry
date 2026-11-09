#pragma once

#include <Core/Config.h>
#include <Wallet/WalletManager.h>
#include <Wallet/Models/Slatepack/SlatepackAddress.h>
#include <Wallet/Models/Slatepack/Armor.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Tor/TorConnection.h>
#include <Net/Tor/TorProcess.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <Common/Util/FileUtil.h>
#include <API/Wallet/Owner/Models/Errors.h>
#include <API/Wallet/Owner/Models/SendResponse.h>
#include <API/Wallet/Owner/Models/TxLifecycle.h>
#include <API/Wallet/Foreign/Clients/CheckVersionClient.h>
#include <API/Wallet/Foreign/Clients/ReceiveTxClient.h>
#include <optional>

class SendHandler : public RPCMethod
{
public:
	SendHandler(const TorProcess::Ptr& pTorProcess, const IWalletManagerPtr& pWalletManager)
		: m_pTorProcess(pTorProcess), m_pWalletManager(pWalletManager) { }
	~SendHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value()) {
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		SendCriteria criteria = SendCriteria::FromJSON(request.GetParams().value());

		auto wallet = m_pWalletManager->GetWallet(criteria.GetToken());
		SlatepackAddress sender_address = wallet.Read()->GetSlatepackAddress();
		std::vector<SlatepackAddress> recipients = GetRecipients(criteria);

		if (!recipients.empty()) {
			criteria.SetSlateVersion(GetSlateVersion(recipients.front().ToTorAddress()));
		}

		Slate slate = m_pWalletManager->Send(criteria);
		LOG_INFO_F(
			"Built send slate {} (stage={}, amount={}, fee={}).",
			uuids::to_string(slate.GetId()),
			slate.stage.ToString(),
			slate.GetAmount(),
			slate.GetFee().GetFee()
		);

		const std::string slateId = uuids::to_string(slate.GetId());
		std::string slatepack = Armor::Pack(sender_address, slate, recipients);

		if (!recipients.empty()) {
			try
			{
				slate = SendViaTOR(criteria, slate, recipients.front().ToTorAddress());
				LOG_INFO_F(
					"Finalized slate {} (stage={}).",
					uuids::to_string(slate.GetId()),
					slate.stage.ToString()
				);

				slatepack = Armor::Pack(sender_address, slate);

				// "FINALIZED" only describes the slate. The transaction is
				// actually posted only when the caller requested post_tx.
				const bool postRequested = criteria.GetPostMethod().has_value();
				return request.BuildResult(
					SendResponse(
						SendResponse::EStatus::FINALIZED,
						slate,
						slatepack,
						"",
						TxLifecycle::ToString(
							postRequested ? ETxLifecycleState::POSTED : ETxLifecycleState::FINALIZED
						),
						TxLifecycle::ToString(
							postRequested ? EBroadcastResult::POSTED : EBroadcastResult::NOT_REQUESTED
						),
						true,
						uuids::to_string(slate.GetId())
					).ToJSON()
				);
			}
			catch (const TxBroadcastException& e)
			{
				// The slate IS finalized (and persisted) but the node/pool
				// rejected the broadcast. Keep the legacy SENT status for UI
				// compatibility, add explicit lifecycle/broadcast fields.
				LOG_ERROR_F("Slate {} finalized but failed to broadcast: {}", slateId, e.GetBroadcastError());
				return request.BuildResult(
					SendResponse(
						SendResponse::EStatus::SENT,
						slate,
						slatepack,
						e.what(),
						TxLifecycle::ToString(ETxLifecycleState::FINALIZED_NOT_BROADCAST),
						TxLifecycle::ToString(EBroadcastResult::FAILED),
						true,
						e.GetSlateId()
					).ToJSON()
				);
			}
			catch (const std::exception& e)
			{
				// Finalization itself failed (or the receiver was unreachable):
				// nothing was published and the wallet tx stays SENDING_STARTED.
				LOG_ERROR_F("Failed to finalize slate {}: {}", slateId, e.what());
				return request.BuildResult(
					SendResponse(
						SendResponse::EStatus::SENT,
						slate,
						slatepack,
						e.what(),
						TxLifecycle::ToString(ETxLifecycleState::NOT_FINALIZED),
						TxLifecycle::ToString(EBroadcastResult::UNKNOWN),
						false,
						slateId
					).ToJSON()
				);
			}
		}

		// Offline slatepack flow: the slate is built but nothing is finalized or
		// broadcast until the counterparty responds.
		return request.BuildResult(
			SendResponse(
				SendResponse::EStatus::SENT,
				slate,
				slatepack,
				"",
				TxLifecycle::ToString(ETxLifecycleState::NOT_FINALIZED),
				TxLifecycle::ToString(EBroadcastResult::NOT_REQUESTED),
				false,
				slateId
			).ToJSON()
		);
	}

	// Requests carry the session token; replies carry the full slate/slatepack.
	// Neither may be written to Wallet.log.
	bool ContainsSecrets() const noexcept final { return true; }

private:
	std::vector<SlatepackAddress> GetRecipients(const SendCriteria& criteria) const noexcept
	{
		std::vector<SlatepackAddress> recipients;

		if (criteria.GetAddress().has_value()) {
			try
			{
				SlatepackAddress slatepack_address = SlatepackAddress::Parse(criteria.GetAddress().value());
				recipients.emplace_back(std::move(slatepack_address));
			}
			catch (std::exception&) { }
		}
	
		return recipients;
	}

	uint16_t GetSlateVersion(const TorAddress& torAddress) const
	{
		TorConnectionPtr pTorConnection = m_pTorProcess->Connect(torAddress);
		if (pTorConnection == nullptr) {
			return MAX_SLATE_VERSION;
		}

		CheckVersionClient::Response version_response = CheckVersionClient::Call(*pTorConnection);
		if (!version_response.error.empty()) {
			return MAX_SLATE_VERSION;
		}

		if (version_response.slate_version < MIN_SLATE_VERSION) {
			throw RPC_EXCEPTION(RPC::Errors::SLATE_VERSION_MISMATCH.GetMsg(), std::nullopt);
		}

		return version_response.slate_version;
	}

	Slate SendViaTOR(SendCriteria& criteria, const Slate& sent_slate, const TorAddress& torAddress) const
	{
		TorConnectionPtr pTorConnection = m_pTorProcess->Connect(torAddress);
		if (pTorConnection == nullptr) {
			throw RPC_EXCEPTION(RPC::Errors::RECEIVER_UNREACHABLE.GetMsg(), std::nullopt);
		}

		Slate received_slate = ReceiveTxClient::Call(m_pTorProcess, torAddress, sent_slate);
		LOG_INFO_F(
			"Received receiver slate {} (stage={}).",
			uuids::to_string(received_slate.GetId()),
			received_slate.stage.ToString()
		);

		try
		{
			FinalizeCriteria finalize_criteria(
				criteria.GetToken(),
				std::nullopt,
				std::move(received_slate),
				criteria.GetPostMethod()
			);

			Slate slate = m_pWalletManager->Finalize(finalize_criteria, m_pTorProcess);

			return slate;
		}
		catch (const TxBroadcastException&)
		{
			// Preserve lifecycle information for the caller.
			throw;
		}
		catch (std::exception&)
		{
			throw RPC_EXCEPTION("Failed to finalize.", std::nullopt);
		}
	}

	TorProcess::Ptr m_pTorProcess;
	IWalletManagerPtr m_pWalletManager;
};
