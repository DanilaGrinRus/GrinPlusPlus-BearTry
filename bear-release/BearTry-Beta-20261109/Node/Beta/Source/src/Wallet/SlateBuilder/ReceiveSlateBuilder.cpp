#include "ReceiveSlateBuilder.h"
#include "SignatureUtil.h"
#include "TransactionBuilder.h"
#include "SlateUtil.h"

#include <Core/Exceptions/WalletException.h>
#include <Core/Exceptions/APIException.h>
#include <API/Wallet/Owner/Models/Errors.h>
#include <Wallet/Models/Slatepack/Armor.h>
#include <Common/Logger.h>

void ReceiveSlateBuilder::ValidateIncomingSlate(const Slate& slate, const uint64_t chainHeight)
{
	// Only a standard send slate (S1) can be received. Receiving an already
	// received/finalized slate would create a second output and a second wallet
	// transaction for the same slate id.
	if (slate.stage != ESlateStage::STANDARD_SENT)
	{
		WALLET_ERROR_F(
			"Refusing to receive slate {} in stage {}.",
			uuids::to_string(slate.GetId()),
			slate.stage.ToString()
		);
		throw API_EXCEPTION(
			RPC::Errors::SLATE_STATE_INVALID.GetCode(),
			"Slate is not in a receivable state (expected Standard1)"
		);
	}

	// Zero-amount slates are not valid payments (matches Rust receive_tx).
	if (slate.amount == 0)
	{
		WALLET_ERROR_F("Refusing to receive zero-amount slate {}.", uuids::to_string(slate.GetId()));
		throw API_EXCEPTION(
			RPC::Errors::SLATE_AMOUNT_INVALID.GetCode(),
			"Slate amount must be greater than 0"
		);
	}

	// ttl == 0 means "no TTL". Otherwise the cutoff height is exclusive:
	// at/after the cutoff the slate must no longer be processed.
	if (slate.ttl != 0 && chainHeight >= slate.ttl)
	{
		WALLET_ERROR_F(
			"Refusing to receive expired slate {} (ttl={}, height={}).",
			uuids::to_string(slate.GetId()),
			slate.ttl,
			chainHeight
		);
		throw API_EXCEPTION(
			RPC::Errors::SLATE_TTL_EXPIRED.GetCode(),
			"Slate TTL has expired"
		);
	}
}

Slate ReceiveSlateBuilder::AddReceiverData(
	Locked<WalletImpl> wallet,
	const SecureVector& masterSeed,
	const Slate& send_slate,
	const std::optional<SlatepackAddress>& addressOpt,
	const std::vector<SlatepackAddress>& recipients,
	const uint64_t chainHeight) const
{
	WALLET_INFO_F("Receiving slate {}", uuids::to_string(send_slate.GetId()));

	ValidateIncomingSlate(send_slate, chainHeight);

	auto pWallet = wallet.Write();
	Slate receiveSlate = send_slate;
	receiveSlate.stage = ESlateStage::STANDARD_RECEIVED;

	// Verify slate was never received, exactly one kernel exists, and fee is valid.
	if (!VerifySlateStatus(pWallet.GetShared(), masterSeed, receiveSlate))
	{
		throw WALLET_EXCEPTION("Failed to verify received slate.");
	}

	auto pBatch = pWallet->GetDatabase().BatchWrite();

	// Generate output
	KeyChainPath keyChainPath = pBatch->GetNextChildPath(pWallet->GetUserPath());
	const uint32_t walletTxId = pBatch->GetNextTransactionId();
	OutputDataEntity outputData = pWallet->CreateBlindedOutput(
		masterSeed,
		receiveSlate.GetAmount(),
		keyChainPath,
		walletTxId,
		EBulletproofType::ENHANCED
	);

	const BlindingFactor receiver_offset = CSPRNG::GenerateRandom32();
	SigningKeys signing_keys = SlateUtil::CalculateSigningKeys({}, { outputData }, receiver_offset);

	// Adjust transaction offset
	receiveSlate.offset = Crypto::AddBlindingFactors({ receiveSlate.offset, receiver_offset }, {});
	WALLET_DEBUG_F("Adjusted transaction offset for slate {}.", uuids::to_string(receiveSlate.GetId()));

	// Add receiver ParticipantData
	SlateSignature signature = BuildSignature(receiveSlate, signing_keys);

	// Add output to Transaction
	receiveSlate.AddOutput(outputData.GetFeatures(), outputData.GetCommitment(), outputData.GetRangeProof());

	WALLET_DEBUG("Updating Payment Proof...");

	UpdatePaymentProof(
		pWallet.GetShared(),
		masterSeed,
		receiveSlate,
		pBatch->GetCurrentAddressIndex(KeyChainPath::FromString("m/0/0"))
	);
	
	receiveSlate.sigs = std::vector<SlateSignature>{ signature };
	UpdateDatabase(
		pBatch.GetShared(),
		masterSeed,
		receiveSlate,
		outputData,
		walletTxId,
		addressOpt,
		Armor::Pack(pWallet->GetSlatepackAddress(), receiveSlate, recipients)
	);

	pBatch->Commit();

	return receiveSlate;
}

