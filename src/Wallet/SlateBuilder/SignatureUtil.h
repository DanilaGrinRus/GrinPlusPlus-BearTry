#pragma once

#include <Common/Logger.h>
#include <Core/Exceptions/WalletException.h>
#include <Crypto/Crypto.h>
#include <Wallet/Models/Slate/Slate.h>
#include <Wallet/Models/Slate/SlateSignature.h>

class SignatureUtil
{
public:
	static Signature AggregateSignatures(const Slate& slate)
	{
		std::vector<CompactSignature> signatures;
		std::vector<PublicKey> pubNonces;

		for (const auto& sig : slate.sigs)
		{
			if (sig.partialOpt.has_value()) {
				WALLET_DEBUG_F("Aggregating signature for slate {}.", uuids::to_string(slate.GetId()));
				signatures.push_back(sig.partialOpt.value());
			} else {
				WALLET_WARNING("No partial signature found for a participant.");
			}
		}

		const PublicKey sumPubNonces = slate.CalculateTotalNonce();

		auto pAggSignature = Crypto::AggregateSignatures(signatures, sumPubNonces);
		if (pAggSignature == nullptr) {
			throw WALLET_EXCEPTION_F("Failed to aggregate signatures for {}", slate);
		}

		return *pAggSignature;
	}

	static bool VerifyPartialSignatures(const std::vector<SlateSignature>& sigs, const Hash& message)
	{
		std::vector<PublicKey> pubKeys;
		std::vector<PublicKey> pubNonces;

		for (const auto& sig : sigs)
		{
			pubKeys.push_back(sig.excess);
			pubNonces.push_back(sig.nonce);
		}

		const PublicKey sumPubKeys = Crypto::AddPublicKeys(pubKeys);
		const PublicKey sumPubNonces = Crypto::AddPublicKeys(pubNonces);

		for (const auto& sig : sigs)
		{
			if (sig.partialOpt.has_value())
			{
				if (!Crypto::VerifyPartialSignature(sig.partialOpt.value(), sig.excess, sumPubKeys, sumPubNonces, message))
				{
					// Never log signature/key material: it is slate data.
					WALLET_ERROR("Partial signature verification failed.");
					return false;
				}
			}
		}

		return true;
	}
};