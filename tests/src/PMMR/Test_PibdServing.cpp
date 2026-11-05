// Derived from grin 5.5.2 (Rust), Copyright The Grin Developers.
// Licensed under the Apache License, Version 2.0. Modified for the Grin++ C++ fork.
// See THIRD_PARTY_NOTICES.md and LICENSES/Apache-2.0.txt for details.
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
#include <P2P/PIBD/PibdArchiveServer.h>
#include <P2P/ArchivePublication.h>
#include <P2P/Sync/StateSyncer.h>
#include <P2P/PIBD/PibdParams.h>
#include <P2P/PIBD/PibdSegmentService.h>
#include <P2P/PIBD/PibdSession.h>
#include <P2P/PIBD/PibdSnapshot.h>
#include <P2P/PIBD/PibdStateWriter.h>
#include <PMMR/Common/MMRUtil.h>
#include <PMMR/KernelMMR.h>
#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/InMemoryPmmr.h>
#include <PMMR/Models/SegmentGenerator.h>
#include <PMMR/Models/SegmentIdentifier.h>
#include <PMMR/OutputPMMR.h>
#include <PMMR/RangeProofPMMR.h>
#include <PMMR/Zip/TxHashSetZip.h>
#include <PMMR/Zip/Zipper.h>

#include <Common/Util/FileUtil.h>
#include <Core/File/BitmapFile.h>
#include <Core/Global.h>
#include <Core/Models/BlockHeader.h>
#include <Core/Models/ProofOfWork.h>
#include <Crypto/Models/BlindingFactor.h>

#include <filesystem.h>

#include <cstdint>
#include <chrono>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
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

	// Builds a complete synthetic horizon (64 outputs, bitmap {0}, 2 kernels) and
	// writes it to disk exactly like a desegmenter would.
	PibdSnapshot WriteTestState(const fs::path& stateDir)
	{
		const uint64_t numOutputs = 64;

		// Leaf 0 is unspent; leaf 1 arrives because of the sibling rule but stays
		// spent; leaf 63 is required as the last leaf. The rest of the tree is
		// pruned, so serving exercises pruned-subtree roots.
		VectorSegmentBitmap bitmap(numOutputs);
		bitmap.Set(0, true);

		std::vector<BitmapChunk> chunks(1);
		chunks[0].Set(0, true);
		const InMemoryPmmr<BitmapChunk> bitmapMmr(std::move(chunks));
		const InMemoryPmmr<OutputIdentifier> outputs = MakeTree<OutputIdentifier>(numOutputs, MakeOutput);
		const InMemoryPmmr<RangeProof> rangeProofs = MakeTree<RangeProof>(numOutputs, MakeRangeProof);
		const InMemoryPmmr<TransactionKernel> kernels = MakeTree<TransactionKernel>(2, MakeKernel);

		PibdDownload::Horizon horizon;
		horizon.BlockHash = Hash::ValueOf(31);
		horizon.Height = 999;
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

		const PibdSnapshot snapshot = BuildSnapshot(session.GetDownload());
		PibdStateWriter::Write(snapshot, stateDir);
		return snapshot;
	}
}

TEST_CASE("PibdParams::ArchiveHorizonSelection")
{
	// grin 5.5.2 chain/src/chain.rs txhashset_archive_header{,_header_only}:
	// saturating_sub(threshold) then round down to the archive interval. The
	// live run's tip 3 318 655 yields H = 3 315 600 (Testnet/Mainnet constants).
	REQUIRE(PibdParams::ComputeArchiveHorizonHeight(3318655, 2880, 720) == 3315600);
	REQUIRE(PibdParams::ComputeArchiveHorizonHeight(3315600 + 2880, 2880, 720) == 3315600);
	REQUIRE(PibdParams::ComputeArchiveHorizonHeight(3315600 + 3600 - 1, 2880, 720) == 3315600);
	REQUIRE(PibdParams::ComputeArchiveHorizonHeight(3315600 + 3600, 2880, 720) == 3315600 + 720);
	REQUIRE(PibdParams::ComputeArchiveHorizonHeight(2879, 2880, 720) == 0);
	REQUIRE(PibdParams::ComputeArchiveHorizonHeight(2880, 2880, 720) == 0);
	REQUIRE(PibdParams::ComputeArchiveHorizonHeight(3600 + 2880, 2880, 720) == 3600);
	REQUIRE(PibdParams::ComputeArchiveHorizonHeight(100000, 2880, 0) == 0);

	REQUIRE_FALSE(PibdParams::IsServableArchiveHorizon(0, 720));
	REQUIRE(PibdParams::IsServableArchiveHorizon(720, 720));
	REQUIRE_FALSE(PibdParams::IsServableArchiveHorizon(721, 720));

	// Rust uses 20/10 on AutomatedTesting/UserTesting chains. C++ does not
	// implement those constants yet; the math itself is chain-type agnostic.
	REQUIRE(PibdParams::ComputeArchiveHorizonHeight(5000, 20, 10) == 4980);
}

