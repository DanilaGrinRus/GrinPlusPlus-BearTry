#include <Net/Servers/RPC/ApiAccess.h>

#include <Net/Util/HTTPUtil.h>
#include <Crypto/Crypto.h>
#include <Common/Util/HexUtil.h>
#include <Common/Util/StringUtil.h>
#include <Common/Logger.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
	std::mutex g_mutex;
	fs::path g_secretPath;
	std::string g_secret;
	bool g_initialized = false;

	constexpr size_t SECRET_HEX_LENGTH = 64;

	std::string ToLower(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return value;
	}

	bool IsHex(const std::string& value)
	{
		if (value.empty()) {
			return false;
		}

		for (const char c : value) {
			if (!std::isxdigit(static_cast<unsigned char>(c))) {
				return false;
			}
		}

		return true;
	}

	bool IsValidSecretText(const std::string& value)
	{
		return value.size() == SECRET_HEX_LENGTH && IsHex(value);
	}

	// Splits "host[:port]" (IPv6 in brackets) into a lowercased host and port.
	bool SplitHost(const std::string& hostHeader, std::string& host, std::optional<uint16_t>& port)
	{
		if (hostHeader.empty()) {
			return false;
		}

		const std::string value = StringUtil::Trim(hostHeader);
		std::string portPart;
		if (value.front() == '[') {
			const size_t close = value.find(']');
			if (close == std::string::npos) {
				return false;
			}
			host = value.substr(1, close - 1);
			if (close + 1 < value.size()) {
				if (value[close + 1] != ':') {
					return false;
				}
				portPart = value.substr(close + 2);
			}
		} else {
			const size_t colon = value.rfind(':');
			if (colon != std::string::npos && colon + 1 < value.size() &&
				std::all_of(value.begin() + colon + 1, value.end(), [](unsigned char c) {
					return std::isdigit(c) != 0;
				})) {
				host = value.substr(0, colon);
				portPart = value.substr(colon + 1);
			} else {
				host = value;
			}
		}

		if (host.empty()) {
			return false;
		}

		// Reject anything that is not a plain host (no spaces, no credentials).
		if (host.find('@') != std::string::npos || host.find('/') != std::string::npos ||
			host.find(' ') != std::string::npos) {
			return false;
		}

		if (!portPart.empty()) {
			if (portPart.size() > 5) {
				return false;
			}
			try {
				const unsigned long parsed = std::stoul(portPart);
				if (parsed == 0 || parsed > 65535) {
					return false;
				}
				port = static_cast<uint16_t>(parsed);
			} catch (const std::exception&) {
				return false;
			}
		}

		host = ToLower(host);
		return true;
	}

	bool IsLoopbackHost(const std::string& host)
	{
		return host == "127.0.0.1" || host == "localhost" || host == "::1" ||
			host == "0:0:0:0:0:0:0:1";
	}

	enum class FileReadResult
	{
		ReadOk,
		ReadMissing,
		ReadError
	};

