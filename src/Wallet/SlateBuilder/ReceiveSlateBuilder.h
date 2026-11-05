#pragma once

#include "../WalletImpl.h"
#include "SigningKeys.h"

#include <Common/Secure.h>
#include <Wallet/Models/Slate/Slate.h>
#include <cstdint>
#include <optional>

class ReceiveSlateBuilder
{
public:
	ReceiveSlateBuilder() = default;

	//
	// Rejects slates that must not be received: wrong stage, zero amount, or an
	// expired TTL. Throws APIException with an owner-API error code.
	// Public so regression tests can exercise the validation without a wallet.
	//
	static void ValidateIncomingSlate(const Slate& slate, const uint64_t chainHeight);

	Slate AddReceiverData(
		Locked<WalletImpl> wallet,
		const SecureVector& masterSeed,
		const Slate& slate,
		const std::optional<SlatepackAddress>& addressOpt,
		const std::vector<SlatepackAddress>& recipients,
		const uint64_t chainHeight
	) const;

private:
	bool VerifySlateStatus(std::shared_ptr<WalletImpl> pWallet, const SecureVector& masterSeed, const Slate& slate) const;
	SlateSignature BuildSignature(Slate& slate, const SigningKeys& signing_keys) const;
	void UpdatePaymentProof(
		const std::shared_ptr<WalletImpl>& pWallet,
		const SecureVector& masterSeed,
		Slate& receiveSlate,
		const int currentIndex
	) const;
	void UpdateDatabase(
		std::shared_ptr<IWalletDB> pBatch,
		const SecureVector& masterSeed,
		const Slate& receiveSlate,
		const OutputDataEntity& outputData,
		const uint32_t walletTxId,
		const std::optional<SlatepackAddress>& addressOpt,
		const std::string& armored_slatepack
	) const;
};