TEST_CASE("PibdParams::SegmentHeightRanges")
{
	// Per-tree ranges from grin 5.5.2 servers/src/common/adapters.rs:50-53.
	REQUIRE(PibdParams::IsValidBitmapSegmentHeight(9));
	REQUIRE(PibdParams::IsValidBitmapSegmentHeight(13));
	REQUIRE_FALSE(PibdParams::IsValidBitmapSegmentHeight(8));
	REQUIRE_FALSE(PibdParams::IsValidBitmapSegmentHeight(14));
	REQUIRE(PibdParams::IsValidKernelSegmentHeight(9));
	REQUIRE(PibdParams::IsValidKernelSegmentHeight(13));
	REQUIRE_FALSE(PibdParams::IsValidKernelSegmentHeight(14));

	REQUIRE(PibdParams::IsValidOutputSegmentHeight(11));
	REQUIRE(PibdParams::IsValidOutputSegmentHeight(15));
	REQUIRE_FALSE(PibdParams::IsValidOutputSegmentHeight(10));
	REQUIRE_FALSE(PibdParams::IsValidOutputSegmentHeight(16));

	REQUIRE(PibdParams::IsValidRangeProofSegmentHeight(7));
	REQUIRE(PibdParams::IsValidRangeProofSegmentHeight(11));
	REQUIRE_FALSE(PibdParams::IsValidRangeProofSegmentHeight(6));
	REQUIRE_FALSE(PibdParams::IsValidRangeProofSegmentHeight(12));
}

TEST_CASE("PibdSegmentService::RangeProofSegmentWireRoundTrip")
{
	const fs::path stateDir = fs::temp_directory_path() / "pibd-serving-rangeproof-wire";
	std::error_code error;
	fs::remove_all(stateDir, error);

	WriteTestState(stateDir);
	std::shared_ptr<OutputPMMR> pOutputPMMR = OutputPMMR::Load(stateDir);
	std::shared_ptr<RangeProofPMMR> pRangeProofPMMR = RangeProofPMMR::Load(stateDir);
	std::shared_ptr<KernelMMR> pKernelMMR = KernelMMR::Load(stateDir);
	PibdSegmentService service(pOutputPMMR, pRangeProofPMMR, pKernelMMR);
	const Segment<RangeProof> segment =
		service.GetRangeProofSegment(SegmentIdentifier(PibdParams::RANGEPROOF_SEGMENT_HEIGHT, 0));
	Serializer serializer;
	segment.Serialize(serializer);
	ByteBuffer buffer(serializer.GetBytes());
	const Segment<RangeProof> decoded = Segment<RangeProof>::Deserialize(buffer);
	REQUIRE(decoded.GetLeafPositions().size() == segment.GetLeafPositions().size());
	REQUIRE(decoded.GetLeafData().size() == segment.GetLeafData().size());
	REQUIRE(decoded.GetHashes().size() == segment.GetHashes().size());
	REQUIRE(buffer.GetRemainingSize() == 0);
	fs::remove_all(stateDir, error);
}

TEST_CASE("PibdSegmentService::RangeProofSegmentLiveState")
{
	const fs::path stateDir = "fixtures/pibd/live-state";
	if (!fs::exists(stateDir)) {
		return;
	}

	std::shared_ptr<OutputPMMR> pOutputPMMR = OutputPMMR::Load(stateDir);
	std::shared_ptr<RangeProofPMMR> pRangeProofPMMR = RangeProofPMMR::Load(stateDir);
	std::shared_ptr<KernelMMR> pKernelMMR = KernelMMR::Load(stateDir);
	PibdSegmentService service(pOutputPMMR, pRangeProofPMMR, pKernelMMR);
	const Segment<RangeProof> segment =
		service.GetRangeProofSegment(SegmentIdentifier(PibdParams::RANGEPROOF_SEGMENT_HEIGHT, 0));
	INFO("leaves=" << segment.GetLeafData().size() << " hashes=" << segment.GetHashes().size());
	if (!segment.GetLeafData().empty()) {
		INFO("first proof bytes=" << segment.GetLeafData().front().GetProofBytes().size());
	}
	Serializer serializer;
	segment.Serialize(serializer);
	INFO("serialized=" << serializer.GetBytes().size());
	ByteBuffer buffer(serializer.GetBytes());
	const Segment<RangeProof> decoded = Segment<RangeProof>::Deserialize(buffer);
	REQUIRE(decoded.GetLeafData().size() == segment.GetLeafData().size());
	REQUIRE(buffer.GetRemainingSize() == 0);
}

