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
#include <P2P/PIBD/PibdSession.h>
#include <P2P/PIBD/PibdSnapshot.h>
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

	uint64_t RequiredLeaves(const uint64_t numLeaves, const VectorSegmentBitmap& bitmap)
	{
		const uint64_t mmrSize = MmrSizeForLeaves(numLeaves);
		uint64_t required = 0;
		for (uint64_t leafIdx = 0; leafIdx < numLeaves; ++leafIdx) {
			if (IsSegmentLeafRequired(MMRUtil::InsertionToPmmrIndex(leafIdx), mmrSize, bitmap)) {
				++required;
			}
		}

		return required;
	}
}

TEST_CASE("PibdSnapshot::BuildsFromCompletedDownload")
{
	const uint64_t numOutputs = 120;
	const uint64_t numKernels = 8;

	VectorSegmentBitmap bitmap(numOutputs);
	bitmap.Set(0, true);
	bitmap.Set(70, true);

	std::vector<BitmapChunk> chunks(1);
	chunks[0].Set(0, true);
	chunks[0].Set(70, true);
	const InMemoryPmmr<BitmapChunk> bitmapMmr(std::move(chunks));

	const InMemoryPmmr<OutputIdentifier> outputs = MakeTree<OutputIdentifier>(numOutputs, MakeOutput);
	const InMemoryPmmr<RangeProof> rangeProofs = MakeTree<RangeProof>(numOutputs, MakeRangeProof);
	const InMemoryPmmr<TransactionKernel> kernels = MakeTree<TransactionKernel>(numKernels, MakeKernel);

	PibdDownload::Horizon horizon;
	horizon.BlockHash = Hash::ValueOf(99);
	horizon.Height = 12345;
	horizon.OutputMmrSize = MmrSizeForLeaves(numOutputs);
	horizon.KernelMmrSize = MmrSizeForLeaves(numKernels);
	horizon.OutputRoot = MMRHashUtil::HashParentWithIndex(outputs.GetRoot(), bitmapMmr.GetRoot(), outputs.GetSize());
	horizon.RangeProofRoot = rangeProofs.GetRoot();
	horizon.KernelRoot = kernels.GetRoot();
	
	PibdSession session(horizon);

	// An incomplete download has no snapshot.
	REQUIRE_THROWS_AS(BuildSnapshot(session.GetDownload()), SegmentException);

	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateOutputBitmap(
		horizon.BlockHash,
		BitmapSegment::FromSegment(SegmentGenerator<BitmapChunk>::Generate(
			bitmapMmr, SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0))),
		outputs.GetRoot())));
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateOutputs(
		horizon.BlockHash,
		SegmentGenerator<OutputIdentifier>::Generate(
			outputs, SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, 0), &bitmap),
		outputs.GetRoot())));
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateRangeProofs(
		horizon.BlockHash,
		SegmentGenerator<RangeProof>::Generate(
			rangeProofs, SegmentIdentifier(PibdParams::RANGEPROOF_SEGMENT_HEIGHT, 0), &bitmap))));
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateKernels(
		horizon.BlockHash,
		SegmentGenerator<TransactionKernel>::Generate(
			kernels, SegmentIdentifier(PibdParams::KERNEL_SEGMENT_HEIGHT, 0)))));

	REQUIRE(session.GetDownload().IsComplete());

	const PibdSnapshot snapshot = BuildSnapshot(session.GetDownload());

	REQUIRE(snapshot.BlockHash == horizon.BlockHash);
	REQUIRE(snapshot.Height == horizon.Height);
	REQUIRE(snapshot.OutputMmrSize == horizon.OutputMmrSize);
	REQUIRE(snapshot.KernelMmrSize == horizon.KernelMmrSize);

	const uint64_t expectedOutputs = RequiredLeaves(numOutputs, bitmap);
	REQUIRE(snapshot.Outputs.size() == expectedOutputs);
	REQUIRE(snapshot.RangeProofs.size() == expectedOutputs);
	REQUIRE(snapshot.Kernels.size() == numKernels);
	REQUIRE(expectedOutputs < numOutputs);   // pruning actually happened

	// Outputs and rangeproofs carry identical leaf index sets, sorted.
	for (size_t i = 0; i < snapshot.Outputs.size(); ++i) {
		REQUIRE(snapshot.Outputs[i].first == snapshot.RangeProofs[i].first);
		if (i > 0) {
			REQUIRE(snapshot.Outputs[i - 1].first < snapshot.Outputs[i].first);
		}
	}

	// Kernel leaf indices are contiguous from zero for the unpruned tree.
	for (size_t i = 0; i < snapshot.Kernels.size(); ++i) {
		REQUIRE(snapshot.Kernels[i].first == (uint64_t)i);
	}

	// The bitmap survived the snapshot.
	REQUIRE(snapshot.Bitmap.Contains(0));
	REQUIRE(snapshot.Bitmap.Contains(70));
	REQUIRE(!snapshot.Bitmap.Contains(1));
}