bool ReceiveSlateBuilder::VerifySlateStatus(
	std::shared_ptr<WalletImpl> pWallet,
	const SecureVector& masterSeed,
	const Slate& slate) const
{
	// Slate was already received.
	std::unique_ptr<WalletTx> pWalletTx = pWallet->GetTxBySlateId(masterSeed, slate.GetId());
	if (pWalletTx != nullptr) {
		const EWalletTxType status = pWalletTx->GetType();
		if (status != EWalletTxType::RECEIVED_CANCELED && status != EWalletTxType::SENDING_STARTED) {
			WALLET_ERROR_F("Already received slate {}", uuids::to_string(slate.GetId()));
			return false;
		}
	}

	return true;
}

SlateSignature ReceiveSlateBuilder::BuildSignature(Slate& slate, const SigningKeys& signing_keys) const
{
	const Hash kernelMessage = TransactionKernel::GetSignatureMessage(
		slate.kernelFeatures,
		slate.fee,
		slate.GetLockHeight()
	);

	// Generate signature
	slate.sigs.push_back(SlateSignature{ signing_keys.public_key, signing_keys.public_nonce, std::nullopt });

	CompactSignature partialSignature = Crypto::CalculatePartialSignature(
		signing_keys.secret_key,
		signing_keys.secret_nonce,
		slate.CalculateTotalExcess(),
		slate.CalculateTotalNonce(),
		kernelMessage
	);

	slate.sigs.back().partialOpt = std::make_optional<CompactSignature>(partialSignature);

	if (!SignatureUtil::VerifyPartialSignatures(slate.sigs, kernelMessage)) {
		WALLET_ERROR_F("Failed to verify signature for slate {}", uuids::to_string(slate.GetId()));
		throw WALLET_EXCEPTION("Failed to verify signatures.");
	}

	// Add receiver's ParticipantData to Slate
	return slate.sigs.back();
}

void ReceiveSlateBuilder::UpdatePaymentProof(
	const std::shared_ptr<WalletImpl>& pWallet,
	const SecureVector& masterSeed,
	Slate& receiveSlate,
	const int currentIndex) const
{
	if (receiveSlate.GetPaymentProof().has_value())
	{
		if (!pWallet->GetTorAddress().has_value())
		{
			throw WALLET_EXCEPTION("");
		}

		SlatePaymentProof& proof = receiveSlate.GetPaymentProof().value();
		
		int address_index = pWallet->FindAddressIndex(masterSeed, proof.GetReceiverAddress(), currentIndex);

		Commitment kernel_commitment = Crypto::ToCommitment(receiveSlate.CalculateTotalExcess());
		WALLET_DEBUG_F("Calculated payment-proof commitment for slate {}.", uuids::to_string(receiveSlate.GetId()));

		// Message: (amount | kernel commitment | sender address)
		Serializer messageSerializer;
		messageSerializer.Append<uint64_t>(receiveSlate.GetAmount());
		kernel_commitment.Serialize(messageSerializer);
		messageSerializer.AppendBigInteger(proof.GetSenderAddress().bytes);

		KeyChain keyChain = KeyChain::FromSeed(masterSeed);
		ed25519_keypair_t torKey = keyChain.DeriveED25519Key(KeyChainPath::FromString("m/0/1").GetChild(address_index));

		ed25519_signature_t signature = ED25519::Sign(
			torKey.secret_key,
			messageSerializer.GetBytes()
		);
		proof.AddSignature(std::move(signature));
		WALLET_DEBUG_F("Added payment-proof receiver signature for slate {}.", uuids::to_string(receiveSlate.GetId()));
	}
}

void ReceiveSlateBuilder::UpdateDatabase(
	std::shared_ptr<IWalletDB> pBatch,
	const SecureVector& masterSeed,
	const Slate& receiveSlate,
	const OutputDataEntity& outputData,
	const uint32_t walletTxId,
	const std::optional<SlatepackAddress>& addressOpt,
	const std::string& armored_slatepack) const
{
	// Save OutputDataEntity
	pBatch->AddOutputs(masterSeed, std::vector<OutputDataEntity>{ outputData });

	// Save WalletTx
	WalletTx walletTx(
		walletTxId,
		EWalletTxType::RECEIVING_IN_PROGRESS,
		std::make_optional(receiveSlate.GetId()),
		addressOpt.has_value() ? std::make_optional(addressOpt.value().ToString()) : std::nullopt,
		std::nullopt,
		std::chrono::system_clock::now(),
		std::nullopt,
		std::nullopt,
		receiveSlate.amount,
		0,
		std::make_optional<Fee>(receiveSlate.fee),
		std::nullopt,
		receiveSlate.GetPaymentProof()
	);

	pBatch->AddTransaction(masterSeed, walletTx);

	// receiveSlate.amount = 0;
	// receiveSlate.fee = 0;
	pBatch->SaveSlate(masterSeed, receiveSlate, armored_slatepack);
}