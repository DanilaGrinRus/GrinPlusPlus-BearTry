#pragma once

#include <Core/Models/BlockHeader.h>
#include <Core/Config.h>
#include <filesystem.h>
#include <cstdint>

// Forward Declarations
class ZipFile;

class TxHashSetZip
{
public:
	TxHashSetZip(const Config& config);

	// targetRoot empty means the live txhashset path; otherwise extraction is
	// staged into that directory (G-21 atomic legacy activation).
	bool Extract(const fs::path& path, const BlockHeader& header, const fs::path& targetRoot = fs::path()) const;

private:
	fs::path Root(const fs::path& targetRoot) const;
	void ExtractKernelFolder(const ZipFile& zipFile, const fs::path& root, uint64_t& remainingBytes) const;
	void ExtractFolder(const ZipFile& zipFile, const std::string& folderName, const BlockHeader& header,
		const fs::path& root, uint64_t& remainingBytes) const;
	void ExtractOutputFolder(const ZipFile& zipFile, const BlockHeader& header) const;
	void ExtractRangeProofFolder(const ZipFile& zipFile, const BlockHeader& header) const;

	const Config& m_config;
};
