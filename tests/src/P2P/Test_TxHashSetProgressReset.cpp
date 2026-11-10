#include <catch.hpp>

#include <P2P/SyncStatus.h>

// Regression for the task-64 stale-status finding: after an interrupted
// TxHashSet archive transfer /v1/status.state.download_size still reported the
// failed attempt's declared size (64 MiB) while downloaded was 0. Failure and
// abort paths now call SyncStatus::ClearTxHashSetProgress().
TEST_CASE("TxHashSet progress is cleared after a failed transfer")
{
	SyncStatus status;
	status.UpdateStatus(ESyncStatus::SYNCING_TXHASHSET);
	status.UpdateDownloaded(0);
	status.UpdateDownloadSize(64ULL * 1024 * 1024);
	status.UpdateProcessingStatus(3);

	REQUIRE(status.GetDownloaded() == 0);
	REQUIRE(status.GetDownloadSize() == 64ULL * 1024 * 1024);
	REQUIRE(status.GetProcessingStatus() == 3);

	status.ClearTxHashSetProgress();

	REQUIRE(status.GetDownloaded() == 0);
	REQUIRE(status.GetDownloadSize() == 0);
	REQUIRE(status.GetProcessingStatus() == 0);
	// Only the transfer counters are cleared. Sync status and request identity
	// must be untouched so retry/rotation/ban behaviour does not change.
	REQUIRE(status.GetStatus() == ESyncStatus::SYNCING_TXHASHSET);
}

TEST_CASE("TxHashSet progress clear keeps a partially downloaded size consistent")
{
	SyncStatus status;
	status.UpdateStatus(ESyncStatus::SYNCING_TXHASHSET);
	status.UpdateDownloaded(2ULL * 1024 * 1024);
	status.UpdateDownloadSize(64ULL * 1024 * 1024);

	// Abort mid-transfer: reported progress must not claim a target size for a
	// transfer that was abandoned at 2 MiB.
	status.ClearTxHashSetProgress();
	status.UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);

	REQUIRE(status.GetDownloaded() == 0);
	REQUIRE(status.GetDownloadSize() == 0);
	REQUIRE(status.GetStatus() == ESyncStatus::TXHASHSET_SYNC_FAILED);
}

TEST_CASE("TxHashSet progress clear is safe before any transfer")
{
	SyncStatus status;
	status.ClearTxHashSetProgress();
	REQUIRE(status.GetDownloaded() == 0);
	REQUIRE(status.GetDownloadSize() == 0);
	REQUIRE(status.GetProcessingStatus() == 0);
}
