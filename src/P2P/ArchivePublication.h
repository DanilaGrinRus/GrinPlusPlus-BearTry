#pragma once

#include <Core/File/FileSync.h>
#include "../PMMR/Zip/ZipFile.h"

#include <filesystem.h>

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
	#include <Windows.h>
#endif

// Safe publication for persistent TxHashSet archives.
//
// A snapshot zip is written to a non-final ".staging" path. Readiness checks
// validate the zip container and required legacy entries; publication flushes
// the staged file and atomically renames it over the final path. A partial or
// interrupted copy at the final path is therefore never treated as ready, and
// readers either see the old complete file or the new complete file. Root
// validation against the target header is performed by the caller before
// publication (extract + PibdArchiveServer::LoadAndValidate).
namespace ArchivePublication
{
	inline bool Fail(const std::string& message, std::string* pError)
	{
		if (pError != nullptr) {
			*pError = message;
		}
		return false;
	}

	inline fs::path StagingPath(const fs::path& finalPath)
	{
		// Deliberately not the final name: readiness and the legacy reader only
		// look at the exact final path.
		return fs::path(finalPath.string() + ".staging");
	}

	inline bool HasEntry(const std::vector<std::string>& entries, const std::string& name)
	{
		for (const std::string& entry : entries) {
			if (entry == name) {
				return true;
			}
		}
		return false;
	}

	inline bool HasEntryPrefix(const std::vector<std::string>& entries, const std::string& prefix)
	{
		for (const std::string& entry : entries) {
			if (entry.size() > prefix.size() && entry.compare(0, prefix.size(), prefix) == 0) {
				return true;
			}
		}
		return false;
	}

	// Readiness is more than fs::exists(): the file must be a complete zip with
	// the legacy TxHashSet entry layout. Root validation against the header is
	// still required before serving/publishing.
	inline bool IsReadyArchive(const fs::path& path, std::string* pError = nullptr)
	{
		std::error_code error;
		if (!fs::is_regular_file(path, error) || error) {
			return Fail("archive is missing or not a regular file", pError);
		}

		const auto bytes = fs::file_size(path, error);
		if (error) {
			return Fail("archive cannot be stat'ed", pError);
		}
		if (bytes == 0) {
			return Fail("archive is empty", pError);
		}

		try {
			const std::shared_ptr<ZipFile> pZip = ZipFile::Load(path);
			if (pZip == nullptr) {
				return Fail("archive zip cannot be opened", pError);
			}

			const std::vector<std::string> entries = pZip->ListFiles();
			for (const std::string& required : {
				std::string("kernel/pmmr_data.bin"),
				std::string("kernel/pmmr_hash.bin"),
				std::string("output/pmmr_data.bin"),
				std::string("output/pmmr_hash.bin"),
				std::string("output/pmmr_prun.bin"),
				std::string("rangeproof/pmmr_data.bin"),
				std::string("rangeproof/pmmr_hash.bin"),
				std::string("rangeproof/pmmr_prun.bin") })
			{
				if (!HasEntry(entries, required)) {
					return Fail("archive is missing " + required, pError);
				}
			}

			// SaveSnapshot materializes pmmr_leaf.bin.<shortHash> for legacy readers.
			if (!HasEntryPrefix(entries, "output/pmmr_leaf.bin.")) {
				return Fail("archive is missing output/pmmr_leaf.bin.<shortHash>", pError);
			}
			if (!HasEntryPrefix(entries, "rangeproof/pmmr_leaf.bin.")) {
				return Fail("archive is missing rangeproof/pmmr_leaf.bin.<shortHash>", pError);
			}

			return true;
		}
		catch (const std::exception& e) {
			return Fail(std::string("archive zip validation failed: ") + e.what(), pError);
		}
		catch (...) {
			return Fail("archive zip validation failed: unknown error", pError);
		}
	}

	// Copy the complete snapshot to the staging path. The caller owns the source
	// zip and must remove it; this function only manages the staging file.
	inline bool StageSnapshot(const fs::path& snapshotZip, const fs::path& finalPath, std::string* pError = nullptr)
	{
		const fs::path staged = StagingPath(finalPath);
		std::error_code error;
		fs::remove(staged, error);
		error.clear();

		if (!fs::exists(snapshotZip, error) || error) {
			return Fail("snapshot zip is missing", pError);
		}

		fs::create_directories(finalPath.parent_path(), error);
		if (error) {
			return Fail("cannot create archive directory: " + error.message(), pError);
		}

		fs::copy_file(snapshotZip, staged, fs::copy_options::overwrite_existing, error);
		if (error) {
			std::error_code removeError;
			fs::remove(staged, removeError);
			return Fail("cannot stage archive snapshot: " + error.message(), pError);
		}

		return true;
	}

