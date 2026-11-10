#include "SqliteStore.h"
#include "WalletSqlite.h"
#include "Schema.h"
#include "Tables/VersionTable.h"
#include "Tables/OutputsTable.h"
#include "Tables/TransactionsTable.h"
#include "Tables/MetadataTable.h"
#include "Tables/SlateContextTable.h"
#include "Tables/SlateTable.h"
#include "Tables/AccountsTable.h"

#include <Wallet/WalletDB/WalletStoreException.h>
#include <Common/Util/FileUtil.h>
#include <Common/Util/StringUtil.h>
#include <Common/Logger.h>

#include <system_error>
#include <vector>
#include <ctime>

static const uint8_t ENCRYPTION_FORMAT = 0;

namespace
{
	// Returns true when the directory contained only leftovers from an
	// interrupted wallet create (and they were removed). Anything unknown is
	// left untouched.
	bool CleanupInterruptedCreateArtifacts(const fs::path& directory)
	{
		std::vector<fs::path> artifacts;
		std::error_code ec;
		for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
			if (!fs::is_regular_file(it->path())) {
				return false;
			}

			const std::string name = it->path().filename().u8string();
			if (name == "seed.json.tmp"
				|| name == "wallet.db-journal"
				|| name == "wallet.db-wal"
				|| name == "wallet.db-shm") {
				artifacts.push_back(it->path());
			} else {
				return false;
			}
		}

		if (ec) {
			return false;
		}

		for (const fs::path& artifact : artifacts) {
			FileUtil::RemoveFile(artifact);
		}

		return true;
	}

	// W2.3: wallet artifacts must not inherit broad directory ACLs. Best-effort
	// on open (existing profiles), but the result is logged for the security gate.
	void HardenWalletPath(const fs::path& path)
	{
		if (!FileUtil::HardenPermissions(path)) {
			WALLET_WARNING_F("Failed to harden wallet permissions for {}", path.u8string());
		}
	}
}

std::shared_ptr<SqliteStore> SqliteStore::Open(const Config& config)
{
	// SqliteStore owns a std::mutex, so it is neither copyable nor movable.
	// Construct it in place instead of make_shared<T>(T(...)).
	return std::shared_ptr<SqliteStore>(new SqliteStore(config.GetWalletPath()));
}

namespace
{
	int CountRows(SqliteDB& database, const std::string& sql)
	{
		auto pStatement = database.Query(sql);
		if (!pStatement->Step()) {
			return 0;
		}
		return pStatement->GetColumnInt(0);
	}
}

bool SqliteStore::HasLegacyEncryption(SqliteDB& database)
{
	// Format byte 2 = row-bound authenticated format. Anything else is legacy.
	const bool legacyOutputs = CountRows(
		database,
		"select count(*) from outputs where length(encrypted)>0 and substr(encrypted,1,1)<>X'02'"
	) > 0;
	const bool legacyTransactions = CountRows(
		database,
		"select count(*) from transactions where length(encrypted)>0 and substr(encrypted,1,1)<>X'02'"
	) > 0;
	const bool legacySlates = CountRows(database, "select count(*) from slate where enc_format=0") > 0;
	const bool legacyContexts = CountRows(database, "select count(*) from slate_contexts where enc_format=0") > 0;
	return legacyOutputs || legacyTransactions || legacySlates || legacyContexts;
}

bool SqliteStore::MigrateLegacyEncryption(SqliteDB& database, const SecureVector& masterSeed)
{
	if (!HasLegacyEncryption(database)) {
		return false;
	}

	const auto outputs = OutputsTable::GetOutputs(database, masterSeed);
	if (!outputs.empty()) {
		OutputsTable::AddOutputs(database, masterSeed, outputs);
	}

	const auto transactions = TransactionsTable::GetTransactions(database, masterSeed);
	if (!transactions.empty()) {
		TransactionsTable::AddTransactions(database, masterSeed, transactions);
	}

	SlateTable::MigrateLegacyRows(database, masterSeed);
	SlateContextTable::MigrateLegacyRows(database, masterSeed);

	return true;
}

fs::path SqliteStore::GetDBFile(const std::string& username) const
{
	return m_walletDirectory / StringUtil::ToWide(username) / "wallet.db";
}

std::vector<GrinStr> SqliteStore::GetAccounts() const
{
	return FileUtil::GetSubDirectories(m_walletDirectory, false);
}

