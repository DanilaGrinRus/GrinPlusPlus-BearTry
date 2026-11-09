#pragma once

#include <Database/BlockDb.h>
#include <Core/Traits/Lockable.h>

#include <ctime>
#include <filesystem.h>
#include <string>
#include <system_error>

// Before a schema migration the node must keep the previous database. The DB is
// open (RocksDB) while this runs, so the backup MUST be a consistent snapshot,
// not a file copy: a recursive copy of a live RocksDB directory can capture a
// torn LOCK/SST/MANIFEST combination and is not guaranteed to reopen.
//
// The chain database is checkpointed with RocksDB's Checkpoint API (immutable
// SST files + atomic manifest/WAL capture). The resulting directory is a
// standalone RocksDB that can be reopened as a CHAIN database.
//
// The peer database under DB/PEERS is an ephemeral cache (PeerDB::OpenDB wipes
// and recreates it on every start) and is intentionally not part of the backup.
//
// A failed checkpoint returns an empty path and callers must abort the
// migration rather than destroying the only copy.
namespace DbBackup
{
	// Checkpoints the open chain database into backupPath (which must not exist).
	// Returns an empty path on failure.
	inline fs::path BackupTo(
		const std::shared_ptr<Locked<IBlockDB>>& pBlockDB,
		const fs::path& backupPath)
	{
		if (pBlockDB == nullptr) {
			return fs::path();
		}

		std::error_code ec;
		if (fs::exists(backupPath, ec)) {
			return fs::path();
		}

		if (!pBlockDB->Read()->CreateCheckpoint(backupPath)) {
			return fs::path();
		}

		return backupPath;
	}

	// Returns the backup path, or an empty path when the backup failed.
	inline fs::path BackupBeforeMigration(
		const std::shared_ptr<Locked<IBlockDB>>& pBlockDB,
		const fs::path& databasePath,
		const uint32_t currentVersion)
	{
		if (pBlockDB == nullptr) {
			return fs::path();
		}

		const std::string suffix =
			"v" + std::to_string(currentVersion) + "-" +
			std::to_string((int64_t)std::time(nullptr));
		const fs::path backupPath = databasePath.parent_path() / ("DB.backup-" + suffix);

		return BackupTo(pBlockDB, backupPath);
	}
}