#ifdef _WIN32
	bool GetCurrentUserSid(std::vector<BYTE>& sid, std::string& error)
	{
		HANDLE hToken = INVALID_HANDLE_VALUE;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
			error = "OpenProcessToken failed";
			return false;
		}

		DWORD length = 0;
		GetTokenInformation(hToken, TokenUser, nullptr, 0, &length);
		if (length == 0) {
			CloseHandle(hToken);
			error = "GetTokenInformation size failed";
			return false;
		}

		std::vector<BYTE> buffer(length);
		const BOOL ok = GetTokenInformation(hToken, TokenUser, buffer.data(), length, &length);
		CloseHandle(hToken);
		if (!ok) {
			error = "GetTokenInformation failed";
			return false;
		}

		const TOKEN_USER* pUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
		const DWORD sidLength = GetLengthSid(pUser->User.Sid);
		sid.assign(reinterpret_cast<const BYTE*>(pUser->User.Sid),
			reinterpret_cast<const BYTE*>(pUser->User.Sid) + sidLength);
		return true;
	}

	bool BuildSecretSecurityDescriptor(std::vector<BYTE>& sd, std::string& error)
	{
		std::vector<BYTE> currentSid;
		if (!GetCurrentUserSid(currentSid, error)) {
			return false;
		}

		LPWSTR pSidString = nullptr;
		if (!ConvertSidToStringSidW(reinterpret_cast<PSID>(currentSid.data()), &pSidString)) {
			error = "ConvertSidToStringSidW failed";
			return false;
		}

		std::wstring sddl = L"O:";
		sddl += pSidString;
		sddl += L"D:P(A;;FA;;;";
		sddl += pSidString;
		sddl += L")(A;;FA;;;SY)";
		LocalFree(pSidString);

		PSECURITY_DESCRIPTOR pSd = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			sddl.c_str(), SDDL_REVISION_1, &pSd, nullptr)) {
			error = "ConvertStringSecurityDescriptorToSecurityDescriptorW failed";
			return false;
		}

		const DWORD sdLength = GetSecurityDescriptorLength(pSd);
		sd.assign(reinterpret_cast<BYTE*>(pSd), reinterpret_cast<BYTE*>(pSd) + sdLength);
		LocalFree(pSd);
		return true;
	}

	bool IsSecureHandle(HANDLE hFile, std::string& error)
	{
		BY_HANDLE_FILE_INFORMATION info;
		if (!GetFileInformationByHandle(hFile, &info)) {
			error = "GetFileInformationByHandle failed";
			return false;
		}

		if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
			(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			error = "secret path is a symlink/reparse point or directory";
			return false;
		}

		std::vector<BYTE> currentSid;
		if (!GetCurrentUserSid(currentSid, error)) {
			return false;
		}

		PSID pOwner = nullptr;
		PACL pDacl = nullptr;
		PSECURITY_DESCRIPTOR pSd = nullptr;
		const DWORD result = GetSecurityInfo(
			hFile,
			SE_FILE_OBJECT,
			OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
			&pOwner,
			nullptr,
			&pDacl,
			nullptr,
			&pSd);

		if (result != ERROR_SUCCESS || pSd == nullptr) {
			error = "GetSecurityInfo failed";
			if (pSd != nullptr) {
				LocalFree(pSd);
			}
			return false;
		}

		bool secure = pOwner != nullptr && EqualSid(pOwner, reinterpret_cast<PSID>(currentSid.data()));
		if (secure) {
			SECURITY_DESCRIPTOR_CONTROL control = 0;
			DWORD revision = 0;
			if (!GetSecurityDescriptorControl(pSd, &control, &revision) ||
				(control & SE_DACL_PROTECTED) == 0 || pDacl == nullptr) {
				secure = false;
			}
		}

		if (secure) {
			BYTE systemSidBuffer[SECURITY_MAX_SID_SIZE];
			DWORD systemSidLength = sizeof(systemSidBuffer);
			if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSidBuffer, &systemSidLength)) {
				secure = false;
			} else {
				const PSID pSystemSid = reinterpret_cast<PSID>(systemSidBuffer);
				ACL_SIZE_INFORMATION aclInfo;
				if (GetAclInformation(pDacl, &aclInfo, sizeof(aclInfo), AclSizeInformation)) {
					for (DWORD i = 0; i < aclInfo.AceCount && secure; i++) {
						void* pAce = nullptr;
						if (!GetAce(pDacl, i, &pAce)) {
							secure = false;
							break;
						}

						const ACE_HEADER* pHeader = reinterpret_cast<const ACE_HEADER*>(pAce);
						if (pHeader->AceType != ACCESS_ALLOWED_ACE_TYPE) {
							continue;
						}

						const ACCESS_ALLOWED_ACE* pAllowed =
							reinterpret_cast<const ACCESS_ALLOWED_ACE*>(pAce);
						const PSID pAceSid = reinterpret_cast<PSID>(
							const_cast<DWORD*>(&pAllowed->SidStart));
						if (!EqualSid(pAceSid, reinterpret_cast<PSID>(currentSid.data())) &&
							!EqualSid(pAceSid, pSystemSid)) {
							secure = false;
						}
					}
				} else {
					secure = false;
				}
			}
		}

		if (pSd != nullptr) {
			LocalFree(pSd);
		}
		if (!secure) {
			error = "secret file owner/DACL is not owner-only";
		}
		return secure;
	}

	FileReadResult ReadExistingSecret(const fs::path& path, std::string& token, std::string& error)
	{
		HANDLE hFile = CreateFileW(
			path.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr);

		if (hFile == INVALID_HANDLE_VALUE) {
			const DWORD lastError = GetLastError();
			if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
				return FileReadResult::ReadMissing;
			}
			error = "CreateFileW(OPEN_EXISTING) failed";
			return FileReadResult::ReadError;
		}

		if (!IsSecureHandle(hFile, error)) {
			CloseHandle(hFile);
			return FileReadResult::ReadError;
		}

		LARGE_INTEGER size;
		if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0 || size.QuadPart > 4096) {
			error = "secret file has an invalid size";
			CloseHandle(hFile);
			return FileReadResult::ReadError;
		}

		std::string data(static_cast<size_t>(size.QuadPart), '\0');
		DWORD totalRead = 0;
		while (totalRead < data.size()) {
			DWORD readNow = 0;
			const DWORD remaining = static_cast<DWORD>(data.size() - totalRead);
			if (!ReadFile(hFile, data.data() + totalRead, remaining, &readNow, nullptr) || readNow == 0) {
				error = "ReadFile failed";
				CloseHandle(hFile);
				return FileReadResult::ReadError;
			}
			totalRead += readNow;
		}

		if (!CloseHandle(hFile)) {
			error = "CloseHandle failed";
			return FileReadResult::ReadError;
		}

		token = StringUtil::Trim(data);
		return FileReadResult::ReadOk;
	}

	FileReadResult CreateSecret(const fs::path& path, const std::string& token, std::string& error)
	{
		std::vector<BYTE> sdBytes;
		if (!BuildSecretSecurityDescriptor(sdBytes, error)) {
			return FileReadResult::ReadError;
		}

		SECURITY_ATTRIBUTES sa;
		sa.nLength = sizeof(sa);
		sa.lpSecurityDescriptor = sdBytes.data();
		sa.bInheritHandle = FALSE;

		HANDLE hFile = CreateFileW(
			path.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			0,
			&sa,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr);

		if (hFile == INVALID_HANDLE_VALUE) {
			const DWORD lastError = GetLastError();
			if (lastError == ERROR_FILE_EXISTS || lastError == ERROR_ALREADY_EXISTS) {
				return FileReadResult::ReadMissing;
			}
			error = "CreateFileW(CREATE_NEW) failed";
			return FileReadResult::ReadError;
		}

		bool ok = true;
		DWORD totalWritten = 0;
		while (totalWritten < token.size()) {
			DWORD written = 0;
			const DWORD remaining = static_cast<DWORD>(token.size() - totalWritten);
			if (!WriteFile(hFile, token.data() + totalWritten, remaining, &written, nullptr) || written == 0) {
				error = "WriteFile failed";
				ok = false;
				break;
			}
			totalWritten += written;
		}

		if (ok && !FlushFileBuffers(hFile)) {
			error = "FlushFileBuffers failed";
			ok = false;
		}

		if (!CloseHandle(hFile) && ok) {
			error = "CloseHandle failed";
			ok = false;
		}

		if (!ok) {
			DeleteFileW(path.c_str());
			return FileReadResult::ReadError;
		}

		// Re-open with the same checks used for every subsequent start. This
		// closes the gap between creation and the first use.
		std::string readBack;
		const FileReadResult result = ReadExistingSecret(path, readBack, error);
		if (result != FileReadResult::ReadOk || readBack != token) {
			DeleteFileW(path.c_str());
			error = "newly created secret could not be re-read securely";
			return FileReadResult::ReadError;
		}

		return FileReadResult::ReadOk;
	}