TEST_CASE("PibdSegmentService::ServesStateThatValidates")
{
	const fs::path stateDir = fs::temp_directory_path() / "pibd-serving-test";
	std::error_code error;
	fs::remove_all(stateDir, error);

	const PibdSnapshot written = WriteTestState(stateDir);

	// Pruning really happened: only the required leaves are in the state.
	REQUIRE(written.Outputs.size() < 64);
	REQUIRE(written.Outputs.size()> 0);

	// Serve from the files a node would read back.
	std::shared_ptr<OutputPMMR> pOutputPMMR = OutputPMMR::Load(stateDir);
	std::shared_ptr<RangeProofPMMR> pRangeProofPMMR = RangeProofPMMR::Load(stateDir);
	std::shared_ptr<KernelMMR> pKernelMMR = KernelMMR::Load(stateDir);

	PibdSegmentService service(pOutputPMMR, pRangeProofPMMR, pKernelMMR);
	REQUIRE(service.GetOutputMmrSize() == written.OutputMmrSize);
	REQUIRE(service.GetKernelMmrSize() == written.KernelMmrSize);

	// The served bitmap is the horizon bitmap taken from the leaf set.
	const VectorSegmentBitmap servedBitmap = service.GetOutputBitmap();
	REQUIRE(servedBitmap.Contains(0));
	REQUIRE(!servedBitmap.Contains(1));
	REQUIRE(!servedBitmap.Contains(2));

	// A fresh downloader receives the served segments and rebuilds the same state.
	PibdDownload::Horizon horizon = [&written] {
		PibdDownload::Horizon h;
		h.BlockHash = written.BlockHash;
		h.Height = written.Height;
		h.OutputMmrSize = written.OutputMmrSize;
		h.KernelMmrSize = written.KernelMmrSize;
		h.OutputRoot = written.OutputRoot;
		h.RangeProofRoot = written.RangeProofRoot;
		h.KernelRoot = written.KernelRoot;
				return h;
	}();

	PibdSession downloader(horizon);
	REQUIRE(downloader.AcceptResponse(SegmentResponseMessage::CreateOutputBitmap(
		horizon.BlockHash,
		service.GetBitmapSegment(SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0)),
		written.OutputMmrRoot)));
	REQUIRE(downloader.AcceptResponse(SegmentResponseMessage::CreateOutputs(
		horizon.BlockHash,
		service.GetOutputSegment(SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, 0)),
		service.GetBitmapMmrRoot())));
	REQUIRE(downloader.AcceptResponse(SegmentResponseMessage::CreateRangeProofs(
		horizon.BlockHash,
		service.GetRangeProofSegment(SegmentIdentifier(PibdParams::RANGEPROOF_SEGMENT_HEIGHT, 0)))));
	REQUIRE(downloader.AcceptResponse(SegmentResponseMessage::CreateKernels(
		horizon.BlockHash,
		service.GetKernelSegment(SegmentIdentifier(PibdParams::KERNEL_SEGMENT_HEIGHT, 0)))));

	REQUIRE(downloader.GetDownload().IsComplete());

	const PibdSnapshot rebuilt = BuildSnapshot(downloader.GetDownload());
	REQUIRE(rebuilt.Outputs.size() == written.Outputs.size());
	REQUIRE(rebuilt.RangeProofs.size() == written.RangeProofs.size());
	REQUIRE(rebuilt.Kernels.size() == written.Kernels.size());

	for (size_t i = 0; i < rebuilt.Outputs.size(); ++i) {
		REQUIRE(rebuilt.Outputs[i].first == written.Outputs[i].first);
	}

	fs::remove_all(stateDir, error);
}

//
// G-09 archive serving: executable tests for the bounded candidate
// (src/P2P/PIBD/PibdArchiveServer.h). These cover the pure/gated/validation
// pieces; the full worker + chain path still needs a live acceptance run.
//
namespace
{
	BlockHeaderPtr MakeArchiveHeader(
		const uint16_t version,
		const PibdSnapshot& snapshot,
		Hash outputRoot,
		Hash rangeProofRoot,
		Hash kernelRoot,
		const uint64_t outputMmrSize,
		const uint64_t kernelMmrSize)
	{
		return std::make_shared<BlockHeader>(
			version,
			snapshot.Height,
			0,
			Hash(),
			Hash(),
			std::move(outputRoot),
			std::move(rangeProofRoot),
			std::move(kernelRoot),
			BlindingFactor(),
			outputMmrSize,
			kernelMmrSize,
			0,
			0,
			0,
			ProofOfWork(0, {}, Hash(snapshot.BlockHash)));
	}

	BlockHeaderPtr MakeArchiveHeader(const PibdSnapshot& snapshot)
	{
		return MakeArchiveHeader(
			3,
			snapshot,
			snapshot.OutputRoot,
			snapshot.RangeProofRoot,
			snapshot.KernelRoot,
			snapshot.OutputMmrSize,
			snapshot.KernelMmrSize);
	}

	std::shared_ptr<PibdArchiveServer::Snapshot> MakeSnapshot(
		const Hash& hash,
		const uint64_t height,
		const std::function<void(const fs::path&)>& onDestroy = {})
	{
		auto pSnapshot = std::make_shared<PibdArchiveServer::Snapshot>();
		pSnapshot->key.hash = hash;
		pSnapshot->key.height = height;
		if (onDestroy) {
			pSnapshot->stagingRoot = fs::path("pibd-lease-unit-test");
			pSnapshot->onDestroy = onDestroy;
		}
		return pSnapshot;
	}

	std::vector<uint8_t> ReadAllBytes(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open()) {
			return {};
		}

