#pragma once

#include <chrono>
#include <ctime>
#include <filesystem.h>
#include <fstream>
#include <string>
#include <system_error>

// Before a schema migration the node must keep the previous database. The DB is
// open (RocksDB) while this runs, so files are copied as streams and the LOCK
// file is skipped (it is recreated on the next open). A failed backup aborts
// the migration rather than silently destroying the only copy.
namespace DbBackup
{
	inline bool CopyFileContents(const fs::path& source, const fs::path& target)
	{
		std::ifstream input(source, std::ios::binary);
		if (!input.is_open()) {
			return false;
		}

		std::ofstream output(target, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) {
			return false;
		}

		output << input.rdbuf();
		output.flush();
		const bool ok = input.good() && output.good();
		input.close();
		output.close();
		return ok;
	}

	inline bool CopyDirectory(const fs::path& source, const fs::path& target)
	{
		std::error_code ec;
		if (!fs::exists(source, ec) || !fs::is_directory(source, ec)) {
			return false;
		}

		fs::create_directories(target, ec);
		if (ec) {
			return false;
		}

		for (const auto& entry : fs::directory_iterator(source, ec)) {
			if (ec) {
				return false;
			}

			const std::string name = entry.path().filename().string();
			if (name == "LOCK") {
				continue;
			}

			const fs::path destination = target / name;
			if (entry.is_directory()) {
				if (!CopyDirectory(entry.path(), destination)) {
					return false;
				}
			} else if (entry.is_regular_file()) {
				if (!CopyFileContents(entry.path(), destination)) {
					return false;
				}
			}
		}

		return true;
	}

	// Returns the backup path, or an empty path when the backup failed.
	inline fs::path BackupBeforeMigration(
		const fs::path& databasePath,
		const uint32_t currentVersion)
	{
		const std::string suffix =
			"v" + std::to_string(currentVersion) + "-" +
			std::to_string((int64_t)std::time(nullptr));
		const fs::path backupPath = databasePath.parent_path() / ("DB.backup-" + suffix);

		if (!CopyDirectory(databasePath, backupPath)) {
			return fs::path();
		}

		return backupPath;
	}
}
