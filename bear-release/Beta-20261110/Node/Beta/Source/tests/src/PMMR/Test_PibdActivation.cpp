#include <catch.hpp>

#include <Common/Util/BitUtil.h>
#include <Common/Util/FileUtil.h>
#include <Core/File/DataFile.h>
#include <Core/Models/Fee.h>
#include <Core/Models/Features.h>
#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/Hash.h>
#include <Crypto/Models/RangeProof.h>
#include <Crypto/Models/Signature.h>
#include <P2P/PIBD/PibdActivation.h>
#include <P2P/PIBD/PibdParams.h>
#include <P2P/PIBD/PibdSession.h>
#include <P2P/PIBD/PibdSnapshot.h>
#include <P2P/PIBD/PibdStateWriter.h>
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

	PibdSnapshot BuildTestSnapshot()
	{
		const uint64_t numOutputs = 32;
		VectorSegmentBitmap bitmap(numOutputs);
		bitmap.Set(0, true);

		std::vector<BitmapChunk> chunks(1);
		chunks[0].Set(0, true);
		const InMemoryPmmr<BitmapChunk> bitmapMmr(std::move(chunks));
		const InMemoryPmmr<OutputIdentifier> outputs = MakeTree<OutputIdentifier>(numOutputs, MakeOutput);
		const InMemoryPmmr<RangeProof> rangeProofs = MakeTree<RangeProof>(numOutputs, MakeRangeProof);
		const InMemoryPmmr<TransactionKernel> kernels = MakeTree<TransactionKernel>(2, MakeKernel);

		PibdDownload::Horizon horizon;
		horizon.BlockHash = Hash::ValueOf(21);
		horizon.Height = 777;
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

	Hash RootOf(const fs::path& treeDir, const uint64_t mmrSize)
	{
		std::shared_ptr<HashFile> pHashFile = HashFile::Load(treeDir / "pmmr_hash.bin");
		std::shared_ptr<const PruneList> pPruneList = PruneList::Load(treeDir / "pmmr_prun.bin");
		return MMRHashUtil::Root(pHashFile, mmrSize, pPruneList);
	}

	void WriteSentinel(const fs::path& dir, const std::string& name)
	{
		FileUtil::CreateDirectories(dir);
		FileUtil::WriteTextToFile(dir / name, "sentinel\n");
	}
}

TEST_CASE("PibdActivation::ReplacesLiveStateAtomically")
{
	const PibdSnapshot snapshot = BuildTestSnapshot();

	const fs::path root = fs::temp_directory_path() / "pibd-activation-test";
	std::error_code error;
	fs::remove_all(root, error);

	const fs::path staging = root / "staging";
	const fs::path live = root / "txhashset";

	PibdStateWriter::Write(snapshot, staging);
	REQUIRE(PibdActivation::IsCompleteState(staging));

	WriteSentinel(live, "old_state.txt");

	PibdActivation::Activate(staging, live);

	// The new state is in place and still verifies from disk.
	REQUIRE(PibdActivation::IsCompleteState(live));
	REQUIRE(RootOf(live / "output", snapshot.OutputMmrSize) == snapshot.OutputMmrRoot);
	REQUIRE(RootOf(live / "kernel", snapshot.KernelMmrSize) == snapshot.KernelRoot);

	// The old state, the backup and the marker are gone.
	REQUIRE(!FileUtil::Exists(live / "old_state.txt"));
	REQUIRE(!FileUtil::Exists(fs::path(live.u8string() + ".backup")));
	REQUIRE(!FileUtil::Exists(root / PibdActivation::MARKER_FILE_NAME));

	fs::remove_all(root, error);
}

TEST_CASE("PibdActivation::RefusesIncompleteStaging")
{
	const PibdSnapshot snapshot = BuildTestSnapshot();

	const fs::path root = fs::temp_directory_path() / "pibd-activation-refuse";
	std::error_code error;
	fs::remove_all(root, error);

	const fs::path staging = root / "staging";
	const fs::path live = root / "txhashset";
	PibdStateWriter::Write(snapshot, staging);
	WriteSentinel(live, "old_state.txt");

	// Remove the completion marker: a partially written state must never replace
	// the live database.
	FileUtil::RemoveFile(staging / PibdActivation::COMPLETE_FILE_NAME);
	REQUIRE_THROWS_AS(PibdActivation::Activate(staging, live), std::exception);
	REQUIRE(FileUtil::Exists(live / "old_state.txt"));

	fs::remove_all(root, error);
}

TEST_CASE("PibdActivation::RecoversInterruptedActivation")
{
	const fs::path root = fs::temp_directory_path() / "pibd-activation-recover";
	std::error_code error;
	fs::remove_all(root, error);
	FileUtil::CreateDirectories(root);

	const fs::path live = root / "txhashset";
	const fs::path backup = fs::path(live.u8string() + ".backup");
	const fs::path marker = root / PibdActivation::MARKER_FILE_NAME;

	// Crash between "live -> backup" and "staging -> live": the live directory is
	// missing, the backup holds the previous state.
	WriteSentinel(backup, "backup_state.txt");
	FileUtil::WriteTextToFile(
		marker, fs::path(root / "staging").u8string() + "\n" + backup.u8string() + "\n");

	REQUIRE(PibdActivation::Recover(live));
	REQUIRE(FileUtil::Exists(live / "backup_state.txt"));
	REQUIRE(!FileUtil::Exists(backup));
	REQUIRE(!FileUtil::Exists(marker));

	// Crash after "staging -> live": the new state is complete, the backup is
	// stale and must be dropped.
	WriteSentinel(backup, "backup_state.txt");
	PibdActivation::WriteCompleteMarker(live, Hash::ValueOf(3), 10);
	FileUtil::WriteTextToFile(
		marker, fs::path(root / "staging").u8string() + "\n" + backup.u8string() + "\n");

	REQUIRE(PibdActivation::Recover(live));
	REQUIRE(PibdActivation::IsCompleteState(live));
	REQUIRE(!FileUtil::Exists(backup));
	REQUIRE(!FileUtil::Exists(marker));

	// No marker: nothing to recover.
	REQUIRE(!PibdActivation::Recover(live));

	fs::remove_all(root, error);
}
