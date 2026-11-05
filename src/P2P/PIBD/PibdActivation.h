#pragma once

#include <Common/Util/FileUtil.h>
#include <Common/Util/HexUtil.h>
#include <Core/Exceptions/FileException.h>
#include <Crypto/Models/Hash.h>

#include <filesystem.h>

#include <string>
#include <system_error>
#include <vector>

// Atomic activation of a downloaded PIBD state.
//
// The writer produces a complete state in a staging directory. Activation swaps
// it in with two renames and a marker file:
//
//   1. write <live parent>/pibd_activation.pending with (staging, backup) paths
//   2. rename live -> backup            (if a live state exists)
//   3. rename staging -> live
//   4. remove the marker, then the backup
//
// If the process dies between 1 and 4, Recover() decides what happened from the
// marker plus the presence of a complete state, and either finishes the
// activation or restores the backup. No step truncates or rewrites live files in
// place, so a crash cannot leave a half-overwritten tree.
class PibdActivation
{
public:
	static constexpr const char* MARKER_FILE_NAME = "pibd_activation.pending";
	static constexpr const char* COMPLETE_FILE_NAME = "pibd_state.complete";

	// Written into a staging directory once every tree has been written and
	// verified. Activation refuses a staging directory without it.
	static void WriteCompleteMarker(const fs::path& stateDir, const Hash& blockHash, const uint64_t height)
	{
		const std::string text =
			HexUtil::ConvertToHex(blockHash.GetData()) + "\n" + std::to_string(height) + "\n";
		FileUtil::WriteTextToFile(stateDir / COMPLETE_FILE_NAME, text);
	}

	static bool IsCompleteState(const fs::path& stateDir)
	{
		return FileUtil::Exists(stateDir / COMPLETE_FILE_NAME);
	}

	static void Activate(const fs::path& stagingDir, const fs::path& liveDir)
	{
		if (!IsCompleteState(stagingDir)) {
			throw FILE_EXCEPTION_F("Staging state {} is incomplete", stagingDir);
		}

		const fs::path parent = liveDir.parent_path();
		if (!parent.empty() && !FileUtil::Exists(parent) && !FileUtil::CreateDirectories(parent)) {
			throw FILE_EXCEPTION_F("Failed to create directory {}", parent);
		}

		const fs::path markerPath = parent / MARKER_FILE_NAME;
		const fs::path backupDir = BackupPath(liveDir);

		// A marker left by an earlier interrupted activation is settled first.
		if (FileUtil::Exists(markerPath)) {
			Recover(liveDir);
		}

		FileUtil::WriteTextToFile(
			markerPath, stagingDir.u8string() + "\n" + backupDir.u8string() + "\n");

		if (FileUtil::Exists(liveDir)) {
			std::error_code error;
			fs::rename(liveDir, backupDir, error);
			if (error) {
				FileUtil::RemoveFile(markerPath);
				throw FILE_EXCEPTION_F("Failed to move live state to backup: {}", error.message());
			}
		}

		std::error_code error;
		fs::rename(stagingDir, liveDir, error);
		if (error) {
			// Roll back immediately so the node keeps its previous state.
			std::error_code rollbackError;
			fs::rename(backupDir, liveDir, rollbackError);
			FileUtil::RemoveFile(markerPath);
			throw FILE_EXCEPTION_F("Failed to activate staging state: {}", error.message());
		}

		FileUtil::RemoveFile(markerPath);

		std::error_code removeError;
		fs::remove_all(backupDir, removeError);   // best effort, the backup is stale now
	}

	// Settle an interrupted activation. Returns true when a marker was present.
	static bool Recover(const fs::path& liveDir)
	{
		const fs::path markerPath = liveDir.parent_path() / MARKER_FILE_NAME;
		if (!FileUtil::Exists(markerPath)) {
			return false;
		}

		const fs::path backupDir = ReadBackupPath(markerPath, liveDir);

		if (IsCompleteState(liveDir)) {
			// The staging directory made it into place; the backup is stale.
			FileUtil::RemoveFile(markerPath);
			std::error_code error;
			fs::remove_all(backupDir, error);
			return true;
		}

		if (FileUtil::Exists(backupDir)) {
			std::error_code error;
			if (FileUtil::Exists(liveDir)) {
				fs::remove_all(liveDir, error);
			}

			fs::rename(backupDir, liveDir, error);
			if (error) {
				throw FILE_EXCEPTION_F(
					"Failed to restore the txhashset backup: {}", error.message());
			}

			FileUtil::RemoveFile(markerPath);
			return true;
		}

		throw FILE_EXCEPTION_F(
			"Interrupted PIBD activation left {} incomplete and no backup exists", liveDir);
	}

private:
	static fs::path BackupPath(const fs::path& liveDir)
	{
		return fs::path(liveDir.u8string() + ".backup");
	}

	static fs::path ReadBackupPath(const fs::path& markerPath, const fs::path& liveDir)
	{
		std::vector<uint8_t> bytes;
		if (!FileUtil::ReadFile(markerPath, bytes)) {
			return BackupPath(liveDir);
		}

		std::string content(bytes.begin(), bytes.end());
		const size_t firstNewline = content.find('\n');
		if (firstNewline == std::string::npos) {
			return BackupPath(liveDir);
		}

		const size_t secondNewline = content.find('\n', firstNewline + 1);
		if (secondNewline == std::string::npos) {
			return BackupPath(liveDir);
		}

		// The marker may have been written in text mode, so strip CR/whitespace.
		std::string backup = content.substr(firstNewline + 1, secondNewline - firstNewline - 1);
		while (!backup.empty() &&
			(backup.back() == '\r' || backup.back() == '\n' || backup.back() == ' ' || backup.back() == '\t')) {
			backup.pop_back();
		}

		return backup.empty() ? BackupPath(liveDir) : fs::u8path(backup);
	}
};