		return std::vector<uint8_t>(
			std::istreambuf_iterator<char>(file),
			std::istreambuf_iterator<char>());
	}

	bool WriteAllBytes(const fs::path& path, const std::vector<uint8_t>& bytes)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file.is_open()) {
			return false;
		}

		file.write((const char*)bytes.data(), (std::streamsize)bytes.size());
		return file.good();
	}

	void MaterializeLegacyLeaf(const fs::path& treeDir)
	{
		const fs::path leafSetPath = treeDir / "pmmr_leafset.bin";
		REQUIRE(fs::exists(leafSetPath));
		const auto pLeafSet = BitmapFile::Load(leafSetPath);
		const Roaring bitmap = pLeafSet->ToRoaring();
		const size_t numBytes = bitmap.getSizeInBytes();
		std::vector<uint8_t> bytes(numBytes);
		REQUIRE(bitmap.write((char*)bytes.data()) == numBytes);
		FileUtil::SafeWriteToFile(treeDir / "pmmr_leaf.bin", bytes);
	}

	// Copies the flat file set of one PMMR tree directory (no recursion, no
	// unknown extras) so the fixture layout is explicit.
	void CopyTreeFiles(const fs::path& sourceDir, const fs::path& destDir)
	{
		std::error_code error;
		fs::create_directories(destDir, error);
		REQUIRE_FALSE(static_cast<bool>(error));
		for (const fs::directory_entry& entry : fs::directory_iterator(sourceDir, error))
		{
			if (error) {
				break;
			}

			std::error_code entryError;
			if (!entry.is_regular_file(entryError) || entryError) {
				continue;
			}

			fs::copy_file(
				entry.path(),
				destDir / entry.path().filename(),
				fs::copy_options::overwrite_existing,
				entryError);
			REQUIRE_FALSE(static_cast<bool>(entryError));
		}
		REQUIRE_FALSE(static_cast<bool>(error));
	}

	// Mirrors TxHashSetManager::SaveSnapshot: a legacy archive zip carries
	// pmmr_leaf.bin.<shortHash> and the extractor renames it to pmmr_leaf.bin.
	fs::path BuildArchiveZip(
		const fs::path& sourceStateDir,
		const fs::path& workDir,
		const bool includeKernelHash,
		const std::string& headerShortHash = {})
	{
		std::error_code error;
		const fs::path zipRoot = workDir / "ziproot";
		fs::remove_all(zipRoot, error);
		fs::create_directories(zipRoot, error);

		for (const std::string& tree : { std::string("kernel"), std::string("output"), std::string("rangeproof") })
		{
			CopyTreeFiles(sourceStateDir / tree, zipRoot / tree);
			REQUIRE(fs::exists(zipRoot / tree));
		}

		const std::string shortHash = headerShortHash.empty()
			? Global::GetGenesisHeader()->ShortHash()
			: headerShortHash;
		for (const std::string& tree : { std::string("output"), std::string("rangeproof") })
		{
			MaterializeLegacyLeaf(zipRoot / tree);
			fs::rename(
				zipRoot / tree / "pmmr_leaf.bin",
				zipRoot / tree / ("pmmr_leaf.bin." + shortHash));
			fs::remove(zipRoot / tree / "pmmr_leafset.bin");
		}

		if (!includeKernelHash) {
			fs::remove(zipRoot / "kernel" / "pmmr_hash.bin");
		}

		const fs::path zipPath = workDir / "archive.zip";
		fs::remove(zipPath, error);
		Zipper::CreateZipFile(
			zipPath,
			{ zipRoot / "kernel", zipRoot / "output", zipRoot / "rangeproof" });
		REQUIRE(fs::exists(zipPath));
		return zipPath;
	}
}

TEST_CASE("PibdArchiveServer::HorizonSelectionAndSpaceBoundary")
{
	const uint64_t interval = (uint64_t)Consensus::TXHASHSET_ARCHIVE_INTERVAL;
	REQUIRE(interval == 720);

	// Tiny heights saturate to the genesis state and are never served.
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(0) == 0);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(1) == 0);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(2878) == 0);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(2879) == 0);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(2880) == 0);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(2881) == 0);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(3599) == 0);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(3600) == 720);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(4319) == 720);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(4320) == 1440);
	REQUIRE(PibdArchiveServer::ComputeHorizonHeight(3318655) == 3315600);

	REQUIRE_FALSE(PibdArchiveServer::IsServableHorizon(0));
	REQUIRE(PibdArchiveServer::IsServableHorizon(720));
	REQUIRE_FALSE(PibdArchiveServer::IsServableHorizon(721));
	REQUIRE(PibdArchiveServer::IsServableHorizon(3315600));

	REQUIRE(PibdArchiveServer::ComputeRequiredSpace(0) ==
		PibdArchiveServer::MIN_FREE_BYTES_AFTER_EXTRACT);
	REQUIRE(PibdArchiveServer::ComputeRequiredSpace(1) ==
		2ull + PibdArchiveServer::MIN_FREE_BYTES_AFTER_EXTRACT);

	const uint64_t maxValue = (std::numeric_limits<uint64_t>::max)();
	REQUIRE(PibdArchiveServer::ComputeRequiredSpace(maxValue / 2ull) == maxValue);
	REQUIRE_FALSE(PibdArchiveServer::HasSpaceFor(fs::temp_directory_path(), maxValue / 2ull));

	const fs::path tempDir = fs::temp_directory_path();
	std::error_code error;
	const fs::space_info spaceInfo = fs::space(tempDir, error);
	if (!error && (uint64_t)spaceInfo.available >= PibdArchiveServer::MIN_FREE_BYTES_AFTER_EXTRACT) {
		REQUIRE(PibdArchiveServer::HasSpaceFor(tempDir, 0));
	}
}

