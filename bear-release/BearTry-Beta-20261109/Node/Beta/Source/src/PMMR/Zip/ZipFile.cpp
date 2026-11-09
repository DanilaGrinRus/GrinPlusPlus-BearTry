#include "ZipFile.h"

#include <Core/Exceptions/FileException.h>
#include <Common/Logger.h>
#include <fstream>

ZipFile::ZipFile(const fs::path& zipFilePath, const unzFile& file)
	: m_zipFilePath(zipFilePath), m_unzFile(file)
{

}

ZipFile::~ZipFile()
{
	unzClose(m_unzFile);
}

std::shared_ptr<ZipFile> ZipFile::Load(const fs::path& zipFilePath)
{
	unzFile unzFile = unzOpen(zipFilePath.u8string().c_str());
	if (unzFile == NULL)
	{
		LOG_ERROR_F("Zip file ({}) failed to open.", zipFilePath);
		throw FILE_EXCEPTION("Failed to open zip file");
	}

	return std::shared_ptr<ZipFile>(new ZipFile(zipFilePath, unzFile));
}

void ZipFile::ExtractFile(const std::string& path, const fs::path& destination,
	uint64_t* remainingBytes) const
{
	if (unzLocateFile(m_unzFile, path.c_str(), 0) != UNZ_OK)
	{
		LOG_INFO_F("Path ({}) in zip file ({}) was not found.", path, m_zipFilePath);
		throw FILE_EXCEPTION("File not found");
	}

	int openResult = unzOpenCurrentFile(m_unzFile);
	if (openResult != UNZ_OK)
	{
		LOG_INFO_F("Failed to open path ({}) in zip file ({}).", path, m_zipFilePath);
		throw FILE_EXCEPTION("Failed to open file");
	}

	std::ofstream destinationFile(destination, std::ios::out | std::ios::binary | std::ios::ate);
	if (!destinationFile.is_open())
	{
		LOG_WARNING_F("Failed to write to destination ({}).", destination);
		unzCloseCurrentFile(m_unzFile);
		throw FILE_EXCEPTION("Failed to write to destination");
	}

	// Check every layer: read/decompression errors, short writes and the CRC
	// reported on close. A partial archive must never become a serving state.
	try {
		unsigned char buffer[64 * 1024];
		int readSize = 0;
		while ((readSize = unzReadCurrentFile(m_unzFile, buffer, sizeof(buffer))) > 0) {
			if (remainingBytes != nullptr) {
				if (static_cast<uint64_t>(readSize) > *remainingBytes) {
					throw FILE_EXCEPTION("TxHashSet archive exceeds extraction byte budget");
				}
				*remainingBytes -= static_cast<uint64_t>(readSize);
			}
			destinationFile.write((const char*)buffer, readSize);
			if (!destinationFile) {
				throw FILE_EXCEPTION("Failed to write extracted file");
			}
		}
		if (readSize < 0) {
			throw FILE_EXCEPTION("Failed to decompress zip entry");
		}
		destinationFile.close();
		if (!destinationFile) {
			throw FILE_EXCEPTION("Failed to close extracted file");
		}
	} catch (...) {
		unzCloseCurrentFile(m_unzFile);
		throw;
	}
	if (unzCloseCurrentFile(m_unzFile) != UNZ_OK) {
		throw FILE_EXCEPTION("Zip entry failed CRC or close validation");
	}
}

std::vector<std::string> ZipFile::ListFiles() const
{
	std::vector<std::string> files;
	int status = unzGoToFirstFile(m_unzFile);
	while (status == UNZ_OK) {
		unz_file_info info{};
		if (unzGetCurrentFileInfo(m_unzFile, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK ||
			info.size_filename > 65535) {
			throw FILE_EXCEPTION("Invalid zip member metadata");
		}
		std::vector<char> name(info.size_filename + 1);
		if (unzGetCurrentFileInfo(m_unzFile, &info, name.data(), static_cast<unsigned long>(name.size()),
			nullptr, 0, nullptr, 0) != UNZ_OK) {
			throw FILE_EXCEPTION("Cannot read zip member name");
		}
		files.emplace_back(name.data(), info.size_filename);
		status = unzGoToNextFile(m_unzFile);
	}
	if (status != UNZ_END_OF_LIST_OF_FILE) throw FILE_EXCEPTION("Invalid zip directory");

	return files;
}