Locked<IWalletDB> SqliteStore::OpenWallet(const std::string& username, const SecureVector& masterSeed)
{
	std::lock_guard<std::mutex> lock(m_userDBsMutex);

	auto iter = m_userDBs.find(username);
	if (iter != m_userDBs.end()) {
		WALLET_DEBUG_F("wallet.db already open for user: {}", username);
		return iter->second;
	}

	const fs::path dbFile = GetDBFile(username);
	if (!FileUtil::Exists(dbFile)) {
		throw WALLET_STORE_EXCEPTION("Wallet does not exist.");
	}
	
	SqliteDB::Ptr pDatabase = SqliteDB::Open(dbFile, username);

	const int version = VersionTable::GetCurrentVersion(*pDatabase);
	
	if (version < LATEST_SCHEMA_VERSION) {
		SqliteTransaction transaction(pDatabase);
		transaction.Begin();

		// Migrate() verifies the resulting schema before recording the latest
		// version, so an incomplete database is never advertised as v4.
		VersionTable::Migrate(*pDatabase, masterSeed, version);

		transaction.Commit();
	} else if (version > LATEST_SCHEMA_VERSION) {
		throw WALLET_STORE_EXCEPTION_F("Sqlite database has higher version ({}) than supported ({})", version, LATEST_SCHEMA_VERSION);
	}

	// Existing profiles may predate W2.3; tighten them on open as well.
	const fs::path userDBPath = m_walletDirectory / username;
	const fs::path seedFile = userDBPath / "seed.json";
	HardenWalletPath(userDBPath);
	HardenWalletPath(seedFile);
	HardenWalletPath(dbFile);

	// W-06 sweep: upgrade legacy format 0/1 rows to format 2. Back up the DB
	// first; failures leave the wallet open (legacy rows stay readable) and are
	// retried on the next open.
	if (HasLegacyEncryption(*pDatabase)) {
		const fs::path backup = dbFile.string() + ".pre-format2-" + std::to_string(std::time(nullptr)) + ".bak";
		std::error_code ec;
		fs::copy_file(dbFile, backup, ec);
		if (ec) {
			WALLET_WARNING_F("W-06 sweep postponed: failed to back up wallet.db ({}).", ec.message());
		}
		else {
			HardenWalletPath(backup);
			try {
				SqliteTransaction transaction(pDatabase);
				transaction.Begin();
				SqliteStore::MigrateLegacyEncryption(*pDatabase, masterSeed);
				transaction.Commit();
				WALLET_INFO_F("W-06 sweep upgraded legacy wallet rows; backup: {}", backup.u8string());
			}
			catch (const std::exception& e) {
				WALLET_ERROR_F("W-06 sweep failed; legacy rows preserved: {}", e.what());
			}
		}
	}

	Locked<IWalletDB> walletDB(std::shared_ptr<IWalletDB>(new WalletSqlite(m_walletDirectory, username, pDatabase)));
	m_userDBs.insert(std::make_pair(username, walletDB));
	return walletDB;
}

void SqliteStore::CloseWallet(const std::string& username)
{
	std::lock_guard<std::mutex> lock(m_userDBsMutex);

	auto iter = m_userDBs.find(username);
	if (iter != m_userDBs.end()) {
		WALLET_DEBUG_F("Closing wallet.db for user: {}", username);
		m_userDBs.erase(iter);
	}
}