TEST_CASE("PibdArchiveServer::RegistryGatingAndExactHash")
{
	using EState = PibdArchiveServer::EState;
	const auto now = std::chrono::steady_clock::now();
	const Hash h1 = Hash::ValueOf(11);
	const Hash h2 = Hash::ValueOf(22);

	PibdArchiveServer::Registry disabled(false);
	REQUIRE(disabled.GetState() == EState::Disabled);
	REQUIRE_FALSE(disabled.ShouldAdvertise());
	REQUIRE_FALSE(disabled.CanPrepare(now));
	REQUIRE(disabled.Acquire(h1) == nullptr);

	PibdArchiveServer::Registry registry(true);
	REQUIRE(registry.GetState() == EState::Absent);
	REQUIRE_FALSE(registry.ShouldAdvertise());
	REQUIRE(registry.Acquire(h1) == nullptr);

	registry.SetExpected(h1, 100);
	REQUIRE(registry.GetExpectedHash() == h1);
	REQUIRE(registry.CanPrepare(now));
	REQUIRE_FALSE(registry.IsReadyForExpected());

	registry.BeginPreparation();
	REQUIRE(registry.GetState() == EState::Preparing);
	REQUIRE_FALSE(registry.CanPrepare(now));

	// A snapshot for a different horizon must never be published.
	REQUIRE_FALSE(registry.Publish(MakeSnapshot(h2, 940)));
	REQUIRE(registry.GetState() == EState::Preparing);
	REQUIRE_FALSE(registry.ShouldAdvertise());

	const auto pReady = MakeSnapshot(h1, 100);
	REQUIRE(registry.Publish(pReady));
	REQUIRE(registry.GetState() == EState::Ready);
	REQUIRE(registry.ShouldAdvertise());
	REQUIRE(registry.Acquire(h1) != nullptr);
	REQUIRE(registry.Acquire(h2) == nullptr);
	REQUIRE(registry.GetReadyHeight() == 100);
	REQUIRE_FALSE(registry.CanPrepare(now));

	// Failure backoff resets after the monotonic retry window.
	PibdArchiveServer::Registry failing(true);
	failing.SetExpected(h1, 100);
	failing.Fail(now);
	REQUIRE(failing.GetState() == EState::Failed);
	REQUIRE_FALSE(failing.CanPrepare(now));
	REQUIRE(failing.CanPrepare(
		now + PibdArchiveServer::FAILURE_RETRY + std::chrono::seconds(1)));
	failing.SetAbsent();
	REQUIRE_FALSE(failing.ShouldAdvertise());
}

TEST_CASE("PibdArchiveServer::LeaseOutlivesHorizonSwitch")
{
	const Hash h1 = Hash::ValueOf(101);
	const Hash h2 = Hash::ValueOf(202);

	PibdArchiveServer::Registry registry(true);
	registry.SetExpected(h1, 100);
	registry.BeginPreparation();

	int destroyed = 0;
	auto pFirst = MakeSnapshot(h1, 100, [&destroyed](const fs::path&) { ++destroyed; });
	REQUIRE(registry.Publish(pFirst));

	// The registry must be the only long-lived owner of the published snapshot.
	// Keeping this local shared_ptr alive would make lease.reset() below a
	// non-final release (this exact fixture bug was exposed by the first run of
	// this test at line 611).
	pFirst.reset();

	// A lease taken for the response must survive a horizon switch.
	std::shared_ptr<const PibdArchiveServer::Snapshot> lease = registry.Acquire(h1);
	REQUIRE(lease != nullptr);
	REQUIRE(lease->key.hash == h1);

	registry.SetExpected(h2, 940);
	REQUIRE_FALSE(registry.ShouldAdvertise());
	REQUIRE(registry.Acquire(h1) == nullptr);   // stale horizon refused
	REQUIRE(registry.Acquire(h2) == nullptr);   // new horizon not ready yet
	// The horizon switch dropped the registry's reference; the lease must now be
	// the only owner, otherwise the assertion below is meaningless.
	REQUIRE(lease.use_count() == 1);
	REQUIRE(destroyed == 0);                    // lease keeps it alive

	// A stale snapshot completed after the switch is rejected.
	REQUIRE_FALSE(registry.Publish(MakeSnapshot(h1, 100)));

	registry.BeginPreparation();
	auto pSecond = MakeSnapshot(h2, 940, [&destroyed](const fs::path&) { ++destroyed; });
	REQUIRE(registry.Publish(pSecond));
	pSecond.reset();
	REQUIRE(registry.ShouldAdvertise());
	REQUIRE(registry.Acquire(h2) != nullptr);
	REQUIRE(registry.Acquire(h1) == nullptr);
	REQUIRE(destroyed == 0);

	// Last release triggers the deferred staging cleanup, exactly once.
	lease.reset();
	REQUIRE(destroyed == 1);
	REQUIRE(registry.Acquire(h2) != nullptr);
}

