#include <catch.hpp>

#include "Test_NrdSupport.h"

#include <P2P/Sync/StateSyncer.h>

#include <filesystem.h>
#include <string>

using namespace NrdTest;

TEST_CASE("StateSyncer flushed-vs-confirmed invariant fails closed")
{
	const BlockHeaderPtr pA = MakeHeader(4, 10, HASH::ZERO, 1, 1);
	const BlockHeaderPtr pB = MakeHeader(4, 10, HASH::ZERO, 1, 2);
	const BlockHeaderPtr pLower = MakeHeader(4, 9, HASH::ZERO, 1, 3);

	std::string reason;
	REQUIRE(StateSyncer::FlushedMatchesConfirmed(pA, pA, &reason));
	REQUIRE(reason.empty());
	REQUIRE_FALSE(StateSyncer::FlushedMatchesConfirmed(pA, pB, &reason));
	REQUIRE_FALSE(reason.empty());
	REQUIRE_FALSE(StateSyncer::FlushedMatchesConfirmed(pA, pLower, &reason));
	REQUIRE_FALSE(StateSyncer::FlushedMatchesConfirmed(nullptr, pA, &reason));
	REQUIRE_FALSE(StateSyncer::FlushedMatchesConfirmed(pA, nullptr, &reason));
}

TEST_CASE("StateSyncer recovery marker probe fails closed without mutation")
{
	const fs::path root = fs::temp_directory_path() / "state-syncer-marker-failclosed";
	std::error_code error;
	fs::remove_all(root, error);
	fs::create_directories(root / "marker_is_dir", error);
	REQUIRE_FALSE(error);

	// A directory at the marker path cannot be used as the persistent bound.
	REQUIRE_FALSE(StateSyncer::IsRecoveryMarkerPathWritable(root / "marker_is_dir"));

	// A normal marker path is writable and leaves no probe/final artifact.
	const fs::path markerPath = root / "recovery_failed";
	REQUIRE(StateSyncer::IsRecoveryMarkerPathWritable(markerPath));
	REQUIRE_FALSE(fs::exists(markerPath));
	REQUIRE_FALSE(fs::exists(fs::path(markerPath.string() + ".write_probe")));

	fs::remove_all(root, error);
}