Locked<IWalletDB> SqliteStore::CreateWallet(const std::string& username, const EncryptedSeed& encryptedSeed)
{
	std::lock_guard<std::mutex> lock(m_userDBsMutex);

	const fs::path userDBPath = m_walletDirectory / username;
	const fs::path seedFile = userDBPath / "seed.json";
	const fs::path walletDBFile = GetDBFile(username);

	if (FileUtil::Exists(seedFile)) {
		if (!FileUtil::Exists(walletDBFile)) {
			WALLET_ERROR_F("Wallet seed exists without wallet.db for user: {}", username);
			throw WALLET_STORE_EXCEPTION(
				"Wallet seed exists without wallet database; manual recovery required."
			);
		}

		WALLET_WARNING("Wallet already exists for user: " + username);
		throw WALLET_STORE_EXCEPTION("Wallet already exists.");
	}

	if (FileUtil::Exists(userDBPath)) {
		// A partial create may have left a directory behind. Never delete a
		// directory that could belong to a real wallet (seed/DB), and never
		// follow a reparse point.
		if (fs::is_symlink(userDBPath)) {
			throw WALLET_STORE_EXCEPTION("Wallet directory is a reparse point; refusing to overwrite.");
		}

		if (FileUtil::Exists(walletDBFile)) {
			WALLET_ERROR_F("Wallet database exists without seed.json for user: {}", username);
			throw WALLET_STORE_EXCEPTION(
				"Wallet database exists without a seed file; manual recovery required."
			);
		}

		std::error_code ec;
		const bool empty = fs::is_empty(userDBPath, ec);
		if (ec || (!empty && !CleanupInterruptedCreateArtifacts(userDBPath))) {
			WALLET_ERROR_F("Wallet directory is not empty for user: {}", username);
			throw WALLET_STORE_EXCEPTION("Wallet directory is not empty; refusing to overwrite.");
		}

		if (!FileUtil::RemoveFile(userDBPath)) {
			throw WALLET_STORE_EXCEPTION("Failed to clean up incomplete wallet directory.");
		}
	}

	if (!FileUtil::CreateDirectories(userDBPath)) {
		WALLET_ERROR_F("Failed to create wallet directory for user: {}", username);
		throw WALLET_STORE_EXCEPTION("Failed to create directory.");
	}
	HardenWalletPath(userDBPath);

	bool seedWritten = false;
	try
	{
		const std::string seedJSON = encryptedSeed.ToJSON().toStyledString();
		const std::vector<unsigned char> seedBytes(seedJSON.data(), seedJSON.data() + seedJSON.size());
		FileUtil::SafeWriteToFile(seedFile, seedBytes);
		seedWritten = true;

		// SafeWriteToFile does not report write/close failures. Verify the
		// contents before creating a database around a potentially truncated
		// seed file.
		std::vector<unsigned char> seedBytesRead;
		if (!FileUtil::ReadFile(seedFile, seedBytesRead) || seedBytesRead != seedBytes) {
			throw WALLET_STORE_EXCEPTION("Failed to verify seed.json after write.");
		}
		HardenWalletPath(seedFile);

		SqliteDB::Ptr pDatabase = CreateWalletDB(username);
		HardenWalletPath(walletDBFile);

		Locked<IWalletDB> walletDB(std::shared_ptr<IWalletDB>(new WalletSqlite(m_walletDirectory, username, pDatabase)));
		m_userDBs.insert(std::make_pair(username, walletDB));
		return walletDB;
	}
	catch (...)
	{
		// Only remove artifacts belonging to this attempt. A pre-existing seed
		// is rejected above, so seedFile here was written by this call.
		WALLET_ERROR_F("Failed to create wallet for user: {}; cleaning up partial files", username);
		if (seedWritten) {
			FileUtil::RemoveFile(seedFile);
		}

		const auto removeArtifact = [](const fs::path& base, const std::string& suffix) {
			fs::path artifact = base;
			artifact += suffix;
			FileUtil::RemoveFile(artifact);
		};

		removeArtifact(seedFile, ".tmp");
		removeArtifact(walletDBFile, "");
		removeArtifact(walletDBFile, "-journal");
		removeArtifact(walletDBFile, "-wal");
		removeArtifact(walletDBFile, "-shm");

		std::error_code ec;
		if (fs::is_empty(userDBPath, ec) && !ec) {
			FileUtil::RemoveFile(userDBPath);
		}

		throw;
	}
}

void SqliteStore::DeleteWallet(const std::string& username)
{
	std::lock_guard<std::mutex> lock(m_userDBsMutex);

	if (m_userDBs.find(username) != m_userDBs.end()) {
		WALLET_ERROR_F("User '{}' still logged in. Can't delete wallet.", username);
		throw WALLET_STORE_EXCEPTION("User still logged in.");
	}

	const fs::path userDBPath = m_walletDirectory / username;
	if (!FileUtil::Exists(userDBPath)) {
		WALLET_ERROR_F("No wallet directory found for user: {}", username);
		throw WALLET_STORE_EXCEPTION("Wallet doesn't exist.");
	}

	if (!FileUtil::RemoveFile(userDBPath)) {
		WALLET_ERROR_F("Failed to delete wallet for user: {}", username);
		throw WALLET_STORE_EXCEPTION("Failed to delete wallet.");
	}
}