#else
	FileReadResult ReadExistingSecret(const fs::path& path, std::string& token, std::string& error)
	{
		const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			if (errno == ENOENT) {
				return FileReadResult::ReadMissing;
			}
			error = "open(O_RDONLY) failed";
			return FileReadResult::ReadError;
		}

		struct stat st;
		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
			error = "secret path is not a regular file";
			close(fd);
			return FileReadResult::ReadError;
		}

		if (st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
			error = "secret file owner/mode is not owner-only";
			close(fd);
			return FileReadResult::ReadError;
		}

		if (st.st_size > 4096) {
			error = "secret file has an invalid size";
			close(fd);
			return FileReadResult::ReadError;
		}

		std::string data(static_cast<size_t>(st.st_size), '\0');
		size_t totalRead = 0;
		while (totalRead < data.size()) {
			const ssize_t readNow = read(fd, data.data() + totalRead, data.size() - totalRead);
			if (readNow <= 0) {
				error = "read failed";
				close(fd);
				return FileReadResult::ReadError;
			}
			totalRead += static_cast<size_t>(readNow);
		}

		if (close(fd) != 0) {
			error = "close failed";
			return FileReadResult::ReadError;
		}

		token = StringUtil::Trim(data);
		return FileReadResult::ReadOk;
	}

	FileReadResult CreateSecret(const fs::path& path, const std::string& token, std::string& error)
	{
		const int fd = open(
			path.c_str(),
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
			0600);
		if (fd < 0) {
			if (errno == EEXIST) {
				return FileReadResult::ReadMissing;
			}
			error = "open(O_CREAT|O_EXCL) failed";
			return FileReadResult::ReadError;
		}

		bool ok = true;
		size_t totalWritten = 0;
		while (totalWritten < token.size()) {
			const ssize_t written = write(fd, token.data() + totalWritten, token.size() - totalWritten);
			if (written <= 0) {
				error = "write failed";
				ok = false;
				break;
			}
			totalWritten += static_cast<size_t>(written);
		}

		if (ok && fsync(fd) != 0) {
			error = "fsync failed";
			ok = false;
		}

		if (close(fd) != 0 && ok) {
			error = "close failed";
			ok = false;
		}

		if (ok) {
			const int dirFd = open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
			if (dirFd >= 0) {
				if (fsync(dirFd) != 0) {
					error = "fsync(data directory) failed";
					ok = false;
				}
				close(dirFd);
			} else {
				error = "open(data directory) failed";
				ok = false;
			}
		}

		if (!ok) {
			unlink(path.c_str());
			return FileReadResult::ReadError;
		}

		std::string readBack;
		const FileReadResult result = ReadExistingSecret(path, readBack, error);
		if (result != FileReadResult::ReadOk || readBack != token) {
			unlink(path.c_str());
			error = "newly created secret could not be re-read securely";
			return FileReadResult::ReadError;
		}

		return FileReadResult::ReadOk;
	}
