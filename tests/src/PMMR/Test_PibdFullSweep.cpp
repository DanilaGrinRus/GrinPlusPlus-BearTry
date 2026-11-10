#define _CRT_SECURE_NO_WARNINGS
#include <catch.hpp>

#include <Core/Models/BlockHeader.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <P2P/PIBD/PibdParams.h>
#include <P2P/PIBD/PibdSegmentService.h>
#include <PMMR/RangeProofBatchVerifier.h>
#include <PMMR/TxHashSet.h>
#include <PMMR/TxHashSetImpl.h>
#include <P2P/SyncStatus.h>
#include <PMMR/KernelMMR.h>
#include <PMMR/OutputPMMR.h>
#include <PMMR/RangeProofPMMR.h>
#include <PMMR/Models/SegmentIdentifier.h>

#include <filesystem.h>
#include <fstream>

TEST_CASE("PibdSegmentService::FullSweepPpCkpt", "[.]")
{
	const char* envState = std::getenv("GRINPP_BENCH_STATE");
	const fs::path stateDir = envState != nullptr ? fs::path(envState) : fs::path("fixtures/pibd/full-sweep/state");
	const fs::path headerPath = "fixtures/pibd/full-sweep/header.bin";
	if (!fs::exists(stateDir / "output/pmmr_hash.bin") || !fs::exists(headerPath)) {
		FAIL("pp-ckpt/fixture absent; full PIBD sweep was not executed");
	}

	std::ifstream headerFile(headerPath, std::ios::binary);
	const std::vector<uint8_t> headerBytes(
		(std::istreambuf_iterator<char>(headerFile)), std::istreambuf_iterator<char>());
	ByteBuffer headerBuffer(headerBytes);
	const BlockHeader header = BlockHeader::Deserialize(headerBuffer);

	std::shared_ptr<OutputPMMR> pOutputPMMR = OutputPMMR::Load(stateDir);
	std::shared_ptr<RangeProofPMMR> pRangeProofPMMR = RangeProofPMMR::Load(stateDir);
	std::shared_ptr<KernelMMR> pKernelMMR = KernelMMR::Load(stateDir);
	PibdSegmentService service(pOutputPMMR, pRangeProofPMMR, pKernelMMR);
	const VectorSegmentBitmap bitmap = service.GetOutputBitmap();
	const Hash bitmapRoot = service.GetBitmapMmrRoot();
	REQUIRE(service.GetOutputMmrSize() == header.GetOutputMMRSize());
	REQUIRE(service.GetKernelMmrSize() == header.GetKernelMMRSize());
	const auto bitmapMmr = service.BuildBitmapMmr();
	const uint64_t bitmapSegments = SegmentIdentifier::CountSegmentsRequired(
		bitmapMmr.GetSize(), PibdParams::BITMAP_SEGMENT_HEIGHT);
	uint64_t bitmapFailures = 0;
	for (uint64_t idx = 0; idx < bitmapSegments; ++idx) {
		try {
			const auto segment = service.GetBitmapSegment(
				SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, idx));
			Serializer serializer(EProtocolVersion::V1000);
			segment.Serialize(serializer);
			ByteBuffer bytes(serializer.GetBytes(), EProtocolVersion::V1000);
			const auto decoded = BitmapSegment::Deserialize(bytes);
			if (bytes.GetRemainingSize() != 0) {
				throw std::runtime_error("Trailing bitmap payload bytes");
			}
			decoded.ToSegment().ValidateWithOtherRoot(
				bitmapMmr.GetSize(), nullptr, header.GetOutputRoot(),
				header.GetOutputMMRSize(), service.GetOutputMmrRoot(), true);
		} catch (const std::exception& e) {
			if (bitmapFailures++ < 3) {
				WARN("bitmap segment " << idx << " failed: " << e.what());
			}
		}
	}

	uint64_t outputFailures = 0;
	uint64_t rangeProofFailures = 0;
	uint64_t kernelFailures = 0;

	const uint64_t outputSegments = SegmentIdentifier::CountSegmentsRequired(
		header.GetOutputMMRSize(), PibdParams::OUTPUT_SEGMENT_HEIGHT);
	for (uint64_t idx = 0; idx < outputSegments; ++idx) {
		try {
			const auto segment = service.GetOutputSegment(
				SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, idx));
			segment.ValidateWithOtherRoot(
				header.GetOutputMMRSize(), &bitmap, header.GetOutputRoot(),
				header.GetOutputMMRSize(), bitmapRoot, false);
		} catch (const std::exception& e) {
			if (outputFailures++ < 3) {
				WARN("output segment " << idx << " failed: " << e.what());
			}
		}
	}

	const uint64_t rangeProofSegments = SegmentIdentifier::CountSegmentsRequired(
		header.GetOutputMMRSize(), PibdParams::RANGEPROOF_SEGMENT_HEIGHT);
	for (uint64_t idx = 0; idx < rangeProofSegments; ++idx) {
		try {
			const auto segment = service.GetRangeProofSegment(
				SegmentIdentifier(PibdParams::RANGEPROOF_SEGMENT_HEIGHT, idx));
			segment.ValidateWithBitmap(
				header.GetOutputMMRSize(), bitmap, header.GetRangeProofRoot());
		} catch (const std::exception& e) {
			if (rangeProofFailures++ < 3) {
				WARN("rangeproof segment " << idx << " failed: " << e.what());
			}
		}
	}

	const uint64_t kernelSegments = SegmentIdentifier::CountSegmentsRequired(
		header.GetKernelMMRSize(), PibdParams::KERNEL_SEGMENT_HEIGHT);
	for (uint64_t idx = 0; idx < kernelSegments; ++idx) {
		try {
			const auto segment = service.GetKernelSegment(
				SegmentIdentifier(PibdParams::KERNEL_SEGMENT_HEIGHT, idx));
			segment.Validate(header.GetKernelMMRSize(), header.GetKernelRoot());
		} catch (const std::exception& e) {
			if (kernelFailures++ < 3) {
				WARN("kernel segment " << idx << " failed: " << e.what());
			}
		}
	}

	WARN("sweep: bitmap " << bitmapFailures << "/" << bitmapSegments
		<< ", output " << outputFailures << "/" << outputSegments
		<< ", rangeproof " << rangeProofFailures << "/" << rangeProofSegments
		<< ", kernel " << kernelFailures << "/" << kernelSegments);
	CHECK(bitmapFailures == 0);
	CHECK(outputFailures == 0);
	CHECK(rangeProofFailures == 0);
	CHECK(kernelFailures == 0);
}

