#pragma once

#include <Common/Logger.h>
#include <filesystem.h>
#include <fstream>
#include <system_error>

// Atomic installation of a downloaded legacy txhashset. The zip is extracted and
// validated in a sibling staging directory; only then the live directory is
// renamed to a backup and the staging directory becomes live. A marker file makes
// a crash between the two renames recoverable on the next start (G-21).
namespace TxHashSetStaging
{
	inline fs::path Marker(const fs::path& live)
	{
		return live.parent_path() / "txhashset_activation.pending";
	}

	inline fs::path Staging(const fs::path& live)
	{
		return live.string() + ".staging";
	}

	inline fs::path Backup(const fs::path& live)
	{
		return live.string() + ".backup";
	}

	inline void Begin(const fs::path& live, const std::string& targetHash)
	{
		std::ofstream marker(Marker(live), std::ios::binary | std::ios::trunc);
		marker << targetHash;
		marker.close();
		if (!marker) {
			throw std::runtime_error("Failed to write txhashset activation marker");
		}
	}

	inline void Finish(const fs::path& live)
	{
		std::error_code ec;
		fs::remove_all(Backup(live), ec);
		if (ec) {
			throw std::runtime_error("Failed to remove committed txhashset backup: " + ec.message());
		}
		fs::remove(Marker(live), ec);
		if (ec) {
			throw std::runtime_error("Failed to remove txhashset activation marker: " + ec.message());
		}
	}

	inline void Recover(const fs::path& live, const std::string& committedHash = "")
	{
		std::error_code ec;
		const fs::path marker = Marker(live);
		if (!fs::exists(marker, ec)) {
			return;
		}

		const fs::path staging = Staging(live);
		const fs::path backup = Backup(live);
		std::ifstream markerFile(marker, std::ios::binary);
		std::string targetHash;
		markerFile >> targetHash;
		markerFile.close();
		if (targetHash.empty()) {
			throw std::runtime_error("Unreadable txhashset activation marker");
		}
		const bool committed = targetHash == committedHash;

		if (committed) {
			// The DB commit won. Complete the interrupted filesystem activation.
			if (!fs::exists(live) && fs::exists(staging)) {
				fs::rename(staging, live);
			}
			if (!fs::exists(live)) {
				throw std::runtime_error("Committed txhashset has no live state");
			}
		} else if (fs::exists(backup)) {
			// The DB commit did not happen. Restore the exact previous state.
			if (fs::exists(live)) {
				fs::remove_all(live);
			}
			fs::rename(backup, live);
		} else if (!fs::exists(staging) && fs::exists(live)) {
			// There was no previous live state and the new one was installed.
			fs::remove_all(live);
		}

		ec.clear();
		if (fs::exists(staging, ec)) {
			ec.clear();
			fs::remove_all(staging, ec);
		}

		if (ec) {
			throw std::runtime_error("Failed to remove staged txhashset: " + ec.message());
		}
		Finish(live);
		LOG_WARNING("Recovered a partially installed txhashset after an unclean shutdown.");
	}
}
