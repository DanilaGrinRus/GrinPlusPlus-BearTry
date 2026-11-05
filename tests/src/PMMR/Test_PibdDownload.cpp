#include <catch.hpp>

#include <Common/Util/BitUtil.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <P2P/PIBD/PibdDownload.h>
#include <P2P/PIBD/PibdParams.h>
#include <PMMR/Common/MMRHashUtil.h>
#include <PMMR/Common/MMRUtil.h>
#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/InMemoryPmmr.h>
#include <PMMR/Models/Segment.h>
#include <PMMR/Models/SegmentException.h>
#include <PMMR/Models/SegmentGenerator.h>
#include <PMMR/Models/SegmentIdentifier.h>

#include <cstdint>
#include <vector>

namespace
{
	struct PibdLeaf
	{
		uint32_t Value;

		void Serialize(Serializer& serializer) const
		{
			serializer.Append<uint32_t>(Value);
		}

		static PibdLeaf Deserialize(ByteBuffer& byteBuffer)
		{
			return PibdLeaf{ byteBuffer.ReadU32() };
		}
	};

	using TestDownload = PibdDownloadT<PibdLeaf, PibdLeaf, PibdLeaf>;

	uint64_t MmrSizeForLeaves(const uint64_t numLeaves)
	{
		return 2 * numLeaves - (uint64_t)BitUtil::CountBitsSet(numLeaves);
	}

	InMemoryPmmr<PibdLeaf> MakeTree(const uint64_t numLeaves, const uint32_t base)
	{
		std::vector<PibdLeaf> leaves;
		leaves.reserve((size_t)numLeaves);
		for (uint64_t i = 0; i < numLeaves; ++i) {
			leaves.push_back(PibdLeaf{ base + (uint32_t)i });
		}

		return InMemoryPmmr<PibdLeaf>(std::move(leaves));
	}

	uint64_t RequiredLeaves(const uint64_t numLeaves, const VectorSegmentBitmap& bitmap)
	{
		const uint64_t mmrSize = MmrSizeForLeaves(numLeaves);
		uint64_t required = 0;
		for (uint64_t leafIdx = 0; leafIdx < numLeaves; ++leafIdx) {
			const uint64_t pos0 = MMRUtil::InsertionToPmmrIndex(leafIdx);
			if (IsSegmentLeafRequired(pos0, mmrSize, bitmap)) {
				++required;
			}
		}

		return required;
	}
}