#endif

	bool LoadOrCreateSecret(const fs::path& path, std::string& token, std::string& error)
	{
		std::string existing;
		const FileReadResult existingResult = ReadExistingSecret(path, existing, error);
		if (existingResult == FileReadResult::ReadOk) {
			token = existing;
			return true;
		}
		if (existingResult == FileReadResult::ReadError) {
			// Never replace a malformed/insecure secret: fail closed instead.
			return false;
		}

		const std::vector<uint8_t> randomBytes = Crypto::GenerateSecureNonce().GetBytes().GetData();
		const std::string generated = HexUtil::ConvertToHex(randomBytes);
		if (!IsValidSecretText(generated)) {
			error = "generated secret has an invalid length";
			return false;
		}

		const FileReadResult createResult = CreateSecret(path, generated, error);
		if (createResult == FileReadResult::ReadOk) {
			token = generated;
			return true;
		}
		if (createResult == FileReadResult::ReadMissing) {
			// Lost a creation race. Read the winner's file securely.
			std::string raced;
			const FileReadResult racedResult = ReadExistingSecret(path, raced, error);
			if (racedResult == FileReadResult::ReadOk) {
				token = raced;
				return true;
			}
		}

		return false;
	}
}

namespace ApiAccess
{
	void Initialize(const fs::path& dataDir)
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		g_secretPath = dataDir / ".api_secret";
		g_secret.clear();
		g_initialized = false;

