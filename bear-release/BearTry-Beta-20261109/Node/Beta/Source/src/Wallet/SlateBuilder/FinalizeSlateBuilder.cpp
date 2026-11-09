#include "FinalizeSlateBuilder.h"
#include "TransactionBuilder.h"
#include "SignatureUtil.h"
#include "SlateUtil.h"

#include <Core/Exceptions/WalletException.h>
#include <Core/Validation/KernelSignatureValidator.h>
#include <Core/Validation/TransactionValidator.h>
#include <Wallet/Models/Slatepack/Armor.h>
#include <Wallet/NodeClient.h>
#include <Wallet/Wallet.h>
#include <Wallet/WalletTxType.h>

// TODO: To help prevent "play" attacks, make sure any outputs provided by the receiver are not already on-chain.

FinalizeSlateBuilder::FinalizeResult FinalizeSlateBuilder::Finalize(const Slate& rcvSlate) const
{
	Slate finalizeSlate = rcvSlate;
	finalizeSlate.stage = ESlateStage::STANDARD_FINALIZED;

	// Load WalletTx first: it identifies whether this slate was already
	// finalized. Repeated finalize calls must not insert a duplicate slate row.
	WalletTx walletTx = m_pWallet->GetTransactionBySlateId(finalizeSlate.GetId());

	if (walletTx.GetType() == EWalletTxType::SENDING_FINALIZED)
	{
		if (!walletTx.GetTransaction().has_value())
		{
			throw WALLET_EXCEPTION_F("Finalized transaction {} has no stored transaction.", walletTx.GetId());
		}

		// The S3 slate and the SENDING_FINALIZED status are written in the same
		// database batch, so a finalized transaction always has a stored S3 slate.
		Slate storedSlate = m_pWallet->GetSlate(finalizeSlate.GetId(), ESlateStage::STANDARD_FINALIZED);

		WALLET_INFO_F(
			"Slate {} is already finalized; reusing stored transaction (idempotent finalize).",
			uuids::to_string(finalizeSlate.GetId())
		);

		return FinalizeResult{ storedSlate, walletTx.GetTransaction().value(), walletTx.GetId(), true };
	}

	if (walletTx.GetType() != EWalletTxType::SENDING_STARTED)
	{
		WALLET_ERROR_F(
			"Cannot finalize slate {}: wallet tx {} is in status {}.",
			uuids::to_string(finalizeSlate.GetId()),
			walletTx.GetId(),
			WalletTxType::ToString(walletTx.GetType())
		);
		throw WALLET_EXCEPTION_F(
			"Cannot finalize slate in status {}.",
			WalletTxType::ToString(walletTx.GetType())
		);
	}

	// Load original send Slate
	Slate sendSlate = m_pWallet->GetSlate(finalizeSlate.GetId(), ESlateStage::STANDARD_SENT);

	// Load private context
	SlateContextEntity slateContext = m_pWallet->GetSlateContext(finalizeSlate.GetId());

	if (!walletTx.GetTransaction().has_value())
	{
		throw WALLET_EXCEPTION_F("Send transaction {} has no stored transaction.", walletTx.GetId());
	}

	if (finalizeSlate.offset.IsNull()) {
		finalizeSlate.offset = walletTx.GetTransaction().value().GetOffset();
		WALLET_DEBUG_F("Offset not supplied for slate {}; using the original offset.", uuids::to_string(finalizeSlate.GetId()));
	} else {
		WALLET_DEBUG_F("Offset supplied by receiver for slate {}.", uuids::to_string(finalizeSlate.GetId()));
	}

	// Add inputs, outputs, and omitted fields
	finalizeSlate.amount = sendSlate.amount;
	finalizeSlate.fee = sendSlate.fee;
	finalizeSlate.kernelFeatures = sendSlate.kernelFeatures;
	finalizeSlate.featureArgsOpt = sendSlate.featureArgsOpt;
	finalizeSlate.ttl = sendSlate.ttl;
	std::for_each(
		slateContext.GetInputs().cbegin(), slateContext.GetInputs().cend(),
		[&finalizeSlate](const OutputDataEntity& input) {
			finalizeSlate.AddInput(input.GetFeatures(), input.GetCommitment());
		}
	);

	std::for_each(
		slateContext.GetOutputs().cbegin(), slateContext.GetOutputs().cend(),
		[&finalizeSlate](const OutputDataEntity& output) {
			finalizeSlate.AddOutput(output.GetFeatures(), output.GetCommitment(), output.GetRangeProof());
		}
	);

	// Add partial signature to slate's participant data
	const Hash kernelMessage = TransactionKernel::GetSignatureMessage(
		finalizeSlate.kernelFeatures,
		finalizeSlate.fee,
		finalizeSlate.GetLockHeight()
	);
	AddPartialSignature(slateContext, finalizeSlate, kernelMessage);

	Commitment finalExcess = Crypto::ToCommitment(finalizeSlate.CalculateTotalExcess());

	// Verify payment proof addresses & signatures
	if (!VerifyPaymentProofs(walletTx, finalizeSlate, finalExcess)) {
		WALLET_ERROR_F(
			"Failed to verify payment proof for {}",
			finalizeSlate
		);
		throw WALLET_EXCEPTION("Failed to verify payment proof.");
	}

	// Finalize transaction
	std::unique_ptr<Transaction> pTransaction = BuildTransaction(finalizeSlate, kernelMessage, finalExcess);
	if (pTransaction == nullptr) {
		WALLET_ERROR_F(
			"Failed to verify finalized transaction for {}",
			finalizeSlate
		);
		throw WALLET_EXCEPTION("Failed to verify finalized transaction.");
	}

	// Update WalletTx
	walletTx.SetType(EWalletTxType::SENDING_FINALIZED);
	walletTx.SetTransaction(*pTransaction);

	// Update database with latest WalletTx
	UpdateDatabase(
		walletTx,
		finalizeSlate,
		Armor::Pack(m_pWallet->GetSlatepackAddress(), finalizeSlate)
	);

	return FinalizeResult{ finalizeSlate, *pTransaction, walletTx.GetId(), false };
}

