#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <filesystem.h>
#include <fstream>
#include <string>
#include <vector>

// Diagnostics and forensics for startup chain-store repair. The counters are
// per process (an unclean shutdown that required repair is visible in
// /v1/status), and the affected files are copied aside before they are truncated
// so the corruption can be analysed later.
namespace DbRecovery
{
	inline std::atomic<uint64_t>& RepairCount() noexcept
	{
		static std::atomic<uint64_t> count{ 0 };
		return count;
	}

	inline std::atomic<int64_t>& LastRepairTime() noexcept
	{
		static std::atomic<int64_t> last{ 0 };
		return last;
	}

	inline void RecordRepair() noexcept
	{
		RepairCount()++;
		LastRepairTime() = (int64_t)std::time(nullptr);
	}

	inline uint64_t GetRepairCount() noexcept
	{
		return RepairCount().load();
	}

	inline int64_t GetLastRepairTime() noexcept
	{
		return LastRepairTime().load();
	}

	inline fs::path GetRecoveryDir(const fs::path& chainPath)
	{
		const std::string name =
			"recovery-" + std::to_string((int64_t)std::time(nullptr));
		return chainPath.parent_path() / name;
	}

	// Copies existing files aside before repair. Missing files are ignored, but
	// every existing file must be copied completely: if any copy fails the
	// function returns false and the caller must abort the repair. A partial or
	// zero-byte backup must never be used as justification for truncation.
	inline bool CopyForRecovery(
		const fs::path& chainPath,
		const std::vector<std::string>& fileNames)
	{
		std::error_code ec;
		const fs::path dir = GetRecoveryDir(chainPath);
		bool copiedAny = false;
		bool createdDir = false;

		for (const std::string& name : fileNames) {
			const fs::path source = chainPath / name;
			if (!fs::exists(source, ec)) {
				continue;
			}

			if (!createdDir) {
				fs::create_directories(dir, ec);
				if (ec) {
					return false;
				}
				createdDir = true;
			}

			const fs::path target = dir / name;
			// HashFile keeps the source open; CopyFileW can fail on the sharing
			// mode (and leave a 0-byte target behind). Copy the bytes ourselves and
			// remove the target on any failure.
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

			if (!ok) {
				ec.clear();
				fs::remove(target, ec);
				return false;
			}

			// Verify the copy instead of trusting a successful stream close.
			ec.clear();
			if (fs::file_size(source, ec) != fs::file_size(target, ec) || ec) {
				ec.clear();
				fs::remove(target, ec);
				return false;
			}

			copiedAny = true;
		}

		return copiedAny || createdDir;
	}
}
