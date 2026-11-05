#include "VersionTable.h"
#include "../Schema.h"
#include "../SqliteDB.h"
#include "OutputsTable.h"
#include "MetadataTable.h"
#include "SlateContextTable.h"
#include "SlateTable.h"
#include "AccountsTable.h"

#include <Common/Util/StringUtil.h>
#include <Common/Logger.h>
#include <Wallet/WalletDB/WalletStoreException.h>

namespace
{
	struct RequiredColumn
	{
		const char* table;
		const char* column;
	};

	// Keep this list in sync with the CreateTable methods for
	// LATEST_SCHEMA_VERSION. A database is not allowed to claim the latest
	// version until every one of these columns exists.
	const RequiredColumn REQUIRED_COLUMNS[] = {
		{ "version", "schema_version" },
		{ "outputs", "id" },
		{ "outputs", "commitment" },
		{ "outputs", "status" },
		{ "outputs", "transaction_id" },
		{ "outputs", "encrypted" },
		{ "transactions", "id" },
		{ "transactions", "slate_id" },
		{ "transactions", "encrypted" },
		{ "metadata", "id" },
		{ "metadata", "next_tx_id" },
		{ "metadata", "refresh_block_height" },
		{ "metadata", "restore_leaf_index" },
		{ "slate_contexts", "slate_id" },
		{ "slate_contexts", "iv" },
		{ "slate_contexts", "enc_context" },
		{ "slate", "slate_id" },
		{ "slate", "stage" },
		{ "slate", "iv" },
		{ "slate", "slate" },
		{ "slate", "armored_slatepack" },
		{ "accounts", "parent_path" },
		{ "accounts", "account_name" },
		{ "accounts", "next_child_index" },
		{ "accounts", "current_address_index" }
	};

	bool HasRequiredColumns(SqliteDB& database)
	{
		for (const RequiredColumn& required : REQUIRED_COLUMNS) {
			if (!database.TableHasColumn(required.table, required.column)) {
				return false;
			}
		}

		return true;
	}
}

void VersionTable::CreateTable(SqliteDB& database)
{
	std::string table_creation_cmd = "create table version(schema_version INTEGER PRIMARY KEY);";
	table_creation_cmd += StringUtil::Format("insert into version(schema_version) values ({});", LATEST_SCHEMA_VERSION);

	database.Execute(table_creation_cmd);
}

void VersionTable::Migrate(SqliteDB& database, const SecureVector& masterSeed, const int previousVersion)
{
	if (previousVersion < 0) {
		throw WALLET_STORE_EXCEPTION_F("Invalid wallet database schema version: {}", previousVersion);
	}

	if (previousVersion > LATEST_SCHEMA_VERSION) {
		throw WALLET_STORE_EXCEPTION_F(
			"Sqlite database has higher version ({}) than supported ({})",
			previousVersion,
			LATEST_SCHEMA_VERSION
		);
	}

	if (!DoesTableExist(database)) {
		// Legacy database without a version table. Create it first; the whole
		// migration is expected to run inside a transaction, so a failed
		// verification will roll this back.
		CreateTable(database);
	}

	// A stale/zero version row on an already-current schema must not trigger
	// destructive data migrations. Verify the schema first and only fix the
	// version marker if everything is already present.
	if (HasRequiredColumns(database)) {
		SetVersion(database, LATEST_SCHEMA_VERSION);
		if (GetCurrentVersion(database) != LATEST_SCHEMA_VERSION) {
			throw WALLET_STORE_EXCEPTION("Wallet database schema version was not recorded.");
		}
		return;
	}

	// Order matters: data migrations for old versions must run before the
	// schema is verified. All of these operations are idempotent when the
	// target column/table already exists.
	OutputsTable::UpdateSchema(database, masterSeed, previousVersion);
	MetadataTable::UpdateSchema(database, previousVersion);
	SlateContextTable::UpdateSchema(database, previousVersion);
	SlateTable::UpdateSchema(database, previousVersion);
	AccountsTable::UpdateSchema(database, previousVersion);

	// Never advertise LATEST_SCHEMA_VERSION for an incomplete database.
	VerifySchema(database);
	SetVersion(database, LATEST_SCHEMA_VERSION);

	const int currentVersion = GetCurrentVersion(database);
	if (currentVersion != LATEST_SCHEMA_VERSION) {
		throw WALLET_STORE_EXCEPTION_F(
			"Wallet database schema version was not recorded: {}",
			currentVersion
		);
	}
}

void VersionTable::VerifySchema(SqliteDB& database)
{
	for (const RequiredColumn& required : REQUIRED_COLUMNS) {
		if (!database.TableHasColumn(required.table, required.column)) {
			throw WALLET_STORE_EXCEPTION_F(
				"Wallet database schema is incomplete: {}.{} is missing",
				required.table,
				required.column
			);
		}
	}
}

int VersionTable::GetCurrentVersion(SqliteDB& database)
{
	if (!DoesTableExist(database)) {
		return 0;
	}

	const std::string select_version_query = "SELECT schema_version FROM version";
	auto pStatement = database.Query(select_version_query);
	if (!pStatement->Step()) {
		return 0;
	}

	return pStatement->GetColumnInt(0);
}

bool VersionTable::DoesTableExist(SqliteDB& database)
{
	return database.TableExists("version");
}

void VersionTable::SetVersion(SqliteDB& database, const int version)
{
	// The version table is expected to contain exactly one row. Normalise it
	// instead of relying on UPDATE affecting a row.
	const std::string update_version_cmd = StringUtil::Format(
		"delete from version; insert into version(schema_version) values ({});",
		version
	);
	database.Execute(update_version_cmd);
}
