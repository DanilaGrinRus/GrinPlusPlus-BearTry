#include <catch.hpp>

#include <Common/Util/BitUtil.h>
#include <Core/Models/Fee.h>
#include <Core/Models/Features.h>
#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/Hash.h>
#include <Crypto/Models/RangeProof.h>
#include <Crypto/Models/Signature.h>
#include <P2P/PIBD/PibdParams.h>
#include <P2P/PIBD/PibdSync.h>
#include <PMMR/Common/HashFile.h>
#include <PMMR/Common/MMRHashUtil.h>
#include <PMMR/Common/MMRUtil.h>
#include <PMMR/Common/PruneList.h>
#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/InMemoryPmmr.h>
#include <PMMR/Models/SegmentGenerator.h>
#include <PMMR/Models/SegmentIdentifier.h>

#include <filesystem.h>

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
		return RangeProof(std::vector<unsigned char>(675, seed));
	}

	TransactionKernel MakeKernel(const uint8_t seed)
	{
		return TransactionKernel(
			EKernelFeatures::DEFAULT_KERNEL, Fee(0, 0), 0, MakeCommitment(seed), MakeSignature(seed));
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

	struct Fixture
	{
		uint64_t NumOutputs = 32;
		VectorSegmentBitmap Bitmap;
		InMemoryPmmr<BitmapChunk> BitmapMmr;
		InMemoryPmmr<OutputIdentifier> Outputs;
		InMemoryPmmr<RangeProof> RangeProofs;
		InMemoryPmmr<TransactionKernel> Kernels;
		PibdDownload::Horizon Horizon;

		Fixture()
			: Bitmap(32),
			BitmapMmr(MakeBitmapMmr()),
			Outputs(MakeTree<OutputIdentifier>(32, MakeOutput)),
			RangeProofs(MakeTree<RangeProof>(32, MakeRangeProof)),
			Kernels(MakeTree<TransactionKernel>(2, MakeKernel))
		{
			Bitmap.Set(0, true);
			Horizon.BlockHash = Hash::ValueOf(55);
			Horizon.Height = 321;
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
			return InMemoryPmmr<BitmapChunk>(std::move(chunks));
		}
	};

	IMessagePtr ResponseFor(const Fixture& fixture, const SegmentRequestMessage& request)
	{
		switch (request.GetMessageType()) {
			case MessageTypes::GetOutputBitmapSegment:
				return std::make_shared<SegmentResponseMessage>(SegmentResponseMessage::CreateOutputBitmap(
					request.GetBlockHash(),
					BitmapSegment::FromSegment(SegmentGenerator<BitmapChunk>::Generate(
						fixture.BitmapMmr,
						SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0))),
					fixture.Outputs.GetRoot()));
			case MessageTypes::GetOutputSegment:
				return std::make_shared<SegmentResponseMessage>(SegmentResponseMessage::CreateOutputs(
					request.GetBlockHash(),
					SegmentGenerator<OutputIdentifier>::Generate(
						fixture.Outputs, request.GetIdentifier(), &fixture.Bitmap),
					fixture.Outputs.GetRoot()));
			case MessageTypes::GetRangeProofSegment:
				return std::make_shared<SegmentResponseMessage>(SegmentResponseMessage::CreateRangeProofs(
					request.GetBlockHash(),
					SegmentGenerator<RangeProof>::Generate(
						fixture.RangeProofs, request.GetIdentifier(), &fixture.Bitmap)));
			case MessageTypes::GetKernelSegment:
				return std::make_shared<SegmentResponseMessage>(SegmentResponseMessage::CreateKernels(
					request.GetBlockHash(),
					SegmentGenerator<TransactionKernel>::Generate(
						fixture.Kernels, request.GetIdentifier())));
			default:
				return nullptr;
		}
	}

	Hash RootOf(const fs::path& treeDir, const uint64_t mmrSize)
	{
		std::shared_ptr<HashFile> pHashFile = HashFile::Load(treeDir / "pmmr_hash.bin");
		std::shared_ptr<const PruneList> pPruneList = PruneList::Load(treeDir / "pmmr_prun.bin");
		return MMRHashUtil::Root(pHashFile, mmrSize, pPruneList);
	}
}

