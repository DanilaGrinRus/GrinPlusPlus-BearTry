#include "SqliteDB.h"

#include <libsqlite3/sqlite3.h>
#include <cctype>
#include <Common/Logger.h>
#include <Common/Util/FileUtil.h>
#include <Common/Util/HexUtil.h>
#include <Wallet/WalletDB/WalletStoreException.h>

/**
 * 
 * SqliteDB::Statement
 * 
 */

SqliteDB::Statement::~Statement()
{
	if (!m_finalized && sqlite3_finalize(m_pStatement) != SQLITE_OK) {
		WALLET_ERROR_F("Error finalizing statement: {}", sqlite3_errmsg(m_pDatabase));
    }
}

bool SqliteDB::Statement::Step()
{
	const int result = sqlite3_step(m_pStatement);
	if (result == SQLITE_ROW) {
		return true;
	}

	if (result == SQLITE_DONE) {
		return false;
	}

	WALLET_ERROR_F(
		"Error stepping statement ({}): {}",
		result,
		sqlite3_errmsg(m_pDatabase)
	);
	throw WALLET_STORE_EXCEPTION_F(
		"Error stepping statement ({}): {}",
		result,
		sqlite3_errmsg(m_pDatabase)
	);
}

void SqliteDB::Statement::Finalize()
{
    m_finalized = true;
	if (sqlite3_finalize(m_pStatement) != SQLITE_OK) {
		WALLET_ERROR_F("Error finalizing statement: {}", sqlite3_errmsg(m_pDatabase));
		throw WALLET_STORE_EXCEPTION("Error finalizing statement.");
	}
}

bool SqliteDB::Statement::IsColumnNull(const int col) const
{
	return sqlite3_column_type(m_pStatement, col) == SQLITE_NULL;
}

int SqliteDB::Statement::GetColumnInt(const int col) const
{
    return sqlite3_column_int(m_pStatement, col);
}

int64_t SqliteDB::Statement::GetColumnInt64(const int col) const
{
    return sqlite3_column_int64(m_pStatement, col);
}

std::vector<uint8_t> SqliteDB::Statement::GetColumnBytes(const int col) const
{
    const int size = sqlite3_column_bytes(m_pStatement, col);
    const unsigned char* pBytes = (const unsigned char*)sqlite3_column_blob(m_pStatement, col);
    if (size <= 0 || pBytes == nullptr) {
        return std::vector<uint8_t>{};
    }

    return std::vector<uint8_t>{ pBytes, pBytes + size };
}

std::string SqliteDB::Statement::GetColumnString(const int col) const
{
	const unsigned char* pText = sqlite3_column_text(m_pStatement, col);
	if (pText == nullptr) {
		return std::string{};
	}

	return std::string((const char*)pText);
}

/**
 * 
 * SqliteDB
 * 
 */

SqliteDB::~SqliteDB()
{
	sqlite3_close(m_pDatabase);
}

SqliteDB::Ptr SqliteDB::Open(const fs::path& db_path, const std::string& username)
{
	sqlite3* pDatabase = nullptr;

    if (sqlite3_open(db_path.u8string().c_str(), &pDatabase) != SQLITE_OK) {
        WALLET_ERROR_F("Failed to open wallet.db for user ({}) with error ({})", username, sqlite3_errmsg(pDatabase));
        sqlite3_close(pDatabase);
        throw WALLET_STORE_EXCEPTION("Failed to open wallet.db");
    }

    return std::make_shared<SqliteDB>(pDatabase, username);
}

void SqliteDB::Execute(const std::string& command)
{
	char* error = nullptr;
	if (sqlite3_exec(m_pDatabase, command.c_str(), NULL, NULL, &error) != SQLITE_OK) {
		WALLET_ERROR_F("Failed to execute command: {}. Error: {}", command, error);
		sqlite3_free(error);
		throw WALLET_STORE_EXCEPTION("Failed to execute command.");
	}
}

