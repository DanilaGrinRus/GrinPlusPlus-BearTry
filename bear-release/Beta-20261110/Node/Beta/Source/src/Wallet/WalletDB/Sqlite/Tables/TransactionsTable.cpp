#include "TransactionsTable.h"
#include "../../WalletEncryptionUtil.h"
#include "../SqliteDB.h"

#include <Common/Logger.h>
#include <Common/Util/StringUtil.h>
#include <Wallet/WalletDB/WalletStoreException.h>

// TABLE: transactions
// id: INTEGER PRIMARY KEY
// slate_id: TEXT
// encrypted: BLOB NOT NULL
void TransactionsTable::CreateTable(SqliteDB& database)
{
	const std::string table_creation_cmd = "create table transactions(id INTEGER PRIMARY KEY, slate_id TEXT, encrypted BLOB NOT NULL);";
	database.Execute(table_creation_cmd);
}

void TransactionsTable::UpdateSchema(SqliteDB& /*database*/, const SecureVector& /*masterSeed*/, const int /*previousVersion*/)
{
	return;
}

void TransactionsTable::AddTransactions(SqliteDB& database, const SecureVector& masterSeed, const std::vector<WalletTx>& transactions)
{
	for (const WalletTx& walletTx : transactions)
	{
		std::string insert_tx_cmd = "insert into transactions(id, slate_id, encrypted) values(?, ?, ?)";
		insert_tx_cmd += " ON CONFLICT(id) DO UPDATE SET slate_id=excluded.slate_id, encrypted=excluded.encrypted";

		Serializer serializer;
		walletTx.Serialize(serializer);

		// W-06: bind the row identity (wallet tx id) as associated data.
		const std::string idString = std::to_string(walletTx.GetId());
		const std::vector<uint8_t> associatedData(idString.begin(), idString.end());
		std::vector<uint8_t> encrypted = WalletEncryptionUtil::Encrypt(masterSeed, "WALLET_TX", associatedData, serializer.GetSecureBytes());

		std::vector<SqliteDB::IParameter::UPtr> parameters;
		parameters.push_back(IntParameter::New((int)walletTx.GetId()));

		if (walletTx.GetSlateId().has_value()) {
			parameters.push_back(TextParameter::New(uuids::to_string(walletTx.GetSlateId().value())));
		} else {
			parameters.push_back(NullParameter::New());
		}
		
		parameters.push_back(BlobParameter::New(encrypted));
		
		database.Update(insert_tx_cmd, parameters);
	}
}

std::vector<WalletTx> TransactionsTable::GetTransactions(SqliteDB& database, const SecureVector& masterSeed)
{
	std::string get_tx_query = "select id, encrypted from transactions";
	auto pStatement = database.Query(get_tx_query);

	std::vector<WalletTx> transactions;
	while (pStatement->Step())
	{
		const std::string idString = std::to_string(pStatement->GetColumnInt64(0));
		const std::vector<uint8_t> associatedData(idString.begin(), idString.end());
		std::vector<uint8_t> encrypted = pStatement->GetColumnBytes(1);
		SecureVector decrypted = WalletEncryptionUtil::Decrypt(masterSeed, "WALLET_TX", associatedData, encrypted);
		std::vector<uint8_t> decryptedUnsafe(decrypted.data(), decrypted.data() + decrypted.size());

		ByteBuffer byteBuffer(std::move(decryptedUnsafe));
		transactions.emplace_back(WalletTx::Deserialize(byteBuffer));
	}

	return transactions;
}

std::unique_ptr<WalletTx> TransactionsTable::GetTransactionById(SqliteDB& database, const SecureVector& masterSeed, const uint32_t walletTxId)
{
	std::string get_tx_query = "select encrypted from transactions where id=?";
	std::vector<SqliteDB::IParameter::UPtr> parameters;
	parameters.push_back(IntParameter::New(walletTxId));

	auto pStatement = database.Query(get_tx_query, parameters);
	if (!pStatement->Step()) {
		WALLET_DEBUG_F("Transaction not found with id {}", walletTxId);
		return nullptr;
	}

	const std::string idString = std::to_string(walletTxId);
	const std::vector<uint8_t> associatedData(idString.begin(), idString.end());
	std::vector<uint8_t> encrypted = pStatement->GetColumnBytes(0);
	SecureVector decrypted = WalletEncryptionUtil::Decrypt(masterSeed, "WALLET_TX", associatedData, encrypted);
	std::vector<uint8_t> decryptedUnsafe(decrypted.data(), decrypted.data() + decrypted.size());

	ByteBuffer byteBuffer(std::move(decryptedUnsafe));
	return std::make_unique<WalletTx>(WalletTx::Deserialize(byteBuffer));
}