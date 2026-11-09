#pragma once

#include <filesystem.h>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

// Best-effort durability for append-only chain files. std::ofstream::flush only
// pushes bytes into the OS cache; the checkpoint must be published only after
// the preceding store writes are durable, otherwise a power loss can leave the
// checkpoint describing data that never reached the disk. Directory fsync is not
// exposed on Windows; the files themselves are flushed.
namespace FileSync
{
	inline bool Flush(const fs::path& path)
	{
		std::error_code ec;
		if (!fs::exists(path, ec)) {
			return true;
		}

#ifdef _WIN32
		const HANDLE handle = CreateFileW(
			path.wstring().c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			return false;
		}

		const BOOL flushed = FlushFileBuffers(handle);
		CloseHandle(handle);
		return flushed != FALSE;
#else
		return true;
#endif
	}
}
