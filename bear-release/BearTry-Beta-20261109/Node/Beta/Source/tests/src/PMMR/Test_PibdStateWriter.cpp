#include <catch.hpp>

#include <Common/Util/BitUtil.h>
#include <Core/File/DataFile.h>
#include <Core/Models/Fee.h>
#include <Core/Models/Features.h>
#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/RangeProof.h>
#include <Crypto/Models/Signature.h>
#include <P2P/PIBD/PibdParams.h>
#include <P2P/PIBD/PibdSession.h>
#include <P2P/PIBD/PibdSnapshot.h>
#include <P2P/PIBD/PibdStateWriter.h>
#include <Core/File/BitmapFile.h>
#include <PMMR/Common/HashFile.h>
#include <PMMR/Common/MMRHashUtil.h>
#include <PMMR/Common/MMRUtil.h>
#include <Core/Models/FullBlock.h>
#include <PMMR/OutputPMMR.h>
#include <PMMR/Common/PruneList.h>
#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/InMemoryPmmr.h>
#include <PMMR/Models/Segment.h>
#include <PMMR/Models/SegmentException.h>
#include <PMMR/Models/SegmentGenerator.h>
#include <PMMR/Models/SegmentIdentifier.h>

#include <filesystem.h>

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
		// 675 proof bytes -> 683 serialized bytes, the fixed RangeProofPMMR size.
		return RangeProof(std::vector<unsigned char>(675, seed));
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

	// Downloads a complete synthetic horizon: 64 outputs, bitmap {0}, 2 kernels.
	PibdSnapshot BuildSnapshot(const uint64_t numOutputs, const uint64_t numKernels)
	{
		VectorSegmentBitmap bitmap(numOutputs);
		bitmap.Set(0, true);

		std::vector<BitmapChunk> chunks(1);
		chunks[0].Set(0, true);
		const InMemoryPmmr<BitmapChunk> bitmapMmr(std::move(chunks));
		const InMemoryPmmr<OutputIdentifier> outputs = MakeTree<OutputIdentifier>(numOutputs, MakeOutput);
		const InMemoryPmmr<RangeProof> rangeProofs = MakeTree<RangeProof>(numOutputs, MakeRangeProof);
		const InMemoryPmmr<TransactionKernel> kernels = MakeTree<TransactionKernel>(numKernels, MakeKernel);

		PibdDownload::Horizon horizon;
		horizon.BlockHash = Hash::ValueOf(77);
		horizon.Height = 4242;
		horizon.OutputMmrSize = outputs.GetSize();
		horizon.KernelMmrSize = kernels.GetSize();
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

		return BuildSnapshot(session.GetDownload());
	}
}

TEST_CASE("PibdStateWriter::RejectsUnalignableHashOnlyLeaf")
{
	// 4 leaves, bitmap {2}; leaf 0 is hash-only and would become a height-0
	// pruned root with no data. Until the physical data model is proven, the
	// writer must reject this snapshot rather than write shifted data indices.
	const uint64_t numOutputs = 4;
	VectorSegmentBitmap bitmap(numOutputs);
	bitmap.Set(2, true);

	const InMemoryPmmr<OutputIdentifier> outputs = MakeTree<OutputIdentifier>(numOutputs, MakeOutput);
	const InMemoryPmmr<RangeProof> rangeProofs = MakeTree<RangeProof>(numOutputs, MakeRangeProof);
	const InMemoryPmmr<TransactionKernel> kernels = MakeTree<TransactionKernel>(2, MakeKernel);

	PibdSnapshot snapshot;
	snapshot.BlockHash = Hash::ValueOf(23);
	snapshot.Height = 100;
	snapshot.OutputMmrSize = outputs.GetSize();
	snapshot.KernelMmrSize = kernels.GetSize();
	snapshot.OutputMmrRoot = outputs.GetRoot();
	snapshot.RangeProofRoot = rangeProofs.GetRoot();
	snapshot.KernelRoot = kernels.GetRoot();
	snapshot.Bitmap = bitmap;
	for (uint64_t i = 1; i < numOutputs; ++i) {
		const uint64_t pos = MMRUtil::InsertionToPmmrIndex(i);
		snapshot.Outputs.emplace_back(i, outputs.GetLeafData(pos));
		snapshot.RangeProofs.emplace_back(i, rangeProofs.GetLeafData(pos));
	}
	for (uint64_t i = 0; i < 2; ++i) {
		const uint64_t pos = MMRUtil::InsertionToPmmrIndex(i);
		snapshot.Kernels.emplace_back(i, kernels.GetLeafData(pos));
	}
	snapshot.OutputHashes.emplace(0, outputs.GetHash(0));
	snapshot.RangeProofHashes.emplace(0, rangeProofs.GetHash(0));

	const fs::path dir = fs::temp_directory_path() / "pibd-hash-only-reject";
	std::error_code error;
	fs::remove_all(dir, error);
	REQUIRE_THROWS_AS(PibdStateWriter::Write(snapshot, dir), SegmentException);
	fs::remove_all(dir, error);
}

