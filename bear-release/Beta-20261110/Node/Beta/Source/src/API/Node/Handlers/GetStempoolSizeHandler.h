#pragma once

#include <TxPool/TransactionPool.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>

// Grin v5.5.2 foreign API: get_stempool_size.
// Returns the number of transactions in the stem pool as {"Ok": n}.
class GetStempoolSizeHandler : public RPCMethod
{
public:
	GetStempoolSizeHandler(const ITransactionPool::Ptr& pTransactionPool)
		: m_pTransactionPool(pTransactionPool) { }
	~GetStempoolSizeHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		Json::Value result;
		result["Ok"] = Json::UInt64(m_pTransactionPool->GetStemPoolSize());
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	ITransactionPool::Ptr m_pTransactionPool;
};
