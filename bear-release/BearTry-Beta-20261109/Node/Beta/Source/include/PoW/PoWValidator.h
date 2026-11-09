#pragma once

#include <Core/Models/BlockHeader.h>
#include <Database/BlockDb.h>
#include <Consensus.h>

class PoWValidator
{
public:
	PoWValidator(const IBlockDB::CPtr& pBlockDB)
		: m_pBlockDB(pBlockDB) { }

	//
	// Validates the difficulty, algo, etc of the header's proof of work.
	// Returns true if the PoW is valid.
	//
	bool IsPoWValid(const BlockHeader& header, const BlockHeader& previousHeader) const;

	// Edge bits accepted by grin v5.5.2: Cuckoo29 (secondary) or Cuckatoo
	// 31..63 (primary, DEFAULT_MIN_EDGE_BITS after HF4). Values above 63 would
	// shift a 64-bit word by >= 64 in Cuckatoo and are rejected up front.
	static bool IsSupportedEdgeBits(const uint8_t edgeBits) noexcept
	{
		return edgeBits == Consensus::SECOND_POW_EDGE_BITS ||
			(edgeBits >= Consensus::DEFAULT_MIN_EDGE_BITS && edgeBits <= 63);
	}

private:
	uint64_t GetMaximumDifficulty(const BlockHeader& header) const;

	std::shared_ptr<const IBlockDB> m_pBlockDB;
};