TEST_CASE("PibdStateWriter::WritesVerifiablePmmrFiles")
{
	const uint64_t numOutputs = 64;
	const uint64_t numKernels = 2;
	const PibdSnapshot snapshot = BuildSnapshot(numOutputs, numKernels);

	const fs::path dir = fs::temp_directory_path() / "pibd-state-writer-test";
	std::error_code error;
	fs::remove_all(dir, error);

	PibdStateWriter::Write(snapshot, dir);

	// Output tree: reload the files a node would read and recompute the root.
	const fs::path outputDir = dir / "output";
	std::shared_ptr<HashFile> pOutputHash = HashFile::Load(outputDir / "pmmr_hash.bin");
	std::shared_ptr<const PruneList> pOutputPrune = PruneList::Load(outputDir / "pmmr_prun.bin");
	REQUIRE(MMRHashUtil::Root(pOutputHash, snapshot.OutputMmrSize, pOutputPrune) == snapshot.OutputMmrRoot);

	std::shared_ptr<DataFile<PibdStateWriter::OUTPUT_LEAF_SIZE>> pOutputData =
		DataFile<PibdStateWriter::OUTPUT_LEAF_SIZE>::Load(outputDir / "pmmr_data.bin");
	REQUIRE(pOutputData->GetSize() == snapshot.Outputs.size());

	// Unpruned leaf 0 carries the expected payload.
	ByteBuffer byteBuffer(pOutputData->GetDataAt(0));
	const OutputIdentifier first = OutputIdentifier::Deserialize(byteBuffer);
	Serializer expected;
	snapshot.Outputs[0].second.Serialize(expected);
	Serializer actual;
	first.Serialize(actual);
	REQUIRE(actual.GetBytes() == expected.GetBytes());

	std::shared_ptr<BitmapFile> pLeafSet = BitmapFile::Load(outputDir / "pmmr_leafset.bin");
	// The leaf set marks unspent leaves only: leaf 0 is unspent, leaf 1 was sent
	// because its sibling bit is set but it is spent, so it stays out of the set.
	REQUIRE(pLeafSet->IsSet(0));
	REQUIRE(!pLeafSet->IsSet(1));
	REQUIRE(!pLeafSet->IsSet(2));

	// Rangeproof tree.
	const fs::path rangeProofDir = dir / "rangeproof";
	std::shared_ptr<HashFile> pRangeProofHash = HashFile::Load(rangeProofDir / "pmmr_hash.bin");
	std::shared_ptr<const PruneList> pRangeProofPrune = PruneList::Load(rangeProofDir / "pmmr_prun.bin");
	REQUIRE(MMRHashUtil::Root(pRangeProofHash, snapshot.OutputMmrSize, pRangeProofPrune) ==
		snapshot.RangeProofRoot);

	// Kernel tree: never pruned.
	const fs::path kernelDir = dir / "kernel";
	std::shared_ptr<HashFile> pKernelHash = HashFile::Load(kernelDir / "pmmr_hash.bin");
	std::shared_ptr<const PruneList> pKernelPrune = PruneList::Load(kernelDir / "pmmr_prun.bin");
	REQUIRE(pKernelPrune->GetTotalShift() == 0);
	REQUIRE(MMRHashUtil::Root(pKernelHash, snapshot.KernelMmrSize, pKernelPrune) == snapshot.KernelRoot);

	std::shared_ptr<DataFile<PibdStateWriter::KERNEL_LEAF_SIZE>> pKernelData =
		DataFile<PibdStateWriter::KERNEL_LEAF_SIZE>::Load(kernelDir / "pmmr_data.bin");
	REQUIRE(pKernelData->GetSize() == numKernels);

	fs::remove_all(dir, error);
}

TEST_CASE("PibdStateWriter::RejectsSnapshotWithoutPrunedHashes")
{
	const PibdSnapshot snapshot = BuildSnapshot(32, 2);

	PibdSnapshot incomplete = snapshot;
	incomplete.OutputHashes.clear();

	const fs::path dir = fs::temp_directory_path() / "pibd-state-writer-bad";
	std::error_code error;
	fs::remove_all(dir, error);

	REQUIRE_THROWS_AS(PibdStateWriter::Write(incomplete, dir), SegmentException);
	fs::remove_all(dir, error);
}
