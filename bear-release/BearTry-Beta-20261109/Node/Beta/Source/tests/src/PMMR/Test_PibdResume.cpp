#include <catch.hpp>

#include <Common/Util/FileUtil.h>
#include <Crypto/Models/Hash.h>
#include <P2P/PIBD/PibdResume.h>
#include <P2P/PIBD/PibdSync.h>

#include <filesystem.h>
#include <vector>

namespace
{
	PibdDownload::Horizon MakeHorizon(const uint8_t fill)
	{
		PibdDownload::Horizon horizon;
		std::vector<uint8_t> bytes(32, fill);
		horizon.BlockHash = Hash(bytes);
		horizon.Height = 3313440;
		horizon.OutputMmrSize = 6684;
		horizon.KernelMmrSize = 123;
		horizon.OutputRoot = Hash(std::vector<uint8_t>(32, (uint8_t)(fill + 1)));
		horizon.RangeProofRoot = Hash(std::vector<uint8_t>(32, (uint8_t)(fill + 2)));
		horizon.KernelRoot = Hash(std::vector<uint8_t>(32, (uint8_t)(fill + 3)));
		return horizon;
	}
}

TEST_CASE("PibdResume::JournalsAcceptedSegmentsInReplayOrder")
{
	const fs::path root = fs::temp_directory_path() / "pibd-resume-journal";
	std::error_code error;
	fs::remove_all(root, error);

	const PibdDownload::Horizon horizon = MakeHorizon(7);
	{
		PibdResume resume(root);
		REQUIRE_FALSE(resume.Prepare(horizon)); // fresh journal

		resume.Store(MessageTypes::OutputSegment, { 4, 5, 6 });
		resume.Store(MessageTypes::OutputBitmapSegment, { 1, 2, 3 });
		resume.Store(MessageTypes::KernelSegment, { 9 });
	}

	// Simulated restart: a new instance over the same directory sees the journal.
	PibdResume reopened(root);
	REQUIRE(reopened.Prepare(horizon));

	std::vector<std::pair<MessageTypes::EMessageType, std::vector<uint8_t>>> replayed;
	reopened.LoadAll([&replayed](const MessageTypes::EMessageType type, const std::vector<uint8_t>& payload) {
		replayed.emplace_back(type, payload);
	});

	REQUIRE(replayed.size() == 3);
	REQUIRE(replayed[0].first == MessageTypes::OutputBitmapSegment);
	REQUIRE(replayed[0].second == std::vector<uint8_t>{ 1, 2, 3 });
	REQUIRE(replayed[1].first == MessageTypes::OutputSegment);
	REQUIRE(replayed[1].second == std::vector<uint8_t>{ 4, 5, 6 });
	REQUIRE(replayed[2].first == MessageTypes::KernelSegment);
	REQUIRE(replayed[2].second == std::vector<uint8_t>{ 9 });

	// A different horizon invalidates the journal.
	const PibdDownload::Horizon other = MakeHorizon(9);
	REQUIRE_FALSE(reopened.Prepare(other));
	std::vector<size_t> after;
	reopened.LoadAll([&after](const MessageTypes::EMessageType, const std::vector<uint8_t>&) {
		after.push_back(1);
	});
	REQUIRE(after.empty());

	reopened.Clear();
	fs::remove_all(root, error);
}

TEST_CASE("PibdSync::StartCreatesResumeManifest")
{
	const fs::path root = fs::temp_directory_path() / "pibd-sync-resume-start";
	std::error_code error;
	fs::remove_all(root, error);

	const fs::path live = root / "txhashset";
	const fs::path staging = root / "pibd_staging";
	const fs::path resumeDir = root / "pibd_resume";

	PibdSync sync(live, staging, resumeDir);
	const PibdDownload::Horizon horizon = MakeHorizon(11);
	const PibdSync::Clock::time_point now = PibdSync::Clock::now();
	REQUIRE(sync.Start(horizon, now));
	REQUIRE(sync.GetStatus() == PibdSync::EStatus::Downloading);
	REQUIRE(FileUtil::Exists(resumeDir / "manifest"));

	// Reset keeps the journal for a later attempt at the same horizon.
	sync.Reset();
	REQUIRE(sync.Start(horizon, now));
	REQUIRE(sync.GetStatus() == PibdSync::EStatus::Downloading);

	fs::remove_all(root, error);
}
