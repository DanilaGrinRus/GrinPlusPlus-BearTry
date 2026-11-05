#pragma once

#include <TxPool/TransactionPool.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <json/json.h>
#include <string>
#include <vector>

// Grin v5.5.2 foreign API: get_unconfirmed_transactions.
// Returns the transactions currently held by the node as
// {"Ok": [{"src": "Broadcast"|"Stempool", "tx": <transaction>}]}.
class GetUnconfirmedTransactionsHandler : public RPCMethod
{
public:
	GetUnconfirmedTransactionsHandler(const ITransactionPool::Ptr& pTransactionPool)
		: m_pTransactionPool(pTransactionPool) { }
	~GetUnconfirmedTransactionsHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		Json::Value entries(Json::arrayValue);
		AppendEntries(entries, m_pTransactionPool->GetUnconfirmedTransactions(), "Broadcast");
		AppendEntries(entries, m_pTransactionPool->GetStemTransactions(), "Stempool");

		Json::Value result;
		result["Ok"] = entries;
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	static void AppendEntries(
		Json::Value& entries,
		const std::vector<TransactionPtr>& transactions,
		const std::string& source)
	{
		for (const TransactionPtr& pTransaction : transactions) {
			if (pTransaction == nullptr) {
				continue;
			}

			Json::Value entry;
			entry["src"] = source;
			entry["tx"] = pTransaction->ToJSON();
			entries.append(entry);
		}
	}

	ITransactionPool::Ptr m_pTransactionPool;
};