TEST_CASE("MMRUtil::PmmrLeafToInsertionIndexInvertsInsertionIndex")
{
	for (uint64_t leafIndex = 0; leafIndex < 200; ++leafIndex) {
		const uint64_t pos0 = MMRUtil::InsertionToPmmrIndex(leafIndex);
		const std::optional<uint64_t> back = MMRUtil::PmmrLeafToInsertionIndex(pos0);
		REQUIRE(back.has_value());
		REQUIRE(back.value() == leafIndex);
	}

	// Internal nodes are not leaves and have no leaf index.
	for (uint64_t pos0 = 0; pos0 < 64; ++pos0) {
		if (!MMRUtil::IsLeaf(pos0)) {
			REQUIRE(!MMRUtil::PmmrLeafToInsertionIndex(pos0).has_value());
		}
	}
}
TEST_CASE("PibdSnapshot::PrunedTreeNeedsStoredHashes")
{
	// Pruning is only representable with the subtree hashes carried by segments:
	// the leaves alone cannot rebuild the root.
	const uint64_t numOutputs = 64;
	VectorSegmentBitmap bitmap(numOutputs);
	bitmap.Set(0, true);

	std::vector<BitmapChunk> chunks(1);
	chunks[0].Set(0, true);
	const InMemoryPmmr<BitmapChunk> bitmapMmr(std::move(chunks));
	const InMemoryPmmr<OutputIdentifier> outputs = MakeTree<OutputIdentifier>(numOutputs, MakeOutput);
	const InMemoryPmmr<RangeProof> rangeProofs = MakeTree<RangeProof>(numOutputs, MakeRangeProof);
	const InMemoryPmmr<TransactionKernel> kernels = MakeTree<TransactionKernel>(2, MakeKernel);

	PibdDownload::Horizon horizon;
	horizon.BlockHash = Hash::ValueOf(5);
	horizon.OutputMmrSize = MmrSizeForLeaves(numOutputs);
	horizon.KernelMmrSize = MmrSizeForLeaves(2);
	horizon.OutputRoot = MMRHashUtil::HashParentWithIndex(outputs.GetRoot(), bitmapMmr.GetRoot(), outputs.GetSize());
	horizon.RangeProofRoot = rangeProofs.GetRoot();
	horizon.KernelRoot = kernels.GetRoot();
	
	PibdSession session(horizon);
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateOutputBitmap(
		horizon.BlockHash,
		BitmapSegment::FromSegment(SegmentGenerator<BitmapChunk>::Generate(
			bitmapMmr, SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0))),
		outputs.GetRoot())));
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateOutputs(
		horizon.BlockHash,
		SegmentGenerator<OutputIdentifier>::Generate(
			outputs, SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, 0), &bitmap),
		outputs.GetRoot())));
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateRangeProofs(
		horizon.BlockHash,
		SegmentGenerator<RangeProof>::Generate(
			rangeProofs, SegmentIdentifier(PibdParams::RANGEPROOF_SEGMENT_HEIGHT, 0), &bitmap))));
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateKernels(
		horizon.BlockHash,
		SegmentGenerator<TransactionKernel>::Generate(
			kernels, SegmentIdentifier(PibdParams::KERNEL_SEGMENT_HEIGHT, 0)))));

	const PibdSnapshot snapshot = BuildSnapshot(session.GetDownload());
	REQUIRE(!snapshot.OutputHashes.empty());   // pruning produced subtree hashes

	// With the hashes the root rebuilds exactly.
	REQUIRE(ComputePrunedTreeRoot(snapshot.OutputMmrSize, snapshot.Outputs, snapshot.OutputHashes)
		== snapshot.OutputMmrRoot);

	// Without them the rebuild must fail rather than invent a root.
	const std::map<uint64_t, Hash> noHashes;
	REQUIRE_THROWS_AS(
		ComputePrunedTreeRoot(snapshot.OutputMmrSize, snapshot.Outputs, noHashes), SegmentException);

	// Wrong subtree hashes must not reproduce the root either. Every stored hash
	// is replaced, because a hash belonging to an unpruned subtree is simply not
	// consulted by the rebuild.
	std::map<uint64_t, Hash> tampered = snapshot.OutputHashes;
	for (std::pair<const uint64_t, Hash>& entry : tampered) {
		entry.second = HASH::ZERO;
	}

	REQUIRE(ComputePrunedTreeRoot(snapshot.OutputMmrSize, snapshot.Outputs, tampered)
		!= snapshot.OutputMmrRoot);
}