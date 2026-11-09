#include <Common/Compat.h>
#include <Common/Util/FileUtil.h>
#include <Core/Exceptions/FileException.h>
#include <Common/Logger.h>

#include <fstream>
#include <cstdlib>

#ifdef _WIN32
#include <aclapi.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <unistd.h>
#endif


bool FileUtil::ReadFile(const fs::path& filePath, std::vector<uint8_t>& data)
{
	if (!fs::exists(filePath))
	{
		return false;
	}

	std::ifstream file(filePath, std::ios::in | std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		return false;
	}

	auto size = fs::file_size(filePath);
	data.resize((size_t)size);

	file.seekg(0, std::ios::beg);
	if (size && !file.read(reinterpret_cast<char*>(data.data()), size)) {
		throw FILE_EXCEPTION_F("Failed to read {}", filePath);
	}
	file.close();

	return true;
}

void FileUtil::RenameFile(const fs::path& source, const fs::path& destination)
{
	std::error_code ec;
	fs::rename(source, destination, ec);
	if (ec)
	{
		LOG_ERROR_F("Failed to rename {} to {}. Error: {}", source, destination, ec.message());
		throw FILE_EXCEPTION_F("Failed to rename {} to {}. Error: {}", source, destination, ec.message());
	}
}

void FileUtil::SafeWriteToFile(const fs::path& filePath, const std::vector<uint8_t>& data)
{
	const fs::path tmpFilePath = GrinStr(filePath.u8string() + ".tmp").ToPath();
	std::ofstream file(tmpFilePath, std::ios::out | std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		LOG_ERROR_F("Failed to open {}", tmpFilePath);
		throw FILE_EXCEPTION_F("Failed to open {}", tmpFilePath);
	}

	file.exceptions(std::ios::badbit | std::ios::failbit);
	if (!data.empty()) file.write(reinterpret_cast<const char*>(data.data()), data.size());
	file.close();

#ifdef _WIN32
	// W-10 residual: make the rename durable. std::ofstream::close only flushes
	// user-space buffers; without FlushFileBuffers a power loss can leave an
	// empty/partial seed.json even though rename succeeded.
	HANDLE hFile = CreateFileW(
		tmpFilePath.wstring().c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);
	if (hFile == INVALID_HANDLE_VALUE) {
		LOG_ERROR_F("Failed to open {} for flush", tmpFilePath);
		throw FILE_EXCEPTION_F("Failed to open {} for flush", tmpFilePath);
	}
	const BOOL flushed = FlushFileBuffers(hFile);
	CloseHandle(hFile);
	if (!flushed) {
		LOG_ERROR_F("Failed to flush {}", tmpFilePath);
		throw FILE_EXCEPTION_F("Failed to flush {}", tmpFilePath);
	}
#endif

	RenameFile(tmpFilePath, filePath);
}

void FileUtil::WriteTextToFile(const fs::path& filePath, const std::string& text)
{
	std::ofstream file(filePath, std::ios::out | std::ios::trunc);
	if (!file.is_open())
	{
		LOG_ERROR_F("Failed to open {}", filePath);
		throw FILE_EXCEPTION_F("Failed to open {}", filePath);
	}

	file.write(text.c_str(), text.size());
	file.close();
}

bool FileUtil::RemoveFile(const fs::path& filePath) noexcept
{
	std::error_code ec;
	const uintmax_t removed = fs::remove_all(filePath, ec);

	LOG_INFO_F("Removed {} with status {}", filePath, ec.message());

	return removed > 0 && ec.value() == 0;
}

void FileUtil::CopyDirectory(const fs::path& sourceDir, const fs::path& destDir)
{
	std::error_code ec;
	fs::create_directories(destDir, ec);
	if (ec)
	{
		throw std::system_error(ec);
	}

	if (!fs::exists(sourceDir, ec) || !fs::exists(destDir, ec))
	{
		throw std::system_error(ec);
	}

	fs::copy(sourceDir, destDir, fs::copy_options::recursive, ec);
	if (ec)
	{
		throw std::system_error(ec);
	}
}

bool FileUtil::CreateDirectories(const fs::path& directory) noexcept
{
	std::error_code ec;
	const bool created = fs::create_directories(directory, ec);
	if (ec)
	{
		LOG_ERROR_F("Error while creating {}. Error: ", directory, ec.message());
	}

	return created;
}

bool FileUtil::Exists(const fs::path& path) noexcept
{
	std::error_code ec;
	const bool exists = fs::exists(path, ec);
	if (ec)
	{
		LOG_ERROR_F("Error while checking if {} exists. Error: ", path, ec.message());
	}

	return exists;
}

fs::path FileUtil::GetHomeDirectory()
{
#ifdef _WIN32
	auto readEnv = [](const wchar_t* name) {
		size_t required = 0;
		if (_wgetenv_s(&required, nullptr, 0, name) != 0 || required == 0) {
			throw FILE_EXCEPTION("Missing home directory environment variable");
		}
		std::vector<wchar_t> buffer(required);
		if (_wgetenv_s(&required, buffer.data(), buffer.size(), name) != 0 || required == 0) {
			throw FILE_EXCEPTION("Failed to read home directory environment variable");
		}
		return std::wstring(buffer.data(), required - 1);
	};
	return fs::path(readEnv(L"HOMEDRIVE")) / readEnv(L"HOMEPATH");
#else
	char* pHomePath = getenv("HOME");
	if (pHomePath != nullptr)
	{
		return fs::path(pHomePath);
	}
	else
	{
		return fs::path("~");
	}
#endif
}