		std::string token;
		std::string error;
		if (!LoadOrCreateSecret(g_secretPath, token, error)) {
			LOG_ERROR_F("Refusing PRIVATE/ADMIN API access: {}", error);
			return;
		}

		if (!IsValidSecretText(token)) {
			LOG_WARNING_F("API secret file {} is malformed; refusing PRIVATE/ADMIN API access", g_secretPath);
			return;
		}

		g_secret = ToLower(token);
		g_initialized = true;
		LOG_INFO_F("API secret loaded from {}", g_secretPath);
	}

	bool IsInitialized()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_initialized;
	}

	const fs::path& GetSecretPath()
	{
		return g_secretPath;
	}

	bool ValidateToken(const std::string& provided, const std::string& expected)
	{
		if (expected.empty() || provided.size() != expected.size()) {
			return false;
		}

		const std::string lhs = ToLower(provided);
		unsigned char diff = 0;
		for (size_t i = 0; i < expected.size(); i++) {
			diff |= static_cast<unsigned char>(lhs[i] ^ expected[i]);
		}

		return diff == 0;
	}

	bool IsAllowedHost(const std::string& hostHeader, const uint16_t serverPort)
	{
		std::string host;
		std::optional<uint16_t> port;
		if (!SplitHost(hostHeader, host, port)) {
			return false;
		}

		if (port.has_value() && *port != serverPort) {
			return false;
		}

		return IsLoopbackHost(host);
	}

	bool IsAllowedOrigin(const std::string& origin, const uint16_t serverPort)
	{
		// Any loopback port is accepted; the admin token remains the primary control.
		(void)serverPort;
		const std::string value = ToLower(StringUtil::Trim(origin));
		if (value.empty() || value == "null") {
			return true;
		}

		const size_t schemeEnd = value.find("://");
		if (schemeEnd == std::string::npos) {
			return false;
		}

		std::string authority = value.substr(schemeEnd + 3);
		const size_t slash = authority.find('/');
		if (slash != std::string::npos) {
			authority = authority.substr(0, slash);
		}

		std::string host;
		std::optional<uint16_t> port;
		if (!SplitHost(authority, host, port)) {
			return false;
		}

		// Loopback origins of any port are allowed (Electron dev server, local tools);
		// the admin token is still required, so this is defense in depth, not auth.
		return IsLoopbackHost(host);
	}

	bool Check(mg_connection* conn, const Level level, const uint16_t serverPort)
	{
		if (level == Level::PUBLIC) {
			return true;
		}

		std::string expected;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_initialized) {
				return false;
			}
			expected = g_secret;
		}

		std::string provided;
		const std::optional<std::string> authorization = HTTPUtil::GetHeaderValue(conn, "Authorization");
		if (authorization.has_value()) {
			std::string value = StringUtil::Trim(authorization.value());
			const std::string prefix = "bearer ";
			if (value.size() > prefix.size() && ToLower(value.substr(0, prefix.size())) == prefix) {
				provided = StringUtil::Trim(value.substr(prefix.size()));
			}
		}

		if (provided.empty()) {
			const std::optional<std::string> apiSecret = HTTPUtil::GetHeaderValue(conn, "X-API-Secret");
			if (apiSecret.has_value()) {
				provided = StringUtil::Trim(apiSecret.value());
			}
		}

		if (!ValidateToken(provided, expected)) {
			return false;
		}

		const std::optional<std::string> host = HTTPUtil::GetHeaderValue(conn, "Host");
		if (!host.has_value() || !IsAllowedHost(host.value(), serverPort)) {
			return false;
		}

		const std::optional<std::string> origin = HTTPUtil::GetHeaderValue(conn, "Origin");
		if (origin.has_value() && !IsAllowedOrigin(origin.value(), serverPort)) {
			return false;
		}

		return true;
	}
}