TEST_CASE("PibdArchiveServer::StagingCleanup")
{
	const fs::path archiveDir = fs::temp_directory_path() / "pibd-archive-staging-cleanup";
	std::error_code error;
	fs::remove_all(archiveDir, error);
	fs::create_directories(archiveDir / ".staging-aaa" / "nested");
	fs::create_directories(archiveDir / ".staging-bbb");
	{
		std::ofstream file(archiveDir / ".staging-aaa" / "nested" / "data.bin", std::ios::binary);
		file << "x";
	}
	{
		std::ofstream file(archiveDir / "archive.zip", std::ios::binary);
		file << "keep";
	}

	REQUIRE(PibdArchiveServer::CleanupStagingDirs(archiveDir) == 2);
	REQUIRE_FALSE(fs::exists(archiveDir / ".staging-aaa"));
	REQUIRE_FALSE(fs::exists(archiveDir / ".staging-bbb"));
	REQUIRE(fs::exists(archiveDir / "archive.zip"));

	REQUIRE(PibdArchiveServer::CleanupStagingDirs(
		fs::temp_directory_path() / "pibd-archive-does-not-exist") == 0);

	fs::remove_all(archiveDir, error);
}

TEST_CASE("PibdArchiveServer::ValidateExtractedState")
{
	const fs::path stateDir = fs::temp_directory_path() / "pibd-archive-validate-state";
	std::error_code error;
	fs::remove_all(stateDir, error);

	const PibdSnapshot snapshot = WriteTestState(stateDir);
	const BlockHeaderPtr pHeader = MakeArchiveHeader(snapshot);

	std::string validationError;
	const auto pState = PibdArchiveServer::LoadAndValidate(stateDir, pHeader, &validationError);
	REQUIRE(pState != nullptr);
	REQUIRE(validationError.empty());
	REQUIRE(pState->GetFlushedBlockHeader() != nullptr);
	REQUIRE(pState->GetFlushedBlockHeader()->GetHash() == pHeader->GetHash());

	const auto rejected = [&stateDir](const BlockHeaderPtr& pBadHeader) {
		std::string err;
		const auto pResult = PibdArchiveServer::LoadAndValidate(stateDir, pBadHeader, &err);
		REQUIRE(pResult == nullptr);
		REQUIRE_FALSE(err.empty());
	};

	rejected(MakeArchiveHeader(3, snapshot, Hash::ValueOf(static_cast<uint8_t>(99)), snapshot.RangeProofRoot,
		snapshot.KernelRoot, snapshot.OutputMmrSize, snapshot.KernelMmrSize));
	rejected(MakeArchiveHeader(3, snapshot, snapshot.OutputRoot, Hash::ValueOf(static_cast<uint8_t>(99)),
		snapshot.KernelRoot, snapshot.OutputMmrSize, snapshot.KernelMmrSize));
	rejected(MakeArchiveHeader(3, snapshot, snapshot.OutputRoot, snapshot.RangeProofRoot,
		Hash::ValueOf(static_cast<uint8_t>(99)), snapshot.OutputMmrSize, snapshot.KernelMmrSize));
	rejected(MakeArchiveHeader(3, snapshot, snapshot.OutputRoot, snapshot.RangeProofRoot,
		snapshot.KernelRoot, snapshot.OutputMmrSize + 2ull, snapshot.KernelMmrSize));
	rejected(MakeArchiveHeader(3, snapshot, snapshot.OutputRoot, snapshot.RangeProofRoot,
		snapshot.KernelRoot, snapshot.OutputMmrSize, snapshot.KernelMmrSize + 2ull));

	// v2 headers commit to the plain output root; a merged root must be rejected.
	rejected(MakeArchiveHeader(2, snapshot, snapshot.OutputRoot, snapshot.RangeProofRoot,
		snapshot.KernelRoot, snapshot.OutputMmrSize, snapshot.KernelMmrSize));
	REQUIRE(PibdArchiveServer::LoadAndValidate(stateDir,
		MakeArchiveHeader(2, snapshot, snapshot.OutputMmrRoot, snapshot.RangeProofRoot,
			snapshot.KernelRoot, snapshot.OutputMmrSize, snapshot.KernelMmrSize)) != nullptr);

	fs::remove_all(stateDir, error);
}