void SqliteStore::ChangePassword(const std::string& username, const EncryptedSeed& encryptedSeed)
{
	// W-10 residual: serialize password changes with the wallet DB registry and
	// verify the written seed file. A concurrent open/login must not observe a
	// half-written seed.json, and a failed write must not be reported as success.
	std::lock_guard<std::mutex> lock(m_userDBsMutex);

	const fs::path userDBPath = m_walletDirectory / username;
	const fs::path seedFile = userDBPath / "seed.json";

	const std::string seedJSON = encryptedSeed.ToJSON().toStyledString();
	const std::vector<unsigned char> seedBytes(seedJSON.data(), seedJSON.data() + seedJSON.size());
	FileUtil::SafeWriteToFile(seedFile, seedBytes);

	const EncryptedSeed stored = LoadWalletSeed(username);
	if (stored.ToJSON().toStyledString() != seedJSON) {
		WALLET_ERROR_F("Password change read-back mismatch for user: {}", username);
		throw WALLET_STORE_EXCEPTION("Password change read-back mismatch.");
	}
	HardenWalletPath(seedFile);
}

SqliteDB::Ptr SqliteStore::CreateWalletDB(const std::string& username)
{
	const fs::path walletDBFile = GetDBFile(username);

	try
	{
		// TODO: Add indices

		SqliteDB::Ptr pDatabase = SqliteDB::Open(walletDBFile, username);

		SqliteTransaction transaction(pDatabase);
		
		transaction.Begin();

		VersionTable::CreateTable(*pDatabase);
		OutputsTable::CreateTable(*pDatabase);
		TransactionsTable::CreateTable(*pDatabase);
		MetadataTable::CreateTable(*pDatabase);
		SlateContextTable::CreateTable(*pDatabase);
		SlateTable::CreateTable(*pDatabase);
		AccountsTable::CreateTable(*pDatabase);

		KeyChainPath nextChildPath = KeyChainPath::FromString("m/0/0").GetRandomChild();
		std::string table_creation_cmd = StringUtil::Format("insert into accounts values('m/0/0','DEFAULT',{}, 0);", nextChildPath.GetKeyIndices().back());

		pDatabase->Execute(table_creation_cmd);

		// A newly created wallet must already be at LATEST_SCHEMA_VERSION and
		// contain every required column. Otherwise remove it and fail instead
		// of producing a wallet that only fails on its first reopen.
		VersionTable::VerifySchema(*pDatabase);
		if (VersionTable::GetCurrentVersion(*pDatabase) != LATEST_SCHEMA_VERSION) {
			throw WALLET_STORE_EXCEPTION_F(
				"New wallet database was created with schema version {}, expected {}",
				VersionTable::GetCurrentVersion(*pDatabase),
				LATEST_SCHEMA_VERSION
			);
		}

		transaction.Commit();
		
		return pDatabase;
	}
	catch (WalletStoreException&)
	{
		FileUtil::RemoveFile(walletDBFile);
		throw;
	}
}

EncryptedSeed SqliteStore::LoadWalletSeed(const std::string& username) const
{
	const auto wideUsername = StringUtil::ToWide(username);

	WALLET_TRACE_F("Loading wallet seed for {}", wideUsername);

	const fs::path seedPath = m_walletDirectory / wideUsername / "seed.json";
	if (!FileUtil::Exists(seedPath)) {
		WALLET_WARNING_F("Wallet not found for user: {}", wideUsername);
		throw WALLET_STORE_EXCEPTION("Wallet not found.");
	}

	std::vector<unsigned char> seedBytes;
	if (!FileUtil::ReadFile(seedPath, seedBytes)) {
		WALLET_ERROR_F("Failed to load seed.json for user: {}", wideUsername);
		throw WALLET_STORE_EXCEPTION("Failed to load seed.json");
	}

	std::string seed(seedBytes.data(), seedBytes.data() + seedBytes.size());

	Json::Value seedJSON;
	if (!JsonUtil::Parse(seed, seedJSON)) {
		WALLET_ERROR_F("Failed to deserialize seed.json for user: {}", wideUsername);
		throw WALLET_STORE_EXCEPTION("Failed to deserialize seed.json");
	}

	return EncryptedSeed::FromJSON(seedJSON);
}