void SqliteDB::Update(const std::string& statement, const std::vector<IParameter::UPtr>& parameters)
{
	sqlite3_stmt* stmt = nullptr;
	if (sqlite3_prepare_v2(m_pDatabase, statement.c_str(), -1, &stmt, NULL) != SQLITE_OK) {
		WALLET_ERROR_F("Error while compiling sql: {}", sqlite3_errmsg(m_pDatabase));
		sqlite3_finalize(stmt);
		throw WALLET_STORE_EXCEPTION("Error compiling statement.");
	}

    SqliteDB::Statement::UPtr pStatement = std::make_unique<SqliteDB::Statement>(m_pDatabase, stmt);

    int index = 1;
    for (const auto& pParam : parameters)
    {
        pParam->Bind(stmt, index++);
    }

    pStatement->Step();
    pStatement->Finalize();
}

SqliteDB::Statement::UPtr SqliteDB::Query(const std::string& query, const std::vector<IParameter::UPtr>& parameters)
{
	sqlite3_stmt* stmt = nullptr;
	if (sqlite3_prepare_v2(m_pDatabase, query.c_str(), -1, &stmt, NULL) != SQLITE_OK) {
		WALLET_ERROR_F("Error while compiling sql: {}", sqlite3_errmsg(m_pDatabase));
		sqlite3_finalize(stmt);
		throw WALLET_STORE_EXCEPTION("Error compiling statement.");
	}

    auto statement = std::make_unique<Statement>(m_pDatabase, stmt);
    int index = 1;
    for (const auto& pParam : parameters) {
        pParam->Bind(stmt, index++);
    }
    return statement;
}

std::string SqliteDB::GetError() const
{
    return std::string(sqlite3_errmsg(m_pDatabase));
}

namespace
{
	bool IsSimpleIdentifier(const std::string& identifier)
	{
		if (identifier.empty()) {
			return false;
		}

		for (const char c : identifier) {
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
				return false;
			}
		}

		return true;
	}
}

bool SqliteDB::TableExists(const std::string& table)
{
	std::vector<IParameter::UPtr> parameters;
	parameters.push_back(TextParameter::New(table));

	auto pStatement = Query(
		"SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
		parameters
	);

	return pStatement->Step();
}

bool SqliteDB::TableHasColumn(const std::string& table, const std::string& column)
{
	if (!IsSimpleIdentifier(table) || !IsSimpleIdentifier(column)) {
		throw WALLET_STORE_EXCEPTION("Invalid schema identifier.");
	}

	// PRAGMA statements do not support parameter binding. table/column are
	// restricted to simple identifiers above, which makes concatenation safe.
	auto pStatement = Query("PRAGMA table_info(" + table + ")");
	while (pStatement->Step()) {
		if (pStatement->GetColumnString(1) == column) {
			return true;
		}
	}

	return false;
}

/**
 * 
 * SqliteDB::IParameter
 * 
 */

void TextParameter::Bind(sqlite3_stmt* stmt, const int index) const
{
	// SQLITE_TRANSIENT lets SQLite own a copy. Bind() is used for both Update()
	// (parameters outlive the statement) and Query() (parameters are destroyed
	// before the caller calls Step()), so SQLITE_STATIC would dangle.
	if (sqlite3_bind_text(stmt, index, m_value.c_str(), (int)m_value.size(), SQLITE_TRANSIENT) != SQLITE_OK) {
		WALLET_ERROR_F("Failed to bind: {}", m_value);
		throw WALLET_STORE_EXCEPTION("Failed to bind.");
    }
}

void IntParameter::Bind(sqlite3_stmt* stmt, const int index) const
{
	if (sqlite3_bind_int(stmt, index, m_value) != SQLITE_OK) {
		WALLET_ERROR_F("Failed to bind: {}", m_value);
		throw WALLET_STORE_EXCEPTION("Failed to bind.");
    }
}

void BlobParameter::Bind(sqlite3_stmt* stmt, const int index) const
{
	if (sqlite3_bind_blob(stmt, index, (const void*)m_blob.data(), (int)m_blob.size(), SQLITE_TRANSIENT) != SQLITE_OK) {
		WALLET_ERROR_F("Failed to bind: {}", HexUtil::ConvertToHex(m_blob));
		throw WALLET_STORE_EXCEPTION("Failed to bind.");
    }
}

void NullParameter::Bind(sqlite3_stmt* stmt, const int index) const
{
	if (sqlite3_bind_null(stmt, index) != SQLITE_OK) {
		WALLET_ERROR("Failed to bind null");
		throw WALLET_STORE_EXCEPTION("Failed to bind.");
    }
}