TEST_CASE("PibdDownload::FullFlow")
{
	const uint64_t numOutputs = 300;
	const uint64_t numKernels = 50;

	// Bitmap: leaves 0 and 100 are unspent, everything else is pruned.
	VectorSegmentBitmap expectedBitmap(numOutputs);
	expectedBitmap.Set(0, true);
	expectedBitmap.Set(100, true);

	std::vector<BitmapChunk> chunks(1);
	chunks[0].Set(0, true);
	chunks[0].Set(100, true);
	const InMemoryPmmr<BitmapChunk> bitmapMmr(std::move(chunks));
	REQUIRE(bitmapMmr.GetNumLeaves() == 1);

	const InMemoryPmmr<PibdLeaf> outputs = MakeTree(numOutputs, 1000);
	const InMemoryPmmr<PibdLeaf> rangeProofs = MakeTree(numOutputs, 2000);
	const InMemoryPmmr<PibdLeaf> kernels = MakeTree(numKernels, 3000);

	TestDownload::Horizon horizon;
	horizon.BlockHash = Hash::ValueOf(7);
	horizon.Height = 1000;
	horizon.OutputMmrSize = outputs.GetSize();
	horizon.KernelMmrSize = kernels.GetSize();
	horizon.OutputRoot = MMRHashUtil::HashParentWithIndex(outputs.GetRoot(), bitmapMmr.GetRoot(), outputs.GetSize());
	horizon.RangeProofRoot = rangeProofs.GetRoot();
	horizon.KernelRoot = kernels.GetRoot();
	
	TestDownload download(horizon);
	REQUIRE(download.GetPhase() == TestDownload::EPhase::Bitmap);
	REQUIRE(download.GetRequiredCount() == 1);   // bitmap only, trees not created yet

	// Phase 1: exactly one bitmap segment request.
	const std::vector<TestDownload::SegmentRequest> bitmapRequests = download.NextRequests(15);
	REQUIRE(bitmapRequests.size() == 1);
	REQUIRE(bitmapRequests[0].Tree == TestDownload::ETree::Bitmap);
	REQUIRE(bitmapRequests[0].Identifier == SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0));

	const Segment<BitmapChunk> bitmapSegmentPmmr = SegmentGenerator<BitmapChunk>::Generate(
		bitmapMmr, SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0));
	REQUIRE(download.AcceptBitmapSegment(BitmapSegment::FromSegment(bitmapSegmentPmmr), outputs.GetRoot()));
	REQUIRE(download.GetPhase() == TestDownload::EPhase::Trees);
	REQUIRE(download.GetOutputBitmap().Contains(0));
	REQUIRE(download.GetOutputBitmap().Contains(100));
	REQUIRE(!download.GetOutputBitmap().Contains(1));
	REQUIRE(download.GetRequiredCount() == 4);

	// Phase 2: the three trees are requested together.
	const std::vector<TestDownload::SegmentRequest> treeRequests = download.NextRequests(15);
	REQUIRE(treeRequests.size() == 3);
	REQUIRE(treeRequests[0].Tree == TestDownload::ETree::Output);
	REQUIRE(treeRequests[1].Tree == TestDownload::ETree::RangeProof);
	REQUIRE(treeRequests[2].Tree == TestDownload::ETree::Kernel);

	for (const TestDownload::SegmentRequest& request : treeRequests) {
		switch (request.Tree) {
			case TestDownload::ETree::Output:
			{
				const Segment<PibdLeaf> segment = SegmentGenerator<PibdLeaf>::Generate(
					outputs, request.Identifier, &expectedBitmap);
				REQUIRE(download.AcceptOutputSegment(segment));
				break;
			}
			case TestDownload::ETree::RangeProof:
			{
				const Segment<PibdLeaf> segment = SegmentGenerator<PibdLeaf>::Generate(
					rangeProofs, request.Identifier, &expectedBitmap);
				REQUIRE(download.AcceptRangeProofSegment(segment));
				break;
			}
			case TestDownload::ETree::Kernel:
			{
				const Segment<PibdLeaf> segment = SegmentGenerator<PibdLeaf>::Generate(
					kernels, request.Identifier);
				REQUIRE(download.AcceptKernelSegment(segment));
				break;
			}
			case TestDownload::ETree::Bitmap:
			default:
				FAIL("unexpected bitmap request after the bitmap phase");
		}
	}

	REQUIRE(download.IsComplete());
	REQUIRE(download.GetPhase() == TestDownload::EPhase::Complete);
	REQUIRE(download.GetReceivedCount() == download.GetRequiredCount());
	REQUIRE(download.NextRequests(15).empty());

	const uint64_t expectedOutputLeaves = RequiredLeaves(numOutputs, expectedBitmap);
	REQUIRE(download.GetOutputs().GetLeaves().size() == expectedOutputLeaves);
	REQUIRE(download.GetRangeProofs().GetLeaves().size() == expectedOutputLeaves);
	REQUIRE(download.GetKernels().GetLeaves().size() == numKernels);
	REQUIRE(download.GetOutputs().GetStoredLeafCount() == expectedOutputLeaves);
}

