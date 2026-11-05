#pragma once

// Copyright (c) 2018-2019 David Burkett
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <Crypto/Models/Hash.h>
#include <Core/Traits/Batchable.h>
#include <Core/Traits/Lockable.h>
#include <vector>

#define PMMR_API

// Forward Declarations
class Config;
class BlockHeader;
class Index;

class IHeaderMMR : public Traits::IBatchable
{
public:
	using CPtr = std::shared_ptr<IHeaderMMR>;

	virtual ~IHeaderMMR() = default;

	virtual void AddHeader(const BlockHeader& header) = 0;
	virtual Hash Root(const uint64_t nextHeight) const = 0;
	virtual void Rewind(const uint64_t nextHeight) = 0;
	// Number of headers committed to the MMR; used to detect a crash between the
	// MMR commit and the chain-index commit.
	virtual uint64_t GetLeafCount() const = 0;
	// Raw number of 32-byte nodes in the on-disk MMR file; used for structural
	// validation before any heights are trusted (G-23).
	virtual uint64_t GetNodeCount() const = 0;
	// Raw stored hash at a 0-based MMR position; used for full hash revalidation.
	virtual std::unique_ptr<Hash> GetHashAt(const Index& index) const = 0;
};

namespace HeaderMMRAPI
{
	PMMR_API std::shared_ptr<Locked<IHeaderMMR>> OpenHeaderMMR(const Config& config);
}