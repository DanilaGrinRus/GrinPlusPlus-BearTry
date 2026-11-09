#pragma once

#include <Common/Secure.h>

// Forward Declarations
class SqliteDB;

class VersionTable
{
public:
	static void CreateTable(SqliteDB& database);

	// Runs all table migrations for a database whose actual schema version is
	// previousVersion, then verifies the resulting schema and only afterwards
	// records LATEST_SCHEMA_VERSION. Must be called inside a transaction.
	static void Migrate(SqliteDB& database, const SecureVector& masterSeed, const int previousVersion);

	// Throws if any table/column required by LATEST_SCHEMA_VERSION is missing.
	static void VerifySchema(SqliteDB& database);

	static int GetCurrentVersion(SqliteDB& database);

private:
	static bool DoesTableExist(SqliteDB& database);
	static void SetVersion(SqliteDB& database, const int version);
};