TEST_CASE("PibdSync::DownloadsWritesAndActivates")
{
	const Fixture fixture;
	const Clock::time_point start;

	const fs::path root = fs::temp_directory_path() / "pibd-sync-test";
	std::error_code error;
	fs::remove_all(root, error);
	FileUtil::CreateDirectories(root / "live");
	FileUtil::WriteTextToFile(root / "live" / "old_state.txt", "sentinel\n");

	PibdSync sync(root / "live", root / "staging", root / "resume");
	REQUIRE(sync.Start(fixture.Horizon, start));
	REQUIRE(sync.GetStatus() == PibdSync::EStatus::Downloading);

	// Drive the download the way the network layer would: ask for requests, answer
	// each with a valid segment, feed the response back.
	for (int round = 0; round < 10 && sync.GetStatus() == PibdSync::EStatus::Downloading; ++round) {
		const std::vector<IMessagePtr> requests = sync.TakeRequests(start + std::chrono::seconds(round));
		REQUIRE(!requests.empty());
		for (const IMessagePtr& pMessage : requests) {
			const std::shared_ptr<SegmentRequestMessage> pRequest =
				std::dynamic_pointer_cast<SegmentRequestMessage>(pMessage);
			REQUIRE(pRequest != nullptr);

			const IMessagePtr pResponse = ResponseFor(fixture, *pRequest);
			REQUIRE(pResponse != nullptr);
			REQUIRE(sync.OnResponse(
				*std::dynamic_pointer_cast<SegmentResponseMessage>(pResponse),
				start + std::chrono::seconds(round)));
		}
	}

	REQUIRE(sync.GetStatus() == PibdSync::EStatus::Completed);
	REQUIRE(sync.TakeRequests(start + std::chrono::seconds(20)).empty());

	// Write + activate, then verify the live state from its files.
	REQUIRE(sync.TryComplete());
	REQUIRE(sync.GetStatus() == PibdSync::EStatus::Activated);
	REQUIRE(RootOf(root / "live" / "output", fixture.Horizon.OutputMmrSize) == fixture.Outputs.GetRoot());
	REQUIRE(RootOf(root / "live" / "kernel", fixture.Horizon.KernelMmrSize) == fixture.Horizon.KernelRoot);
	REQUIRE(!FileUtil::Exists(root / "live" / "old_state.txt"));

	fs::remove_all(root, error);
}

TEST_CASE("PibdSync::BadSegmentFailsWithoutTouchingLiveState")
{
	const Fixture fixture;
	const Clock::time_point start;

	const fs::path root = fs::temp_directory_path() / "pibd-sync-bad";
	std::error_code error;
	fs::remove_all(root, error);
	FileUtil::CreateDirectories(root / "live");
	FileUtil::WriteTextToFile(root / "live" / "old_state.txt", "sentinel\n");

	PibdSync sync(root / "live", root / "staging", root / "resume");
	REQUIRE(sync.Start(fixture.Horizon, start));

	const std::vector<IMessagePtr> requests = sync.TakeRequests(start);
	REQUIRE(requests.size() == 1);
	const std::shared_ptr<SegmentRequestMessage> pRequest =
		std::dynamic_pointer_cast<SegmentRequestMessage>(requests[0]);
	REQUIRE(pRequest != nullptr);

	// A bitmap segment whose bits do not match the horizon bitmap root must fail
	// the attempt (the carried output root is irrelevant to bitmap validation).
	const BitmapSegment valid = BitmapSegment::FromSegment(SegmentGenerator<BitmapChunk>::Generate(
		fixture.BitmapMmr, SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0)));
	std::vector<BitmapBlock> tamperedBlocks;
	tamperedBlocks.push_back(BitmapBlock(1));
	tamperedBlocks.back().Set(3, true);
	const BitmapSegment tampered(
		SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0),
		std::move(tamperedBlocks),
		valid.GetProof());

	const SegmentResponseMessage badResponse = SegmentResponseMessage::CreateOutputBitmap(
		pRequest->GetBlockHash(), tampered, fixture.Outputs.GetRoot());

	REQUIRE(!sync.OnResponse(badResponse, start));
	REQUIRE(sync.GetStatus() == PibdSync::EStatus::Failed);
	REQUIRE(!sync.GetError().empty());
	REQUIRE(!sync.TryComplete());
	REQUIRE(sync.TakeRequests(start).empty());

	// The live state was never touched.
	REQUIRE(FileUtil::Exists(root / "live" / "old_state.txt"));

	fs::remove_all(root, error);
}