bool FileUtil::TruncateFile(const fs::path& filePath, const uint64_t size)
{
#ifdef _WIN32
	HANDLE hFile = CreateFile(filePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	LARGE_INTEGER li;
	li.QuadPart = size;
	const bool success = SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) && SetEndOfFile(hFile);

	const DWORD err = GetLastError();
	LOG_INFO_F("hFile: {} - success: {} - error: {}", (uint64_t)hFile, std::to_string(success), err);

	CloseHandle(hFile);

	return success;
#else
	return truncate(filePath.c_str(), size) == 0;
#endif
}

std::vector<GrinStr> FileUtil::GetSubDirectories(const fs::path& filePath, const bool includeHidden)
{
	std::vector<GrinStr> listOfFiles;
	try {
		// Check if given path exists and points to a directory
		if (fs::exists(filePath) && fs::is_directory(filePath))
		{
			// Create a Recursive Directory Iterator object and points to the starting of directory
			fs::recursive_directory_iterator iter(filePath);

			// Create a Recursive Directory Iterator object pointing to end.
			fs::recursive_directory_iterator end;

			// Iterate till end
			while (iter != end)
			{
				// Check if current entry is a directory and if exists in skip list
				if (fs::is_directory(iter->path()))
				{
					// Skip the iteration of current directory pointed by iterator
					// c++17 fstem API to skip current directory iteration
					iter.disable_recursion_pending();
					GrinStr filename = StringUtil::ToUTF8(iter->path().filename().wstring());
					if (includeHidden || !filename.StartsWith("."))
					{
						listOfFiles.push_back(filename);
					}
				}

				std::error_code ec;
				// Increment the iterator to point to next entry in recursive iteration
				iter.increment(ec);
				if (ec)
				{
					LOG_ERROR_F("Error While Accessing: ({}) - {}", iter->path().string(), ec.message());
					break;
				}
			}
		}
	}
	catch (const std::exception& e)
	{
		LOG_ERROR_F("Exception thrown: {}", e);
	}

	return listOfFiles;
}

bool FileUtil::HardenPermissions(const fs::path& path) noexcept
{
#ifdef _WIN32
	try
	{
		const std::wstring widePath = path.wstring();
		PSID pOwner = nullptr;
		PSECURITY_DESCRIPTOR pSD = nullptr;
		if (::GetNamedSecurityInfoW(
				const_cast<LPWSTR>(widePath.c_str()),
				SE_FILE_OBJECT,
				OWNER_SECURITY_INFORMATION,
				&pOwner,
				nullptr,
				nullptr,
				nullptr,
				&pSD) != ERROR_SUCCESS)
		{
			return false;
		}

		BYTE systemSid[SECURITY_MAX_SID_SIZE];
		DWORD systemSidSize = static_cast<DWORD>(sizeof(systemSid));
		BYTE adminSid[SECURITY_MAX_SID_SIZE];
		DWORD adminSidSize = static_cast<DWORD>(sizeof(adminSid));
		const bool haveSids =
			::CreateWellKnownSid(WinLocalSystemSid, nullptr, reinterpret_cast<PSID>(systemSid), &systemSidSize) != FALSE &&
			::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, reinterpret_cast<PSID>(adminSid), &adminSidSize) != FALSE;
		if (!haveSids || pOwner == nullptr)
		{
			::LocalFree(pSD);
			return false;
		}

		const DWORD inheritance = fs::is_directory(path)
			? SUB_CONTAINERS_AND_OBJECTS_INHERIT
			: NO_INHERITANCE;

		EXPLICIT_ACCESS_W entries[3] = {};
		const auto fill = [&](const int index, PSID sid) {
			entries[index].grfAccessPermissions = GENERIC_ALL;
			entries[index].grfAccessMode = SET_ACCESS;
			entries[index].grfInheritance = inheritance;
			entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
			entries[index].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
			entries[index].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
		};
		fill(0, pOwner);
		fill(1, reinterpret_cast<PSID>(systemSid));
		fill(2, reinterpret_cast<PSID>(adminSid));

		PACL pNewAcl = nullptr;
		if (::SetEntriesInAclW(3, entries, nullptr, &pNewAcl) != ERROR_SUCCESS)
		{
			::LocalFree(pSD);
			return false;
		}

		const DWORD status = ::SetNamedSecurityInfoW(
			const_cast<LPWSTR>(widePath.c_str()),
			SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
			nullptr,
			nullptr,
			pNewAcl,
			nullptr);

		::LocalFree(pNewAcl);
		::LocalFree(pSD);
		return status == ERROR_SUCCESS;
	}
	catch (...)
	{
		return false;
	}
#else
	(void)path;
	return true;
#endif
}

size_t FileUtil::GetFileSize(const fs::path& file)
{
	std::error_code error;
	size_t fileSize = fs::file_size(file, error);
	if (error.value() != 0)
	{
		return 0;
	}

	return fileSize;
}