void FinalizeSlateBuilder::AddPartialSignature(
	const SlateContextEntity& context,
	Slate& finalizeSlate,
	const Hash& kernelMessage) const
{
	bool add_participant_info = true;
	for (const auto& sig : finalizeSlate.sigs) {
		if (sig.excess == context.CalcExcess()) {
			add_participant_info = false;
			break;
		}
	}

	// Copy sig data from original send slate to finalize slate
	if (add_participant_info) {
		SlateSignature senderSig(
			context.CalcExcess(),
			context.CalcNonce(),
			std::nullopt
		);

		finalizeSlate.sigs.push_back(senderSig);
	}

	// Generate partial signature
	CompactSignature partialSignature = Crypto::CalculatePartialSignature(
		context.GetSecretKey(),
		context.GetSecretNonce(),
		finalizeSlate.CalculateTotalExcess(),
		finalizeSlate.CalculateTotalNonce(),
		kernelMessage
	);

	for (auto& sig : finalizeSlate.sigs) {
		if (sig.excess == context.CalcExcess()) {
			sig.partialOpt = std::make_optional<CompactSignature>(partialSignature);
			break;
		}
	}

	WALLET_INFO_F("Num sigs: {}", finalizeSlate.sigs.size());

	if (!SignatureUtil::VerifyPartialSignatures(finalizeSlate.sigs, kernelMessage)) {
		WALLET_ERROR_F("Failed to verify partial signature for {}", finalizeSlate);
		throw WALLET_EXCEPTION_F("Failed to verify partial signature for {}", finalizeSlate);
	}
}

