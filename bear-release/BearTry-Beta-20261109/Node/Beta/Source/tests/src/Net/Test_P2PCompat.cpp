#include <catch.hpp>

#include <GrinVersion.h>
#include <P2P/AdvertisedTip.h>
#include <P2P/Capabilities.h>
#include "../../../src/P2P/Messages/MessageHeader.h"
#include <P2P/Common.h>
#include <P2P/Messages/MessageTypes.h>
#include <memory>

// Wire-format pins for Grin v5.5.2 compatibility. These values are consensus on
// the network boundary: changing any of them silently breaks interop.
TEST_CASE("GrinVersion::MatchesRelease")
{
	// The repository is Grin++ v1.2.9; do not pretend to be the Rust reference.
	REQUIRE(GRINPP_VERSION == "1.2.9");
	REQUIRE(P2P::USER_AGENT == "Grin++ 1.2.9");
}

TEST_CASE("Capabilities::WireFormat")
{
	REQUIRE((uint32_t)Capabilities::HEADER_HIST == 0x01);
	REQUIRE((uint32_t)Capabilities::FULL_HIST == 0x01);
	REQUIRE((uint32_t)Capabilities::TXHASHET_HIST == 0x02);
	REQUIRE((uint32_t)Capabilities::PEER_LIST == 0x04);
	REQUIRE((uint32_t)Capabilities::TX_KERNEL_HASH == 0x08);
	REQUIRE((uint32_t)Capabilities::PIBD_HIST == 0x10);
	REQUIRE((uint32_t)Capabilities::BLOCK_HIST == 0x20);
	REQUIRE((uint32_t)Capabilities::PIBD_HIST_1 == 0x40);
	REQUIRE((uint32_t)Capabilities::PIHD_HIST == 0x80);

	// Archive serving is not integrated into the normal node yet. Do not claim
	// PIBD capability while a normal Rust peer would select a different horizon.
	REQUIRE_FALSE(Capabilities(Capabilities::ADVERTISED).HasCapability(Capabilities::PIBD_HIST));
	REQUIRE_FALSE(Capabilities(Capabilities::ADVERTISED).HasCapability(Capabilities::PIBD_HIST_1));
	REQUIRE(!Capabilities(Capabilities::ADVERTISED).HasCapability(Capabilities::BLOCK_HIST));

	// Grin 5.5.2 has no PIHD messages (its enum ends at KernelSegment = 28), so
	// the extension bit must never be advertised; header sync stays legacy.
	REQUIRE(!Capabilities(Capabilities::ADVERTISED).HasCapability(Capabilities::PIHD_HIST));
	REQUIRE(Capabilities(Capabilities::PIBD_HIST_1 | Capabilities::PIHD_HIST)
		.HasCapability(Capabilities::PIBD_HIST_1));
	REQUIRE(Capabilities(Capabilities::PIBD_HIST_1 | Capabilities::PIHD_HIST)
		.HasCapability(Capabilities::PIHD_HIST));
}

TEST_CASE("Capabilities::AdvertisedGating")
{
	// G-09 capability contract: the compile-time mask stays conservative; PIBD
	// bits are added only when the serving component reports a validated ready
	// archive for the current deterministic horizon. Never from file presence.
	REQUIRE_FALSE(Capabilities::Advertised(false).HasCapability(Capabilities::PIBD_HIST));
	REQUIRE_FALSE(Capabilities::Advertised(false).HasCapability(Capabilities::PIBD_HIST_1));
	REQUIRE_FALSE(Capabilities::Advertised(false).HasCapability(Capabilities::BLOCK_HIST));
	REQUIRE(Capabilities::Advertised(false).HasCapability(Capabilities::HEADER_HIST));
	REQUIRE(Capabilities::Advertised(false).HasCapability(Capabilities::TXHASHET_HIST));
	REQUIRE(Capabilities::Advertised(false).HasCapability(Capabilities::PEER_LIST));

	REQUIRE(Capabilities::Advertised(true).HasCapability(Capabilities::PIBD_HIST));
	REQUIRE(Capabilities::Advertised(true).HasCapability(Capabilities::PIBD_HIST_1));
	REQUIRE_FALSE(Capabilities::Advertised(true).HasCapability(Capabilities::BLOCK_HIST));
	REQUIRE_FALSE(Capabilities::Advertised(true).HasCapability(Capabilities::PIHD_HIST));
	REQUIRE(Capabilities::Advertised(true).HasCapability(Capabilities::HEADER_HIST));
}

TEST_CASE("MessageTypes::PibdIds")
{
	REQUIRE((int)MessageTypes::GetOutputBitmapSegment == 21);
	REQUIRE((int)MessageTypes::OutputBitmapSegment == 22);
	REQUIRE((int)MessageTypes::GetOutputSegment == 23);
	REQUIRE((int)MessageTypes::OutputSegment == 24);
	REQUIRE((int)MessageTypes::GetRangeProofSegment == 25);
	REQUIRE((int)MessageTypes::RangeProofSegment == 26);
	REQUIRE((int)MessageTypes::GetKernelSegment == 27);
	REQUIRE((int)MessageTypes::KernelSegment == 28);
}

