#pragma once

#include "SqliteDB.h"

#include <Common/Util/StringUtil.h>
#include <Wallet/WalletDB/WalletStoreException.h>
#include <Common/Logger.h>

class SqliteTransaction
{
public:
	SqliteTransaction(const SqliteDB::Ptr& pDatabase)
		: m_pDatabase(pDatabase), m_open(false) { }

	~SqliteTransaction()
	{
		if (m_open) {
			Rollback();
		}
	}

	void Begin()
	{
		if (m_open) {
			WALLET_ERROR("Tried to begin a transaction twice");
			throw WALLET_STORE_EXCEPTION("Tried to begin a transaction twice.");
		}

		m_pDatabase->Execute("BEGIN;");

		m_open = true;
	}

	void Commit()
	{
		if (!m_open) {
			WALLET_ERROR("Tried to commit a closed transaction");
			throw WALLET_STORE_EXCEPTION("Tried to commit a closed transaction.");
		}

		// Keep m_open == true until COMMIT succeeds. If COMMIT throws (BUSY,
		// FULL, IOERR, ...), the SQLite transaction may still be active; the
		// destructor then performs a best-effort ROLLBACK instead of leaking an
		// open transaction into the next write.
		m_pDatabase->Execute("COMMIT;");

		m_open = false;
	}

	void Rollback() noexcept
	{
		if (!m_open) {
			WALLET_ERROR("Tried to rollback a closed transaction");
			return;
		}

		m_open = false;

		// Rollback is called from destructors and from noexcept paths. A
		// failure here must be logged, never allowed to reach std::terminate.
		try {
			m_pDatabase->Execute("ROLLBACK;");
		} catch (const std::exception& e) {
			WALLET_ERROR_F("Failed to rollback transaction: {}", e.what());
		} catch (...) {
			WALLET_ERROR("Failed to rollback transaction: unknown error");
		}
	}

	bool IsOpen() const noexcept { return m_open; }

private:
	SqliteDB::Ptr m_pDatabase;
	bool m_open;
};