std::unique_ptr<Transaction> FinalizeSlateBuilder::BuildTransaction(
	Slate& finalizeSlate,
	const Hash& kernelMessage,
	const Commitment& finalExcess) const
{
	// Aggregate partial signatures
	Signature aggSignature = SignatureUtil::AggregateSignatures(finalizeSlate);
	
	PublicKey totalExcess = finalizeSlate.CalculateTotalExcess();
	WALLET_DEBUG_F(
		"Validating slate {}: signature {} for excess {} against message {}.",
		uuids::to_string(finalizeSlate.GetId()),
		aggSignature,
		totalExcess,
		kernelMessage
	);
	if (!Crypto::VerifyAggregateSignature(aggSignature, totalExcess, kernelMessage)) {
		WALLET_ERROR_F(
			"Failed to verify aggregate signature for {}",
			finalizeSlate
		);
		return nullptr;
	}

	// Update the tx kernel to reflect the offset excess and sig
	TransactionKernel kernel(
		finalizeSlate.kernelFeatures,
		finalizeSlate.fee,
		finalizeSlate.GetLockHeight(),
		finalExcess,
		aggSignature
	);
	if (!KernelSignatureValidator().Verify(kernel)) {
		WALLET_ERROR_F(
			"Failed to verify kernel signatures for {}",
			finalizeSlate
		);
		return nullptr;
	}

	Transaction transaction = TransactionBuilder::BuildTransaction(
		finalizeSlate.GetInputs(),
		finalizeSlate.GetOutputs(),
		kernel,
		finalizeSlate.offset
	);

	try
	{
		const uint64_t block_height = m_pNode->GetChainHeight() + 1;
		TransactionValidator().Validate(transaction, block_height); // TODO: Check if inputs unspent(txHashSet->Validate())?
	}
	catch (std::exception& e)
	{
		WALLET_ERROR_F(
			"Failed to validate transaction for {}. Error: {}",
			finalizeSlate,
			e.what()
		);
		return nullptr;
	}

	WALLET_INFO_F("Final transaction: {}", transaction);

	return std::make_unique<Transaction>(std::move(transaction));
}

bool FinalizeSlateBuilder::VerifyPaymentProofs(
	const WalletTx& walletTx,
	const Slate& finalizeSlate,
	const Commitment& finalExcess) const
{
	auto& paymentProofOpt = walletTx.GetPaymentProof();
	if (paymentProofOpt.has_value()) {
		if (!finalizeSlate.GetPaymentProof().has_value()) {
			return false;
		}

		auto& origProof = paymentProofOpt.value();
		auto& newProof = finalizeSlate.GetPaymentProof().value();

		if (origProof.GetReceiverAddress() != newProof.GetReceiverAddress()
			|| origProof.GetSenderAddress() != newProof.GetSenderAddress()
			|| !newProof.GetReceiverSignature().has_value())
		{
			return false;
		}

		// Check signature - Message: (amount | kernel commitment | sender address)
		Serializer messageSerializer;
		messageSerializer.Append<uint64_t>(finalizeSlate.GetAmount());
		finalExcess.Serialize(messageSerializer);
		messageSerializer.AppendBigInteger(newProof.GetSenderAddress().bytes);

		return ED25519::VerifySignature(
			newProof.GetReceiverAddress(),
			newProof.GetReceiverSignature().value(),
			messageSerializer.GetBytes()
		);
	}

	return true;
}

void FinalizeSlateBuilder::UpdateDatabase(
	const WalletTx& walletTx,
	const Slate& finalizeSlate,
	const std::string& armored_slatepack) const
{
	auto pBatch = m_pWallet->GetDatabase().BatchWrite();
	pBatch->SaveSlate(m_pWallet->GetMasterSeed(), finalizeSlate, armored_slatepack);
	pBatch->AddTransaction(m_pWallet->GetMasterSeed(), walletTx);
	pBatch->Commit();
}