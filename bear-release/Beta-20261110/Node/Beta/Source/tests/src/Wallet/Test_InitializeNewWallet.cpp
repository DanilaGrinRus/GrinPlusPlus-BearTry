#include <catch.hpp>

#include <TestServer.h>
#include <TorProcessManager.h>

#include <Wallet/WalletManager.h>
#include <uuid.h>
#include <Wallet/WalletDB/Sqlite/SqliteTransaction.h>
#include <Wallet/SlateBuilder/CoinSelection.h>
#include <Wallet/Exceptions/SessionTokenException.h>
#include <Wallet/Exceptions/InsufficientFundsException.h>


TEST_CASE("Wallet Creation/Deletion")
{
	auto pServer = TestServer::CreateWithWallet();

	// Create wallet
	const std::string username = uuids::to_string(uuids::uuid_system_generator()());
	auto created_user = pServer->CreateUser(username, "Password1", UseTor::NO);

	// Validate seed words
	SecureString seedWords = created_user.wallet->GetSeedWords();
	REQUIRE(created_user.seed_words == seedWords);

	const auto token = created_user.wallet->GetToken();
	const SessionToken truncated(token.GetSessionId(), std::vector<unsigned char>{});
	auto wrongKey = token.GetTokenKey();
	wrongKey.back() ^= 1;
	const SessionToken forged(token.GetSessionId(), wrongKey);
	const auto manager = pServer->GetWalletManager();
	REQUIRE_THROWS_AS(manager->GetWallet(truncated), SessionTokenException);
	REQUIRE_THROWS_AS(manager->Logout(forged), SessionTokenException);
	REQUIRE_NOTHROW(manager->GetWallet(token));

	created_user.wallet->Logout();

	// Delete wallet
	pServer->GetWalletManager()->DeleteWallet(username, "Password1");
}

TEST_CASE("Wallet Words Length")
{
	auto pServer = TestServer::CreateWithWallet();

	// Create wallet
	std::vector<uint8_t> numWordsVec = { 12, 15, 18, 21, 24 };
	for (uint8_t numWords : numWordsVec)
	{
		const std::string username = uuids::to_string(uuids::uuid_system_generator()());
		auto created_user = pServer->CreateUser(username, "Password1", UseTor::NO, (SeedWordSize)numWords);

		// Validate seed words
		SecureString seedWords = created_user.wallet->GetSeedWords();
		REQUIRE(created_user.seed_words == seedWords);
		REQUIRE(StringUtil::Split((const std::string&)seedWords, " ").size() == numWords);

		// Logout
		created_user.wallet->Logout();

		// Delete wallet
		pServer->GetWalletManager()->DeleteWallet(username, "Password1");
	}
}
TEST_CASE("Wallet SQLite reports failed steps and rolls back failed commits")
{
	auto db = SqliteDB::Open(":memory:", "audit");
	db->Execute("PRAGMA foreign_keys=ON");
	db->Execute("CREATE TABLE parent(id INTEGER PRIMARY KEY)");
	db->Execute("CREATE TABLE child(pid REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED)");
	{
		SqliteTransaction tx(db);
		tx.Begin();
		db->Execute("INSERT INTO child VALUES(42)");
		REQUIRE_THROWS(tx.Commit());
	}
	auto count = db->Query("SELECT COUNT(*) FROM child");
	REQUIRE(count->Step());
	REQUIRE(count->GetColumnInt(0) == 0);
	auto invalid = db->Query("SELECT abs(-9223372036854775808)");
	REQUIRE_THROWS(invalid->Step());
}

TEST_CASE("Wallet ALL rejects insufficient funds before building change")
{
	REQUIRE_THROWS_AS(CoinSelection::SelectCoinsToSpend({}, 1, 1, ESelectionStrategy::ALL, {}, 1, 1),
		InsufficientFundsException);
}
