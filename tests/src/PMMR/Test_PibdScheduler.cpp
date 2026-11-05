#include <catch.hpp>

#include <Common/Util/BitUtil.h>
#include <Core/Models/Fee.h>
#include <Core/Models/Features.h>
#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/RangeProof.h>
#include <Crypto/Models/Signature.h>
#include <P2P/PIBD/PibdParams.h>
#include <P2P/PIBD/PibdScheduler.h>
#include <PMMR/Common/MMRUtil.h>
#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/InMemoryPmmr.h>
#include <PMMR/Models/Segment.h>
#include <PMMR/Models/SegmentException.h>
#include <PMMR/Models/SegmentGenerator.h>
#include <PMMR/Models/SegmentIdentifier.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	Commitment MakeCommitment(const uint8_t seed)
	{
		std::vector<uint8_t> bytes(33, seed);
		return Commitment(CBigInteger<33>(std::move(bytes)));
	}

	Signature MakeSignature(const uint8_t seed)
	{
		std::vector<uint8_t> bytes(64, seed);
		return Signature(CBigInteger<64>(std::move(bytes)));
	}

	OutputIdentifier MakeOutput(const uint8_t seed)
	{
		return OutputIdentifier(EOutputFeatures::DEFAULT, MakeCommitment(seed));
	}

	RangeProof MakeRangeProof(const uint8_t seed)
	{
		return RangeProof(std::vector<unsigned char>(16, seed));
	}

	TransactionKernel MakeKernel(const uint8_t seed)
	{
		return TransactionKernel(
			EKernelFeatures::DEFAULT_KERNEL, Fee(0, 0), 0, MakeCommitment(seed), MakeSignature(seed));
	}

	uint64_t MmrSizeForLeaves(const uint64_t numLeaves)
	{
		return 2 * numLeaves - (uint64_t)BitUtil::CountBitsSet(numLeaves);
	}

	template<class T, class TFactory>
	InMemoryPmmr<T> MakeTree(const uint64_t numLeaves, TFactory factory)
	{
		std::vector<T> leaves;
		leaves.reserve((size_t)numLeaves);
		for (uint64_t i = 0; i < numLeaves; ++i) {
			leaves.push_back(factory((uint8_t)(i + 1)));
		}

		return InMemoryPmmr<T>(std::move(leaves));
	}

	// A complete synthetic horizon: bitmap with two unspent leaves, 120 outputs,
	// 8 kernels.
	struct PibdFixture
	{
		VectorSegmentBitmap Bitmap;
		InMemoryPmmr<BitmapChunk> BitmapMmr;
		InMemoryPmmr<OutputIdentifier> Outputs;
		InMemoryPmmr<RangeProof> RangeProofs;
		InMemoryPmmr<TransactionKernel> Kernels;
		PibdDownload::Horizon Horizon;

		PibdFixture()
			: Bitmap(120),
			BitmapMmr(MakeBitmapMmr()),
			Outputs(MakeTree<OutputIdentifier>(120, MakeOutput)),
			RangeProofs(MakeTree<RangeProof>(120, MakeRangeProof)),
			Kernels(MakeTree<TransactionKernel>(8, MakeKernel))
		{
			Bitmap.Set(0, true);
			Bitmap.Set(70, true);
			Horizon.BlockHash = Hash::ValueOf(42);
			Horizon.Height = 7000;
			Horizon.OutputMmrSize = Outputs.GetSize();
			Horizon.KernelMmrSize = Kernels.GetSize();
			Horizon.OutputRoot = MMRHashUtil::HashParentWithIndex(Outputs.GetRoot(), BitmapMmr.GetRoot(), Outputs.GetSize());
			Horizon.RangeProofRoot = RangeProofs.GetRoot();
			Horizon.KernelRoot = Kernels.GetRoot();
					}

		static InMemoryPmmr<BitmapChunk> MakeBitmapMmr()
		{
			std::vector<BitmapChunk> chunks(1);
			chunks[0].Set(0, true);
			chunks[0].Set(70, true);
			return InMemoryPmmr<BitmapChunk>(std::move(chunks));
		}

		BitmapSegment BitmapResponse() const
		{
			return BitmapSegment::FromSegment(SegmentGenerator<BitmapChunk>::Generate(
				BitmapMmr, SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0)));
		}

		Segment<OutputIdentifier> OutputResponse() const
		{
			return SegmentGenerator<OutputIdentifier>::Generate(
				Outputs, SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, 0), &Bitmap);
		}

		Segment<RangeProof> RangeProofResponse() const
		{
			return SegmentGenerator<RangeProof>::Generate(
				RangeProofs, SegmentIdentifier(PibdParams::RANGEPROOF_SEGMENT_HEIGHT, 0), &Bitmap);
		}

		Segment<TransactionKernel> KernelResponse() const
		{
			return SegmentGenerator<TransactionKernel>::Generate(
				Kernels, SegmentIdentifier(PibdParams::KERNEL_SEGMENT_HEIGHT, 0));
		}
	};

	PibdScheduler::Config TightConfig()
	{
		PibdScheduler::Config config;
		config.MaxInFlight = 1;
		config.Timeout = std::chrono::seconds(20);
		config.MaxAttempts = 2;
		config.FallbackAfter = std::chrono::seconds(660);
		return config;
	}
}

