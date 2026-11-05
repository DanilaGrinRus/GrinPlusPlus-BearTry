#pragma once

#include <caches/Cache.h>
#include <Crypto/Hasher.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/Hash.h>
#include <Crypto/Models/RangeProof.h>
#include <mutex>
#include <vector>

class BulletProofsCache
{
public:
	BulletProofsCache()
		: m_bulletproofsCache(3000)
	{

	}

	// The cache key commits to both the commitment and the exact proof bytes.
	// A changed proof for the same commitment therefore misses the cache and is
	// verified again.
	void AddToCache(const Commitment& commitment, const RangeProof& proof)
	{
		const Hash key = BuildKey(commitment, proof);
		std::unique_lock<std::mutex> lock(m_mutex);
		m_bulletproofsCache.Put(key, key);
	}

	bool WasAlreadyVerified(const Commitment& commitment, const RangeProof& proof) const
	{
		const Hash key = BuildKey(commitment, proof);
		std::unique_lock<std::mutex> lock(m_mutex);
		if (m_bulletproofsCache.Cached(key)) {
			return m_bulletproofsCache.Get(key) == key;
		}

		return false;
	}

private:
	static Hash BuildKey(const Commitment& commitment, const RangeProof& proof)
	{
		std::vector<uint8_t> keyBytes;
		const std::vector<unsigned char>& commitmentBytes = commitment.GetVec();
		const std::vector<unsigned char>& proofBytes = proof.GetProofBytes();
		keyBytes.reserve(commitmentBytes.size() + proofBytes.size());
		keyBytes.insert(keyBytes.end(), commitmentBytes.cbegin(), commitmentBytes.cend());
		keyBytes.insert(keyBytes.end(), proofBytes.cbegin(), proofBytes.cend());
		return Hasher::Blake2b(keyBytes);
	}

	mutable std::mutex m_mutex;
	mutable LRUCache<Hash, Hash> m_bulletproofsCache;
};