TEST_CASE("FullRangeProofWorkerBenchmark", "[rangebench][.]")
{
	const char* envState = std::getenv("GRINPP_BENCH_STATE");
	const fs::path stateDir = envState != nullptr ? fs::path(envState) : fs::path("fixtures/pibd/full-sweep/state");
	auto outputs = OutputPMMR::Load(stateDir);
	auto proofs = RangeProofPMMR::Load(stateDir);
	TxHashSet state(nullptr, outputs, proofs, nullptr);
	std::cout << "RANGEBENCH state output_mmr=" << outputs->GetSize() << std::endl;
	uint64_t expectedProofs = 0;
	auto run = [&](const char* label, const int workers) {
		SyncStatus status;
		RangeProofBatchVerifier::Stats stats;
		const auto start = std::chrono::steady_clock::now();
		const bool ok = workers == 1
			? RangeProofBatchVerifier::VerifySequential(state, status, &stats)
			: RangeProofBatchVerifier::VerifyParallel(state, status, workers, &stats);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
		std::cout << "RANGEBENCH label=" << label << " workers=" << workers
			<< " verified=" << stats.VerifiedProofs << " ms=" << ms
			<< " ok=" << (ok ? 1 : 0) << std::endl;
		REQUIRE(ok);
		if (expectedProofs == 0) expectedProofs = stats.VerifiedProofs;
		REQUIRE(stats.VerifiedProofs == expectedProofs);
		REQUIRE(stats.SubmittedProofs == expectedProofs);
	};
	run("1", 1);
	run("2", 2);
	run("3", 3);
	run("4", 4);
	const auto autoSelection = RangeProofBatchVerifier::ConfiguredWorkerSelection();
	run("auto", autoSelection.Workers);
}







