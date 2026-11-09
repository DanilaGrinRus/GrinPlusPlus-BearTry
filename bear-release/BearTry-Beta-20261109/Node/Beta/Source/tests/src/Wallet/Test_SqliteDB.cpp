#include <catch.hpp>

#include <TestFileUtil.h>
#include <Wallet/WalletDB/Sqlite/SqliteDB.h>
#include <Wallet/WalletDB/Sqlite/SqliteTransaction.h>
#include <Wallet/WalletDB/WalletStoreException.h>

#include <memory>
#include <typeinfo>

TEST_CASE("SqliteDB::Step distinguishes DONE from a read error")
{
	// Two connections to the same deterministic temporary database.
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = SqliteDB::Open(pTempFile->GetPath(), "test");
	pDatabase->Execute(
		"create table t(id INTEGER PRIMARY KEY, value TEXT NOT NULL);"
		"insert into t(id, value) values(1, 'a');"
	);

	// DONE (no matching row) must not throw.
	auto pDone = pDatabase->Query("select value from t where id=2");
	REQUIRE_FALSE(pDone->Step());
	pDone->Finalize();

	// Prepare a read before taking an exclusive lock; then force SQLITE_BUSY
	// during Step().
	auto pRead = pDatabase->Query("select value from t");
	auto pLocker = SqliteDB::Open(pTempFile->GetPath(), "locker");
	pLocker->Execute("BEGIN EXCLUSIVE;");

	REQUIRE_THROWS_AS(pRead->Step(), WalletStoreException);

	pLocker->Execute("ROLLBACK;");
}

TEST_CASE("SqliteDB query parameters stay valid after Query returns")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = SqliteDB::Open(pTempFile->GetPath(), "test");
	pDatabase->Execute(
		"create table t(id INTEGER PRIMARY KEY, value TEXT NOT NULL);"
		"insert into t(id, value) values(1, 'needle');"
	);

	std::vector<SqliteDB::IParameter::UPtr> parameters;
	parameters.push_back(TextParameter::New("needle"));
	auto pStatement = pDatabase->Query("select id from t where value=?", parameters);

	// Query() owns its parameter objects only during the call; binding must be
	// SQLITE_TRANSIENT (copy), otherwise this Step() reads freed memory.
	REQUIRE(pStatement->Step());
	REQUIRE(pStatement->GetColumnInt(0) == 1);
}

TEST_CASE("SqliteTransaction recovers after a failed COMMIT")
{
	auto pTempFile = TestFileUtil::CreateTempFile();
	auto pDatabase = SqliteDB::Open(pTempFile->GetPath(), "test");
	pDatabase->Execute(
		"create table t(id INTEGER PRIMARY KEY, value TEXT NOT NULL);"
		"insert into t(id, value) values(1, 'a');"
	);

	auto pOther = SqliteDB::Open(pTempFile->GetPath(), "other");

	// Hold a SHARED lock while this connection tries to COMMIT a write.
	auto pRead = pOther->Query("select count(*) from t");
	REQUIRE(pRead->Step());
	REQUIRE(pRead->GetColumnInt(0) == 1);

	auto pTransaction = std::make_unique<SqliteTransaction>(pDatabase);
	pTransaction->Begin();
	pDatabase->Update("insert into t(id, value) values(2, 'b')");

	bool commitRejected = false;
	try
	{
		pTransaction->Commit();
	}
	catch (const WalletStoreException&)
	{
		commitRejected = true;
	}
	catch (const std::exception& e)
	{
		FAIL("Unexpected exception type from COMMIT: " << typeid(e).name() << " / " << e.what());
	}
	REQUIRE(commitRejected);

	// Release the reader after the failed COMMIT, then destroy the
	// transaction. After the fix the destructor rolls the still-open SQLite
	// transaction back; before the fix it was a no-op and the next BEGIN failed.
	pRead.reset();
	pTransaction.reset();

	SqliteTransaction nextTransaction(pDatabase);
	REQUIRE_NOTHROW(nextTransaction.Begin());
	nextTransaction.Commit();
}