TEST_CASE("PibdArchiveServer::ArchiveZipExtractsAndValidates")
{
	const fs::path workDir = fs::temp_directory_path() / "pibd-archive-zip-test";
	std::error_code error;
	fs::remove_all(workDir, error);
	fs::create_directories(workDir);

	const fs::path stateDir = workDir / "state";
	const PibdSnapshot snapshot = WriteTestState(stateDir);
	const BlockHeaderPtr pArchiveHeader = MakeArchiveHeader(snapshot);
	const BlockHeaderPtr pZipHeader = Global::GetGenesisHeader();
	REQUIRE(pZipHeader != nullptr);

	const fs::path zipPath = BuildArchiveZip(stateDir, workDir / "valid", true);
	const fs::path extractDir = workDir / "extracted";
	REQUIRE(TxHashSetZip(Global::GetConfig()).Extract(zipPath, *pZipHeader, extractDir));
	std::string validationError;
	REQUIRE(PibdArchiveServer::LoadAndValidate(extractDir, pArchiveHeader, &validationError) != nullptr);
	REQUIRE(validationError.empty());

	// Truncated archive: the central directory is gone, Extract must fail.
	const std::vector<uint8_t> zipBytes = ReadAllBytes(zipPath);
	REQUIRE(zipBytes.size() > 32);
	const fs::path truncatedPath = workDir / "truncated.zip";
	REQUIRE(WriteAllBytes(
		truncatedPath,
		std::vector<uint8_t>(zipBytes.cbegin(), zipBytes.cbegin() + (zipBytes.size() / 2))));
	REQUIRE_FALSE(TxHashSetZip(Global::GetConfig()).Extract(
		truncatedPath, *pZipHeader, workDir / "extract-truncated"));

	// Corrupted entry byte: stored entries are CRC-checked on close.
	std::vector<uint8_t> corrupted = zipBytes;
	corrupted[corrupted.size() / 2] ^= 0xFF;
	const fs::path corruptedPath = workDir / "corrupted.zip";
	REQUIRE(WriteAllBytes(corruptedPath, corrupted));
	REQUIRE_FALSE(TxHashSetZip(Global::GetConfig()).Extract(
		corruptedPath, *pZipHeader, workDir / "extract-corrupted"));

	// Missing expected entry.
	const fs::path missingZip = BuildArchiveZip(stateDir, workDir / "missing", false);
	REQUIRE_FALSE(TxHashSetZip(Global::GetConfig()).Extract(
		missingZip, *pZipHeader, workDir / "extract-missing"));

	fs::remove_all(workDir, error);
}

TEST_CASE("PibdArchiveServer::DeferredGarbageDrain")
{
	const fs::path root = fs::temp_directory_path() / "pibd-archive-garbage-drain";
	std::error_code error;
	fs::remove_all(root, error);
	fs::create_directories(root / "retired-a" / "nested");
	fs::create_directories(root / "retired-b");
	{
		std::ofstream file(root / "retired-a" / "nested" / "data.bin", std::ios::binary);
		file << "x";
	}

	std::deque<fs::path> pending;
	pending.push_back(root / "retired-a");
	pending.push_back(root / "retired-b");

	// Deferred-cleanup contract: enqueueing alone must not delete anything.
	REQUIRE(fs::exists(root / "retired-a"));
	REQUIRE(fs::exists(root / "retired-b"));

	REQUIRE(PibdArchiveServer::RemoveGarbageDirs(pending) == 2);
	REQUIRE(pending.empty());
	REQUIRE_FALSE(fs::exists(root / "retired-a"));
	REQUIRE_FALSE(fs::exists(root / "retired-b"));

	// Idempotent on an empty queue.
	std::deque<fs::path> emptyQueue;
	REQUIRE(PibdArchiveServer::RemoveGarbageDirs(emptyQueue) == 0);

	fs::remove_all(root, error);
}

TEST_CASE("PibdArchiveServer::ServingOptInParsing")
{
	// Production default is OFF; the env var is the only isolated-test opt-in.
	REQUIRE_FALSE(PibdArchiveServer::ENABLED_BY_DEFAULT);

	REQUIRE(PibdArchiveServer::IsTruthyOptInValue("1"));
	REQUIRE(PibdArchiveServer::IsTruthyOptInValue("true"));
	REQUIRE(PibdArchiveServer::IsTruthyOptInValue("TRUE"));
	REQUIRE(PibdArchiveServer::IsTruthyOptInValue("TrUe"));
	REQUIRE(PibdArchiveServer::IsTruthyOptInValue("on"));
	REQUIRE(PibdArchiveServer::IsTruthyOptInValue("yes"));
	REQUIRE(PibdArchiveServer::IsTruthyOptInValue("  yes  "));
	REQUIRE(PibdArchiveServer::IsTruthyOptInValue("\ttrue\n"));

	REQUIRE_FALSE(PibdArchiveServer::IsTruthyOptInValue(""));
	REQUIRE_FALSE(PibdArchiveServer::IsTruthyOptInValue("   "));
	REQUIRE_FALSE(PibdArchiveServer::IsTruthyOptInValue("0"));
	REQUIRE_FALSE(PibdArchiveServer::IsTruthyOptInValue("false"));
	REQUIRE_FALSE(PibdArchiveServer::IsTruthyOptInValue("off"));
	REQUIRE_FALSE(PibdArchiveServer::IsTruthyOptInValue("no"));
	REQUIRE_FALSE(PibdArchiveServer::IsTruthyOptInValue("yesplease"));
	REQUIRE_FALSE(PibdArchiveServer::IsTruthyOptInValue("2"));
	REQUIRE_FALSE(PibdArchiveServer::IsTruthyOptInValue("enabled"));

	// A hard default wins without consulting the process environment.
	REQUIRE(PibdArchiveServer::ResolveServingOptIn(true));
}