TEST_CASE("PibdDownload::RejectsSegmentsOutOfOrder")
{
	const uint64_t numOutputs = 300;
	std::vector<BitmapChunk> chunks(1);
	const InMemoryPmmr<BitmapChunk> bitmapMmr(std::move(chunks));
	const InMemoryPmmr<PibdLeaf> outputs = MakeTree(numOutputs, 1000);
	const InMemoryPmmr<PibdLeaf> kernels = MakeTree(4, 3000);

	TestDownload::Horizon horizon;
	horizon.OutputMmrSize = outputs.GetSize();
	horizon.KernelMmrSize = kernels.GetSize();
	horizon.OutputRoot = MMRHashUtil::HashParentWithIndex(outputs.GetRoot(), bitmapMmr.GetRoot(), outputs.GetSize());
	horizon.RangeProofRoot = outputs.GetRoot();
	horizon.KernelRoot = kernels.GetRoot();
	
	TestDownload download(horizon);

	// Output segments are not accepted before the bitmap is assembled.
	const Segment<PibdLeaf> outputSegment = SegmentGenerator<PibdLeaf>::Generate(
		outputs, SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, 0));
	REQUIRE_THROWS_AS(download.AcceptOutputSegment(outputSegment), SegmentException);

	// Wrong bitmap root: the segment is rejected and the phase does not advance.
	TestDownload::Horizon badHorizon = horizon;
	badHorizon.OutputRoot = HASH::ZERO;
	TestDownload badDownload(badHorizon);
	const Segment<BitmapChunk> bitmapSegment = SegmentGenerator<BitmapChunk>::Generate(
		bitmapMmr, SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0));
	REQUIRE_THROWS_AS(
		badDownload.AcceptBitmapSegment(BitmapSegment::FromSegment(bitmapSegment), outputs.GetRoot()), SegmentException);
	REQUIRE(badDownload.GetPhase() == TestDownload::EPhase::Bitmap);
}

TEST_CASE("PibdDownload::DuplicatesAndMismatchedTrees")
{
	const uint64_t numOutputs = 300;
	VectorSegmentBitmap bitmap(numOutputs);
	bitmap.Set(0, true);

	std::vector<BitmapChunk> chunks(1);
	chunks[0].Set(0, true);
	const InMemoryPmmr<BitmapChunk> bitmapMmr(std::move(chunks));
	const InMemoryPmmr<PibdLeaf> outputs = MakeTree(numOutputs, 1000);
	const InMemoryPmmr<PibdLeaf> kernels = MakeTree(4, 3000);

	TestDownload::Horizon horizon;
	horizon.OutputMmrSize = outputs.GetSize();
	horizon.KernelMmrSize = kernels.GetSize();
	horizon.OutputRoot = MMRHashUtil::HashParentWithIndex(outputs.GetRoot(), bitmapMmr.GetRoot(), outputs.GetSize());
	horizon.RangeProofRoot = outputs.GetRoot();
	horizon.KernelRoot = kernels.GetRoot();
	
	TestDownload download(horizon);
	const BitmapSegment bitmapSegment = BitmapSegment::FromSegment(
		SegmentGenerator<BitmapChunk>::Generate(
			bitmapMmr, SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0)));

	REQUIRE(download.AcceptBitmapSegment(bitmapSegment, outputs.GetRoot()));
	REQUIRE(!download.AcceptBitmapSegment(bitmapSegment, outputs.GetRoot()));   // late duplicate
	REQUIRE(download.GetPhase() == TestDownload::EPhase::Trees);

	// A pruned output segment validates and the duplicate is ignored.
	const Segment<PibdLeaf> outputSegment = SegmentGenerator<PibdLeaf>::Generate(
		outputs, SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, 0), &bitmap);
	REQUIRE(download.AcceptOutputSegment(outputSegment));
	REQUIRE(!download.AcceptOutputSegment(outputSegment));

	// An unpruned output segment (no bitmap) does not validate against the
	// pruned root: the required leaves are there but the proof/hash layout for a
	// pruned tree is not, so it must be rejected by a fresh download.
	const Segment<PibdLeaf> unprunedOutput = SegmentGenerator<PibdLeaf>::Generate(
		outputs, SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, 0));
	TestDownload unprunedDownload(horizon);
	REQUIRE(unprunedDownload.AcceptBitmapSegment(bitmapSegment, outputs.GetRoot()));
	REQUIRE_THROWS_AS(unprunedDownload.AcceptOutputSegment(unprunedOutput), SegmentException);

	// A kernel segment fed as an output segment is rejected by the proof check,
	// never accepted.
	const Segment<PibdLeaf> kernelSegment = SegmentGenerator<PibdLeaf>::Generate(
		kernels, SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, 0));
	TestDownload mismatchedDownload(horizon);
	REQUIRE(mismatchedDownload.AcceptBitmapSegment(bitmapSegment, outputs.GetRoot()));
	REQUIRE_THROWS_AS(mismatchedDownload.AcceptOutputSegment(kernelSegment), SegmentException);

	// Request cap is honoured.
	REQUIRE(download.NextRequests(1).size() == 1);
}