TEST_CASE("PibdScheduler::InFlightLimit")
{
	const PibdFixture fixture;
	const Clock::time_point start;
	PibdScheduler scheduler(fixture.Horizon, start, TightConfig());

	const std::vector<IMessagePtr> first = scheduler.TakeRequests(start);
	REQUIRE(first.size() == 1);
	REQUIRE(first[0]->GetMessageType() == MessageTypes::GetOutputBitmapSegment);
	REQUIRE(scheduler.GetInFlightCount() == 1);

	// Nothing else goes out while the single slot is busy.
	REQUIRE(scheduler.TakeRequests(start).empty());
	REQUIRE(scheduler.GetInFlightCount() == 1);
	REQUIRE(scheduler.GetTotalRequestsSent() == 1);
}

TEST_CASE("PibdScheduler::RetriesThenFails")
{
	const PibdFixture fixture;
	const Clock::time_point start;
	PibdScheduler scheduler(fixture.Horizon, start, TightConfig());

	REQUIRE(scheduler.TakeRequests(start).size() == 1);                  // attempt 1
	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(10)).empty());   // still in flight
	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(21)).size() == 1); // attempt 2
	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(62)).size() == 1); // attempt 3
	REQUIRE(!scheduler.IsFailed());
	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(45)).empty());     // attempt 3 refused
	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(661)).empty());
	REQUIRE(scheduler.IsFailed());
	REQUIRE(!scheduler.GetFailure().empty());
}

