#pragma once

#include <vector>
#include <filesystem.h>
#include <Common/GrinStr.h>

class FileUtil
{
public:
	static bool Exists(const fs::path& path) noexcept;

	static bool ReadFile(const fs::path& filePath, std::vector<uint8_t>& data);
	static void RenameFile(const fs::path& source, const fs::path& destination);
	static void SafeWriteToFile(const fs::path& filePath, const std::vector<uint8_t>& data);
	static void WriteTextToFile(const fs::path& filePath, const std::string& text);
	static bool RemoveFile(const fs::path& filePath) noexcept;
	static bool TruncateFile(const fs::path& filePath, const uint64_t size);
	static size_t GetFileSize(const fs::path& file);

	static void CopyDirectory(const fs::path& sourceDir, const fs::path& destDir);
	static bool CreateDirectories(const fs::path& directory) noexcept;
	static std::vector<GrinStr> GetSubDirectories(const fs::path& filePath, const bool includeHidden);

	//
	// Restricts an existing file/directory DACL to the current owner, SYSTEM and
	// Administrators (Windows). Removes inherited ACEs. Best-effort: returns
	// false on failure; callers log and decide whether to fail closed.
	// No-op on non-Windows builds at this stage.
	//
	static bool HardenPermissions(const fs::path& path) noexcept;

	static fs::path GetHomeDirectory();
};
