// Derived from grin 5.5.2 (Rust), Copyright The Grin Developers.
// Licensed under the Apache License, Version 2.0. Modified for the Grin++ C++ fork.
// See THIRD_PARTY_NOTICES.md and LICENSES/Apache-2.0.txt for details.
#pragma once

#include "Pool.h"

#include <TxPool/TransactionPool.h>
#include <Core/Models/Transaction.h>
#include <Core/Models/ShortId.h>
#include <Crypto/Models/Hash.h>
#include <Core/Enums/Environment.h>
#include <shared_mutex>
#include <set>

class TransactionPool : public ITransactionPool
{
public:
	// D2 Rust grin 5.5.2 pool/src/transaction_pool.rs verify_kernel_variants:
	// NRD is rejected when the network feature is off, then when the head header
	// version is below HF3/v4. Pure seam for tests; AddTransaction wires it to
	// Global::GetEnv() and the last confirmed header version.
	enum class ENrdPolicyResult
	{
		OK,
		NRD_NOT_ENABLED,
		NRD_PRE_HF3
	};

	static ENrdPolicyResult CheckNrdPolicy(
		const std::vector<TransactionKernel>& kernels,
		const uint16_t headerVersion,
		const Environment environment);

	TransactionPool(const Config& config)
		: m_config(config), m_memPool(), m_stemPool() { }
    virtual ~TransactionPool() = default;

	std::vector<TransactionPtr> GetTransactionsByShortId(const Hash& hash, const uint64_t nonce, const std::set<ShortId>& missingShortIds) const final;
	EAddTransactionStatus AddTransaction(std::shared_ptr<const IBlockDB> pBlockDB, ITxHashSetConstPtr pTxHashSet, TransactionPtr pTransaction, const EPoolType poolType, const BlockHeader& lastConfirmedBlock) final;
	std::vector<TransactionPtr> FindTransactionsByKernel(const std::set<TransactionKernel>& kernels) const final;
	TransactionPtr FindTransactionByKernelHash(const Hash& kernelHash) const final;
	void ReconcileBlock(std::shared_ptr<const IBlockDB> pBlockDB, ITxHashSetConstPtr pTxHashSet, const FullBlock& block) final;

	// Dandelion
	TransactionPtr GetTransactionToStem(std::shared_ptr<const IBlockDB> pBlockDB, ITxHashSetConstPtr pTxHashSet) final;
	TransactionPtr GetTransactionToFluff(std::shared_ptr<const IBlockDB> pBlockDB, ITxHashSetConstPtr pTxHashSet) final;
	std::vector<TransactionPtr> GetExpiredTransactions() const final;

	// Node API views (grin v5.5.2 foreign methods)
	size_t GetPoolSize() const final
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		return m_memPool.Size();
	}

	size_t GetStemPoolSize() const final
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		return m_stemPool.Size();
	}

	std::vector<TransactionPtr> GetUnconfirmedTransactions() const final
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		return m_memPool.GetAllTransactions();
	}

	std::vector<TransactionPtr> GetStemTransactions() const final
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		return m_stemPool.GetAllTransactions();
	}

private:
	const Config& m_config;
	mutable std::shared_mutex m_mutex;

	Pool m_memPool;
	Pool m_stemPool;
};