TEST_CASE("MessageTypes::MaximumSizes")
{
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::Hand) == 128);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::Shake) == 88);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::GetHeaders) == 1 + 32 * P2P::MAX_LOCATORS);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::Headers) == 2 + 365 * P2P::MAX_BLOCK_HEADERS);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::TxHashSetRequest) == 40);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::TxHashSetArchive) == 64);

	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::GetOutputSegment) == 41);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::GetKernelSegment) == 41);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::OutputSegment) == 2 * P2P::MAX_BLOCK_SIZE);
	REQUIRE(MessageTypes::GetMaximumSize(MessageTypes::KernelSegment) == 2 * P2P::MAX_BLOCK_SIZE);
}

namespace
{
	// Deterministic single-tip view: both accessors belong to one immutable
	// header snapshot, mirroring BlockHeaderPtr.
	struct StubTipHeader
	{
		uint64_t Height = 0;
		uint64_t TotalDifficulty = 0;

		uint64_t GetHeight() const { return Height; }
		uint64_t GetTotalDifficulty() const { return TotalDifficulty; }
	};
}

// Regression for the G-09 L1 stall (docs/modernization/G09_ARCHIVE_SERVING.md
// §8.9): advertising the confirmed body tip while serving candidate headers let
// a syncing peer overshoot the advertised difficulty and stop. Hand/Shake and
// Ping/Pong must advertise the header head (CANDIDATE), never the body tip.
TEST_CASE("P2P::AdvertisedTipUsesHeaderHead")
{
	SyncStatus syncStatus;
	syncStatus.UpdateHeaderStatus(207, 2500);
	syncStatus.UpdateBlockStatus(100, 1000, 0);

	const P2P::AdvertisedTip tip = P2P::GetAdvertisedTip(syncStatus);
	REQUIRE(tip.Height == 207);
	REQUIRE(tip.TotalDifficulty == 2500);
	REQUIRE(P2P::GetAdvertisedDifficulty(syncStatus) == 2500);
	REQUIRE(syncStatus.GetBlockDifficulty() == 1000);

	// Both advertised fields move together: a reader can never see the new
	// height with the old difficulty (or vice versa).
	syncStatus.UpdateHeaderStatus(208, 2600);
	const HeaderTip headerTip = syncStatus.GetHeaderTip();
	REQUIRE(headerTip.Height == 208);
	REQUIRE(headerTip.TotalDifficulty == 2600);
	const P2P::AdvertisedTip next = P2P::GetAdvertisedTip(syncStatus);
	REQUIRE(next.Height == 208);
	REQUIRE(next.TotalDifficulty == 2600);
}

// Controlled snapshot test: the tip changes *between* the two reads a split
// implementation would make. The provider is invoked exactly once and the
// returned pair must come entirely from the first snapshot; a second read would
// have seen snapshot B's height and/or difficulty.
TEST_CASE("P2P::AdvertisedTipReadsOneSnapshot")
{
	auto snapshotA = std::make_shared<StubTipHeader>(StubTipHeader{ 100, 1000 });
	auto snapshotB = std::make_shared<StubTipHeader>(StubTipHeader{ 207, 2500 });
	std::shared_ptr<StubTipHeader> current = snapshotA;
	int providerCalls = 0;
	auto provider = [&]() {
		++providerCalls;
		auto snapshot = current;
		current = snapshotB; // candidate tip advances after this snapshot is taken
		return snapshot;
	};

	const auto tip = P2P::ReadAdvertisedTip(provider);
	REQUIRE(tip.has_value());
	REQUIRE(providerCalls == 1);
	REQUIRE(tip->Height == snapshotA->GetHeight());
	REQUIRE(tip->TotalDifficulty == snapshotA->GetTotalDifficulty());
	REQUIRE(tip->Height != snapshotB->GetHeight());
	REQUIRE(tip->TotalDifficulty != snapshotB->GetTotalDifficulty());

	// No readable tip => nullopt, so the caller withholds the message.
	std::shared_ptr<StubTipHeader> none;
	const auto withheld = P2P::ReadAdvertisedTip([&]() { return none; });
	REQUIRE_FALSE(withheld.has_value());
}

TEST_CASE("Wrong network magic reports both received and expected bytes")
{
    ByteBuffer buffer(std::vector<uint8_t>{ 0x47, 0x45, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
    bool rejectedWithDetails = false;
    try {
        (void)MessageHeader::Deserialize(buffer);
    } catch (const std::exception& e) {
        const std::string message(e.what());
        rejectedWithDetails = message.find("received=4745") != std::string::npos &&
            message.find("expected=") != std::string::npos;
    }
    REQUIRE(rejectedWithDetails);
}