	// Flush the staged file and atomically replace the final path. On any failure
	// the staging file is removed; the final path is never truncated in place.
	inline bool PublishStaged(const fs::path& staged, const fs::path& finalPath, std::string* pError = nullptr)
	{
		std::string readinessError;
		if (!IsReadyArchive(staged, &readinessError)) {
			std::error_code removeError;
			fs::remove(staged, removeError);
			return Fail("staged archive is not ready: " + readinessError, pError);
		}

		if (!FileSync::Flush(staged)) {
			std::error_code removeError;
			fs::remove(staged, removeError);
			return Fail("cannot fsync staged archive", pError);
		}

		std::error_code error;
		fs::create_directories(finalPath.parent_path(), error);
		if (error) {
			std::error_code removeError;
			fs::remove(staged, removeError);
			return Fail("cannot create archive directory: " + error.message(), pError);
		}

#ifdef _WIN32
		if (!MoveFileExW(
				staged.wstring().c_str(),
				finalPath.wstring().c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			std::error_code removeError;
			fs::remove(staged, removeError);
			return Fail("atomic archive rename failed", pError);
		}
#else
		fs::rename(staged, finalPath, error);
		if (error) {
			std::error_code removeError;
			fs::remove(staged, removeError);
			return Fail("atomic archive rename failed: " + error.message(), pError);
		}
#endif

		return true;
	}

	inline std::string FirstLine(const std::string& content)
	{
		const size_t newline = content.find('\n');
		std::string line = newline == std::string::npos ? content : content.substr(0, newline);
		while (!line.empty() &&
			(line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
		{
			line.pop_back();
		}
		return line;
	}

	// A persisted recovery failure is only skipped for the same target hash; a
	// different activation/horizon gets a fresh attempt.
	inline bool SameRecoveryTarget(const std::string& failedMarkerContent, const std::string& targetHex)
	{
		return FirstLine(failedMarkerContent) == targetHex;
	}

	// One confirmed-chain view for the archive-rewind preflight. The target block
	// body/spent record is deliberately not part of the required set: Rewind
	// unwinds heights flushedTip..target+1 and stops at the target header.
	struct RewindPreflightView
	{
		bool headerPresent = false;
		bool bodyPresent = false;
		bool spentComplete = false;
		size_t inputCount = 0;
		size_t mappedCount = 0;
		std::string headerHashHex;
	};

	using RewindPreflightViewFn = std::function<RewindPreflightView(uint64_t)>;

	inline bool ValidateRewindPreflight(
		const std::string& targetHashHex,
		const uint64_t targetHeight,
		const uint64_t flushedHeight,
		const RewindPreflightViewFn& viewAtHeight,
		std::string* pError = nullptr)
	{
		if (targetHeight > flushedHeight) {
			return Fail(
				"archive preflight: target height " + std::to_string(targetHeight) +
				" is above flushed tip " + std::to_string(flushedHeight),
				pError);
		}

		const RewindPreflightView targetView = viewAtHeight(targetHeight);
		if (!targetView.headerPresent) {
			return Fail(
				"archive preflight: missing confirmed header at height " +
				std::to_string(targetHeight) + " (target " + std::to_string(targetHeight) +
				", flushed " + std::to_string(flushedHeight) + ")",
				pError);
		}
		if (targetView.headerHashHex != targetHashHex) {
			return Fail(
				"archive preflight: target header mismatch at height " +
				std::to_string(targetHeight) + " expected " + targetHashHex +
				" got " + targetView.headerHashHex,
				pError);
		}

		// A no-op rewind only needs the target header. Do not require the target
		// body or its SPENT_OUTPUTS record; Rewind never consumes them.
		if (targetHeight == flushedHeight) {
			return true;
		}

		for (uint64_t height = flushedHeight; height > targetHeight; --height) {
			const RewindPreflightView view = viewAtHeight(height);
			if (!view.headerPresent) {
				return Fail(
					"archive preflight: missing confirmed header at height " +
					std::to_string(height) + " (target " + std::to_string(targetHeight) +
					", flushed " + std::to_string(flushedHeight) + ")",
					pError);
			}

			if (!view.bodyPresent) {
				return Fail(
					"archive preflight: missing block body at height " +
					std::to_string(height) + " hash " + view.headerHashHex +
					" (target " + std::to_string(targetHeight) + ")",
					pError);
			}

			if (!view.spentComplete) {
				return Fail(
					"archive preflight: block " + std::to_string(height) + " hash " +
					view.headerHashHex + " has " + std::to_string(view.inputCount) +
					" input(s) but " + std::to_string(view.mappedCount) +
					" mapped spent position(s) (target " + std::to_string(targetHeight) +
					", flushed " + std::to_string(flushedHeight) + ")",
					pError);
			}
		}

		return true;
	}
}