TEST_CASE("PibdScheduler::SlowPeerRateFails")
{
	const PibdFixture fixture;
	const Clock::time_point start;
	PibdScheduler::Config config = TightConfig();
	config.RateWindow = std::chrono::seconds(30);
	config.MinSegmentsPerWindow = 2;
	PibdScheduler scheduler(fixture.Horizon, start, config);

	REQUIRE(scheduler.TakeRequests(start).size() == 1);
	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(31)).empty());
	REQUIRE(scheduler.IsFailed());
	REQUIRE(scheduler.GetFailure().find("rate") != std::string::npos);
}
TEST_CASE("PibdScheduler::CompletesThroughMessages")
{
	const PibdFixture fixture;
	const Clock::time_point start;
	PibdScheduler scheduler(fixture.Horizon, start, TightConfig());

	// Bitmap.
	REQUIRE(scheduler.TakeRequests(start).size() == 1);
	const SegmentResponseMessage bitmapResponse = SegmentResponseMessage::CreateOutputBitmap(
		fixture.Horizon.BlockHash, fixture.BitmapResponse(), fixture.Outputs.GetRoot());
	REQUIRE(scheduler.OnResponse(bitmapResponse, start + std::chrono::seconds(1)));
	REQUIRE(scheduler.GetInFlightCount() == 0);

	// A duplicate bitmap response is stale (bitmap phase is over) and changes nothing.
	REQUIRE(!scheduler.OnResponse(bitmapResponse, start + std::chrono::seconds(2)));

	// Output, rangeproof and kernel, one at a time.
	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(2))[0]->GetMessageType() ==
		MessageTypes::GetOutputSegment);
	REQUIRE(scheduler.OnResponse(
		SegmentResponseMessage::CreateOutputs(
			fixture.Horizon.BlockHash, fixture.OutputResponse(), fixture.BitmapMmr.GetRoot()),
		start + std::chrono::seconds(3)));

	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(3))[0]->GetMessageType() ==
		MessageTypes::GetRangeProofSegment);
	REQUIRE(scheduler.OnResponse(
		SegmentResponseMessage::CreateRangeProofs(
			fixture.Horizon.BlockHash, fixture.RangeProofResponse()),
		start + std::chrono::seconds(4)));

	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(4))[0]->GetMessageType() ==
		MessageTypes::GetKernelSegment);
	REQUIRE(scheduler.OnResponse(
		SegmentResponseMessage::CreateKernels(
			fixture.Horizon.BlockHash, fixture.KernelResponse()),
		start + std::chrono::seconds(5)));

	REQUIRE(scheduler.IsComplete());
	REQUIRE(!scheduler.IsFailed());
	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(6)).empty());
	REQUIRE(scheduler.GetSession().GetDownload().GetReceivedCount() == 4);
}

TEST_CASE("PibdScheduler::TimeoutReissuesTheSameSegment")
{
	const PibdFixture fixture;
	const Clock::time_point start;
	PibdScheduler scheduler(fixture.Horizon, start, TightConfig());

	REQUIRE(scheduler.TakeRequests(start).size() == 1);
	REQUIRE(scheduler.OnResponse(
		SegmentResponseMessage::CreateOutputBitmap(
			fixture.Horizon.BlockHash, fixture.BitmapResponse(), fixture.Outputs.GetRoot()),
		start));

	const std::vector<IMessagePtr> outputRequest = scheduler.TakeRequests(start);
	REQUIRE(outputRequest.size() == 1);

	// The peer never answers: after the timeout the same output segment is asked
	// for again (attempt 2), and this time it is accepted.
	const std::vector<IMessagePtr> retry = scheduler.TakeRequests(start + std::chrono::seconds(21));
	REQUIRE(retry.size() == 1);
	REQUIRE(retry[0]->GetMessageType() == MessageTypes::GetOutputSegment);
	REQUIRE(scheduler.GetInFlightCount() == 1);

	REQUIRE(scheduler.OnResponse(
		SegmentResponseMessage::CreateOutputs(
			fixture.Horizon.BlockHash, fixture.OutputResponse(), fixture.BitmapMmr.GetRoot()),
		start + std::chrono::seconds(22)));
	REQUIRE(scheduler.GetInFlightCount() == 0);
}

TEST_CASE("PibdScheduler::FallbackWhenNoProgress")
{
	const PibdFixture fixture;
	const Clock::time_point start;
	PibdScheduler scheduler(fixture.Horizon, start, TightConfig());

	REQUIRE(scheduler.TakeRequests(start).size() == 1);
	REQUIRE(scheduler.TakeRequests(start + std::chrono::seconds(661)).empty());
	REQUIRE(scheduler.IsFailed());
	REQUIRE(scheduler.GetFailure().find("progress") != std::string::npos);
}