// G-09 task-10: persistent archives must be validated as complete containers
// and published atomically. A partial staging file or a partial final file is
// never considered ready, and publication replaces the final path only after the
// staged zip is complete. Root validation against the exact header remains in
// PibdArchiveServer::LoadAndValidate (see ArchiveZipExtractsAndValidates and
// RegistryGatingAndExactHash for the wrong-height/hash gate).
TEST_CASE("P2P::ArchivePublicationValidatedAtomicPublish")
{
	const fs::path root = fs::temp_directory_path() / "g09-archive-publication-test";
	std::error_code error;
	fs::remove_all(root, error);
	fs::create_directories(root, error);
	REQUIRE_FALSE(error);

	const fs::path stateDir = root / "state";
	WriteTestState(stateDir);
	const fs::path validZip = BuildArchiveZip(stateDir, root / "build", true);
	REQUIRE(ArchivePublication::IsReadyArchive(validZip));

	const fs::path finalPath = root / "archive" / "TxHashSet.0.zip";
	fs::create_directories(finalPath.parent_path(), error);
	REQUIRE_FALSE(error);
	const fs::path staged = ArchivePublication::StagingPath(finalPath);

	// Interrupted copy: a truncated staging file is not publishable, leaves no
	// final archive, and the staging file is cleaned up on failure.
	fs::copy_file(validZip, staged, fs::copy_options::overwrite_existing, error);
	REQUIRE_FALSE(error);
	fs::resize_file(staged, 4096);
	std::string publishError;
	REQUIRE_FALSE(ArchivePublication::PublishStaged(staged, finalPath, &publishError));
	REQUIRE_FALSE(publishError.empty());
	REQUIRE_FALSE(fs::exists(finalPath));

	// Complete staged file publishes atomically and is readable by the legacy
	// zip reader.
	fs::copy_file(validZip, staged, fs::copy_options::overwrite_existing, error);
	REQUIRE_FALSE(error);
	REQUIRE(ArchivePublication::PublishStaged(staged, finalPath, &publishError));
	REQUIRE(ArchivePublication::IsReadyArchive(finalPath));

	// Existing partial final file: readiness rejects it and a subsequent publish
	// atomically replaces it instead of trusting it.
	fs::resize_file(finalPath, 1024);
	REQUIRE_FALSE(ArchivePublication::IsReadyArchive(finalPath));
	fs::copy_file(validZip, staged, fs::copy_options::overwrite_existing, error);
	REQUIRE_FALSE(error);
	REQUIRE(ArchivePublication::PublishStaged(staged, finalPath, &publishError));
	REQUIRE(ArchivePublication::IsReadyArchive(finalPath));

	const std::shared_ptr<ZipFile> pZip = ZipFile::Load(finalPath);
	REQUIRE(pZip != nullptr);
	const std::vector<std::string> entries = pZip->ListFiles();
	REQUIRE(ArchivePublication::HasEntryPrefix(entries, "output/pmmr_leaf.bin."));
	REQUIRE(ArchivePublication::HasEntryPrefix(entries, "rangeproof/pmmr_leaf.bin."));

	// Missing archive content (no kernel hash) is rejected rather than published.
	const fs::path brokenZip = BuildArchiveZip(stateDir, root / "build-broken", false);
	REQUIRE_FALSE(ArchivePublication::IsReadyArchive(brokenZip));

	// Bounded recovery: only the same target hash is skipped after a failure.
	REQUIRE(ArchivePublication::SameRecoveryTarget("abc123\n", "abc123"));
	REQUIRE_FALSE(ArchivePublication::SameRecoveryTarget("abc123\n", "def456"));
	REQUIRE_FALSE(ArchivePublication::SameRecoveryTarget("", "abc123"));

	fs::remove_all(root, error);
}
// F13: container-valid archive with a header that does not match its roots must
// not be accepted as ready/suppress regeneration.
TEST_CASE("PibdArchiveServer::ExistingArchiveHeaderRootsValidated")
{
	const fs::path root = fs::temp_directory_path() / "g09-readiness-wrong-root";
	std::error_code error;
	fs::remove_all(root, error);
	fs::create_directories(root, error);
	REQUIRE_FALSE(error);

	const fs::path stateDir = root / "state";
	const PibdSnapshot snapshot = WriteTestState(stateDir);
	const BlockHeaderPtr pGoodHeader = MakeArchiveHeader(snapshot);
	const fs::path zipPath = BuildArchiveZip(stateDir, root / "build", true, pGoodHeader->ShortHash());

	std::string validationError;
	REQUIRE(StateSyncer::ValidateExistingArchiveForHeader(zipPath, pGoodHeader, &validationError));
	REQUIRE(validationError.empty());

	const BlockHeaderPtr pWrongHeader = Global::GetGenesisHeader();
	REQUIRE_FALSE(StateSyncer::ValidateExistingArchiveForHeader(zipPath, pWrongHeader, &validationError));
	REQUIRE_FALSE(validationError.empty());

	fs::remove_all(root, error);
}
