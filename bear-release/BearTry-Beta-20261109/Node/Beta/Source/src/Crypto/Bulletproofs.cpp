#include "Bulletproofs.h"
#include "Pedersen.h"

#include <secp256k1-zkp/secp256k1_bulletproofs.h>
#include <Common/Util/FunctionalUtil.h>
#include <Common/Logger.h>
#include <Crypto/CSPRNG.h>
#include <Core/Exceptions/CryptoException.h>
#include <map>

const uint64_t MAX_WIDTH = 1 << 20;
const size_t SCRATCH_SPACE_SIZE = 256 * MAX_WIDTH;
const size_t MAX_GENERATORS = 256;

static Bulletproofs instance;

Bulletproofs& Bulletproofs::GetInstance()
{
	return instance;
}

Bulletproofs::Bulletproofs()
{
	m_pContext = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
	m_pGenerators = secp256k1_bulletproof_generators_create(m_pContext, &secp256k1_generator_const_g, MAX_GENERATORS);
}

Bulletproofs::~Bulletproofs()
{
	secp256k1_bulletproof_generators_destroy(m_pContext, m_pGenerators);
	secp256k1_context_destroy(m_pContext);
}

bool Bulletproofs::VerifyBulletproofs(const std::vector<std::pair<Commitment, RangeProof>>& rangeProofs) const
{
	std::shared_lock<std::shared_mutex> readLock(m_mutex);

	if (rangeProofs.empty()) {
		return true;
	}

	// The secp256k1 multi-verify API assumes one proof length for the whole
	// batch. Group by length instead of applying the first proof's length to
	// every element; empty or oversized proofs are rejected before secp sees
	// them.
	std::map<size_t, std::vector<size_t>> groups;
	for (size_t i = 0; i < rangeProofs.size(); ++i) {
		const size_t proofLength = rangeProofs[i].second.GetProofBytes().size();
		if (proofLength == 0 || proofLength > MAX_PROOF_SIZE) {
			LOG_ERROR_F("Invalid rangeproof length {}", proofLength);
			return false;
		}
		groups[proofLength].push_back(i);
	}

	const size_t numBits = 64;
	for (const auto& group : groups) {
		const size_t proofLength = group.first;

		std::vector<Commitment> commitments;
		std::vector<const unsigned char*> bulletproofPointers;
		std::vector<size_t> uncachedIndices;
		for (const size_t index : group.second) {
			const Commitment& commitment = rangeProofs[index].first;
			const RangeProof& proof = rangeProofs[index].second;
			if (!m_cache.WasAlreadyVerified(commitment, proof)) {
				commitments.push_back(commitment);
				bulletproofPointers.emplace_back(proof.GetProofBytes().data());
				uncachedIndices.push_back(index);
			}
		}

		if (commitments.empty()) {
			continue;
		}

		// array of generator multiplied by value in pedersen commitments (cannot be NULL)
		std::vector<secp256k1_generator> valueGenerators(
			commitments.size(), secp256k1_generator_const_h);

		std::vector<secp256k1_pedersen_commitment*> commitmentPointers =
			Pedersen::ConvertCommitments(*m_pContext, commitments);
		if (commitmentPointers.empty()) {
			LOG_ERROR("Failed to convert rangeproof commitments");
			return false;
		}

		secp256k1_scratch_space* pScratchSpace =
			secp256k1_scratch_space_create(m_pContext, SCRATCH_SPACE_SIZE);
		if (pScratchSpace == nullptr) {
			Pedersen::CleanupCommitments(commitmentPointers);
			LOG_ERROR("Failed to allocate rangeproof scratch space");
			return false;
		}

		const int result = secp256k1_bulletproof_rangeproof_verify_multi(
			m_pContext,
			pScratchSpace,
			m_pGenerators,
			bulletproofPointers.data(),
			commitments.size(),
			proofLength,
			NULL,
			commitmentPointers.data(),
			1,
			numBits,
			valueGenerators.data(),
			NULL,
			NULL);
		secp256k1_scratch_space_destroy(pScratchSpace);
		Pedersen::CleanupCommitments(commitmentPointers);

		if (result != 1) {
			LOG_ERROR_F("Failed to validate {} rangeproofs (length {})", commitments.size(), proofLength);
			return false;
		}

		// Cache only after the whole group verified successfully.
		for (const size_t index : uncachedIndices) {
			m_cache.AddToCache(rangeProofs[index].first, rangeProofs[index].second);
		}
	}

	return true;
}

RangeProof Bulletproofs::GenerateRangeProof(const uint64_t amount, const SecretKey& key, const SecretKey& privateNonce, const SecretKey& rewindNonce, const ProofMessage& proofMessage) const
{
	std::unique_lock<std::shared_mutex> writeLock(m_mutex);

	const SecretKey randomSeed = CSPRNG::GenerateRandom32();
	const int randomizeResult = secp256k1_context_randomize(m_pContext, randomSeed.data());
	if (randomizeResult != 1) {
		throw CryptoException("secp256k1_context_randomize failed with error: " + std::to_string(randomizeResult));
	}

	std::vector<uint8_t> proofBytes(MAX_PROOF_SIZE, 0);
	size_t proofLen = MAX_PROOF_SIZE;

	secp256k1_scratch_space* pScratchSpace = secp256k1_scratch_space_create(m_pContext, SCRATCH_SPACE_SIZE);

	std::vector<const unsigned char*> blindingFactors({ key.data() });
	int result = secp256k1_bulletproof_rangeproof_prove(
		m_pContext,
		pScratchSpace,
		m_pGenerators,
		proofBytes.data(),
		&proofLen,
		NULL,
		NULL,
		NULL,
		&amount,
		NULL,
		blindingFactors.data(),
		NULL,
		1,
		&secp256k1_generator_const_h,
		64,
		rewindNonce.data(),
		privateNonce.data(),
		NULL,
		0,
		proofMessage.data()
	);
	secp256k1_scratch_space_destroy(pScratchSpace);

	if (result != 1) {
		throw CRYPTO_EXCEPTION_F("secp256k1_bulletproof_rangeproof_prove failed with error: {}", result);
	}

	proofBytes.resize(proofLen);
	return RangeProof(std::move(proofBytes));
}

std::unique_ptr<RewoundProof> Bulletproofs::RewindProof(const Commitment& commitment, const RangeProof& rangeProof, const SecretKey& nonce) const
{
	std::shared_lock<std::shared_mutex> readLock(m_mutex);

	std::vector<secp256k1_pedersen_commitment*> commitmentPointers = Pedersen::ConvertCommitments(*m_pContext, std::vector<Commitment>({ commitment }));

	if (!commitmentPointers.empty())
	{
		uint64_t value;
		SecretKey blinding_factor;
		ProofMessage message;

		int result = secp256k1_bulletproof_rangeproof_rewind(
			m_pContext,
			&value,
			blinding_factor.data(),
			rangeProof.GetProofBytes().data(),
			rangeProof.GetProofBytes().size(),
			0,
			commitmentPointers.front(),
			&secp256k1_generator_const_h,
			nonce.data(),
			NULL,
			0,
			message.data()
		);
		Pedersen::CleanupCommitments(commitmentPointers);

		if (result == 1)
		{
			return std::make_unique<RewoundProof>(RewoundProof(
				value, 
				std::make_unique<SecretKey>(std::move(blinding_factor)), 
				std::move(message)
			));
		}
	}

	return std::unique_ptr<RewoundProof>(nullptr);
}