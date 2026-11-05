#include "Zipper.h"

#include <Common/Util/FileUtil.h>
#include <Core/File/File.h>
#include <Core/Exceptions/FileException.h>
#include <Common/Logger.h>
#include <fstream>
#include <filesystem.h>
#include <array>
#include <algorithm>
#include <limits>

void Zipper::CreateZipFile(const fs::path& destination, const std::vector<fs::path>& paths)
{
	zipFile zf = zipOpen(destination.u8string().c_str(), APPEND_STATUS_CREATE);
	if (zf == nullptr)
	{
		LOG_ERROR_F("Failed to create zip file at ({})", destination);
		throw FILE_EXCEPTION_F("Failed to create zip file at ({})", destination);
	}

	try
	{
		for (size_t i = 0; i < paths.size(); i++)
		{
			if (fs::is_directory(paths[i]))
			{
				AddDirectory(zf, paths[i], paths[i].filename());
			}
			else
			{
				AddFile(zf, paths[i], destination);
			}
		}
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Failed to create zip file: {}", e.what());
		zipClose(zf, NULL);
		throw;
	}

	if (zipClose(zf, NULL))
	{
		LOG_ERROR_F("Failed to close zip file ({})", destination);
		throw FILE_EXCEPTION_F("Failed to close zip file ({})", destination);
	}
}

void Zipper::AddDirectory(zipFile zf, const fs::path& sourceDir, const fs::path& destDir)
{
	for (const auto& entry : fs::directory_iterator(sourceDir))
	{
		if (fs::is_directory(entry))
		{
			AddDirectory(zf, entry, destDir / entry.path().filename());
		}
		else
		{
			AddFile(zf, entry.path().string(), destDir);
		}
	}
}

void Zipper::AddFile(zipFile zf, const fs::path& sourceFile, const fs::path& destDir)
{
	File pFile = File::Load(sourceFile, std::ios::binary | std::ios::in);
	if (!pFile->is_open()) {
		throw FILE_EXCEPTION_F("Failed to open source file {}", sourceFile);
	}
	const uint64_t size = FileUtil::GetFileSize(sourceFile);
	const fs::path fileName = destDir / sourceFile.filename();
	zip_fileinfo zfi{};
	const int zip64 = size >= (std::numeric_limits<uint32_t>::max)() ? 1 : 0;
	if (zipOpenNewFileInZip64(zf, fileName.generic_string().c_str(), &zfi,
		nullptr, 0, nullptr, 0, nullptr, 0, Z_NO_COMPRESSION, zip64) != ZIP_OK) {
		throw FILE_EXCEPTION_F("Failed to open zip entry {}", fileName);
	}

	try {
		std::array<char, 64 * 1024> buffer{};
		uint64_t written = 0;
		while (written < size) {
			const std::streamsize requested = static_cast<std::streamsize>(
				(std::min)(uint64_t(buffer.size()), size - written));
			pFile->read(buffer.data(), requested);
			const std::streamsize got = pFile->gcount();
			if (got != requested || zipWriteInFileInZip(zf, buffer.data(),
				static_cast<unsigned int>(got)) != ZIP_OK) {
				throw FILE_EXCEPTION_F("Failed to stream source file {} into zip", sourceFile);
			}
			written += static_cast<uint64_t>(got);
		}
	} catch (...) {
		zipCloseFileInZip(zf);
		throw;
	}
	if (zipCloseFileInZip(zf) != ZIP_OK) {
		throw FILE_EXCEPTION_F("Failed to close zip entry {}", fileName);
	}
}