TEST_CASE("PibdScheduler::MultiPeerAssignmentsRespectGlobalAndPerPeerLimits")
{
	const PibdFixture fixture;
	const Clock::time_point start;
	PibdScheduler::Config config = TightConfig();
	config.MaxInFlight = 4;
	config.Timeout = std::chrono::seconds(20);
	PibdScheduler scheduler(fixture.Horizon, start, config);

	const std::vector<std::string> peers = { "peer-a", "peer-b" };
	const auto first = scheduler.TakeRequestsForPeers(start, peers, 1);
	REQUIRE(first.size() == 1);
	REQUIRE(scheduler.GetPeerInFlight(first[0].Peer) == 1);
	REQUIRE(scheduler.TakeRequestsForPeers(start, peers, 1).empty());

	// A failed send releases only its own assignment; it can be retried.
	REQUIRE(scheduler.ReleaseAssignment(first[0]));
	REQUIRE(scheduler.GetPeerInFlight(first[0].Peer) == 0);
	const auto retry = scheduler.TakeRequestsForPeers(start, peers, 1);
	REQUIRE(retry.size() == 1);
	REQUIRE(retry[0].Peer == first[0].Peer);
}

TEST_CASE("PibdScheduler::MultiPeerWrongPeerAndLateResponses")
{
	const PibdFixture fixture;
	const Clock::time_point start;
	PibdScheduler::Config config = TightConfig();
	config.MaxInFlight = 1;
	config.Timeout = std::chrono::seconds(20);
	PibdScheduler scheduler(fixture.Horizon, start, config);

	const auto assigned = scheduler.TakeRequestsForPeers(start, { "peer-a", "peer-b" }, 1);
	REQUIRE(assigned.size() == 1);

	// A response from a peer that does not own the assignment is rejected and
	// does not consume/lose the segment.
	REQUIRE_FALSE(scheduler.OnResponse(
		SegmentResponseMessage::CreateOutputBitmap(
			fixture.Horizon.BlockHash, fixture.BitmapResponse(), fixture.Outputs.GetRoot()),
		start + std::chrono::seconds(1), "wrong-peer"));
	REQUIRE(scheduler.GetWrongPeerResponses() == 1);
	REQUIRE(scheduler.GetInFlightCount() == 1);
	REQUIRE(scheduler.GetReceivedSegments() == 0);

	// The assigned peer's response is accepted.
	REQUIRE(scheduler.OnResponse(
		SegmentResponseMessage::CreateOutputBitmap(
			fixture.Horizon.BlockHash, fixture.BitmapResponse(), fixture.Outputs.GetRoot()),
		start + std::chrono::seconds(2), assigned[0].Peer));
	REQUIRE(scheduler.GetInFlightCount() == 0);
	REQUIRE(scheduler.GetReceivedSegments() == 1);

	// A duplicate is stale, not a failure and not a second segment.
	REQUIRE_FALSE(scheduler.OnResponse(
		SegmentResponseMessage::CreateOutputBitmap(
			fixture.Horizon.BlockHash, fixture.BitmapResponse(), fixture.Outputs.GetRoot()),
		start + std::chrono::seconds(3), assigned[0].Peer));

	// No pending assignment: a validated late response is accepted (progress).
	REQUIRE(scheduler.OnResponse(
		SegmentResponseMessage::CreateOutputs(
			fixture.Horizon.BlockHash, fixture.OutputResponse(), fixture.BitmapMmr.GetRoot()),
		start + std::chrono::seconds(4), "peer-a"));
	REQUIRE(scheduler.GetLateResponses() == 1);
}

TEST_CASE("PibdScheduler::MultiPeerDisconnectReleasesAndReassigns")
{
	const PibdFixture fixture;
	const Clock::time_point start;
	PibdScheduler::Config config = TightConfig();
	config.MaxInFlight = 1;
	config.Timeout = std::chrono::seconds(20);
	PibdScheduler scheduler(fixture.Horizon, start, config);

	const auto assigned = scheduler.TakeRequestsForPeers(start, { "peer-a" }, 1);
	REQUIRE(assigned.size() == 1);
	REQUIRE(scheduler.ReleasePeer("peer-a") == 1);
	REQUIRE(scheduler.GetPeerInFlight("peer-a") == 0);

	const auto reassigned = scheduler.TakeRequestsForPeers(start, { "peer-b" }, 1);
	REQUIRE(reassigned.size() == 1);
	REQUIRE(reassigned[0].Peer == "peer-b");
}

