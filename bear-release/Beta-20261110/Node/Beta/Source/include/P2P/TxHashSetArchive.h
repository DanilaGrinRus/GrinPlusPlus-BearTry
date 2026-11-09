#pragma once

#include <Core/Models/BlockHeader.h>
#include <filesystem.h>
#include <string>

// Persistent txhashset archives. A node that has just activated a state at the
// archive horizon can snapshot it without any rewind, so the archive is saved
// and later served by header hash. This mirrors the Rust node keeping an archive
// at the archive height instead of rebuilding historical state on every request.
namespace TxHashSetArchive
{
	inline fs::path Dir(const fs::path& txHashSetPath)
	{
		return txHashSetPath / "archive";
	}

	inline fs::path Path(const fs::path& txHashSetPath, const BlockHeader& header)
	{
		return Dir(txHashSetPath) /
			("TxHashSet." + std::to_string(header.GetHeight()) + "." +
				header.ShortHash() + ".zip");
	}

	inline fs::path Path(const fs::path& txHashSetPath, const BlockHeaderPtr& pHeader)
	{
		return pHeader != nullptr ? Path(txHashSetPath, *pHeader) : fs::path();
	}
}
