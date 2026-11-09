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
			EKernelFeatures::DEFAULT_KERNEL,
			Fee(0, 0),
			0,
			MakeCommitment(seed),
			MakeSignature(seed));
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

	InMemoryPmmr<BitmapChunk> MakeBitmapMmr(const std::vector<uint64_t>& setBits)
	{
		std::vector<BitmapChunk> chunks(1);
		for (const uint64_t bit : setBits) {
			chunks[0].Set((size_t)bit, true);
		}

		return InMemoryPmmr<BitmapChunk>(std::move(chunks));
	}
}

TEST_CASE("PibdSession::CapabilityGate")
{
	REQUIRE(!PibdSession::PeerSupportsPibd(Capabilities(Capabilities::UNKNOWN)));
	REQUIRE(!PibdSession::PeerSupportsPibd(Capabilities(Capabilities::FAST_SYNC_NODE)));
	REQUIRE(!PibdSession::PeerSupportsPibd(Capabilities(Capabilities::PIBD_HIST)));
	REQUIRE(PibdSession::PeerSupportsPibd(
		Capabilities((uint32_t)Capabilities::PIBD_HIST | (uint32_t)Capabilities::PIBD_HIST_1)));
	REQUIRE(PibdSession::PeerSupportsPibd(Capabilities(Capabilities::PIBD_HIST_1)));
}

TEST_CASE("PibdSession::EndToEndMessageFlow")
{
	const uint64_t numOutputs = 120;
	const uint64_t numKernels = 8;

	VectorSegmentBitmap expectedBitmap(numOutputs);
	expectedBitmap.Set(0, true);
	expectedBitmap.Set(70, true);

	const InMemoryPmmr<BitmapChunk> bitmapMmr = MakeBitmapMmr({ 0, 70 });
	const InMemoryPmmr<OutputIdentifier> outputs = MakeTree<OutputIdentifier>(numOutputs, MakeOutput);
	const InMemoryPmmr<RangeProof> rangeProofs = MakeTree<RangeProof>(numOutputs, MakeRangeProof);
	const InMemoryPmmr<TransactionKernel> kernels = MakeTree<TransactionKernel>(numKernels, MakeKernel);

	PibdDownload::Horizon horizon;
	horizon.BlockHash = Hash::ValueOf(9);
	horizon.Height = 5000;
	horizon.OutputMmrSize = MmrSizeForLeaves(numOutputs);
	horizon.KernelMmrSize = MmrSizeForLeaves(numKernels);
	horizon.OutputRoot = MMRHashUtil::HashParentWithIndex(outputs.GetRoot(), bitmapMmr.GetRoot(), outputs.GetSize());
	horizon.RangeProofRoot = rangeProofs.GetRoot();
	horizon.KernelRoot = kernels.GetRoot();

	REQUIRE(horizon.OutputMmrSize == outputs.GetSize());
	REQUIRE(horizon.KernelMmrSize == kernels.GetSize());

	PibdSession session(horizon);
	REQUIRE(session.GetDownload().GetPhase() == PibdDownload::EPhase::Bitmap);

	// 1. Bitmap request message.
	std::vector<IMessagePtr> requests = session.CreateRequests(15);
	REQUIRE(requests.size() == 1);
	REQUIRE(requests[0]->GetMessageType() == MessageTypes::GetOutputBitmapSegment);
	const std::shared_ptr<SegmentRequestMessage> bitmapRequest =
		std::dynamic_pointer_cast<SegmentRequestMessage>(requests[0]);
	REQUIRE(bitmapRequest != nullptr);
	REQUIRE(bitmapRequest->GetBlockHash() == horizon.BlockHash);
	REQUIRE(bitmapRequest->GetIdentifier() ==
		SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0));

	// 2. Bitmap response.
	const Segment<BitmapChunk> bitmapSegmentPmmr = SegmentGenerator<BitmapChunk>::Generate(
		bitmapMmr, SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0));
	const SegmentResponseMessage bitmapResponse = SegmentResponseMessage::CreateOutputBitmap(
		horizon.BlockHash, BitmapSegment::FromSegment(bitmapSegmentPmmr), outputs.GetRoot());
	REQUIRE(session.AcceptResponse(bitmapResponse));
	REQUIRE(session.GetDownload().GetPhase() == PibdDownload::EPhase::Trees);

	// A response for another horizon block is ignored, not applied.
	const SegmentResponseMessage staleResponse = SegmentResponseMessage::CreateOutputBitmap(
		HASH::ZERO, BitmapSegment::FromSegment(bitmapSegmentPmmr), outputs.GetRoot());
	REQUIRE(!session.AcceptResponse(staleResponse));

	// 3. Tree request messages (output, rangeproof, kernel).
	requests = session.CreateRequests(15);
	REQUIRE(requests.size() == 3);
	REQUIRE(requests[0]->GetMessageType() == MessageTypes::GetOutputSegment);
	REQUIRE(requests[1]->GetMessageType() == MessageTypes::GetRangeProofSegment);
	REQUIRE(requests[2]->GetMessageType() == MessageTypes::GetKernelSegment);

	const Segment<OutputIdentifier> outputSegment = SegmentGenerator<OutputIdentifier>::Generate(
		outputs, SegmentIdentifier(PibdParams::OUTPUT_SEGMENT_HEIGHT, 0), &expectedBitmap);
	const Segment<RangeProof> rangeProofSegment = SegmentGenerator<RangeProof>::Generate(
		rangeProofs, SegmentIdentifier(PibdParams::RANGEPROOF_SEGMENT_HEIGHT, 0), &expectedBitmap);
	const Segment<TransactionKernel> kernelSegment = SegmentGenerator<TransactionKernel>::Generate(
		kernels, SegmentIdentifier(PibdParams::KERNEL_SEGMENT_HEIGHT, 0));

	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateOutputs(
		horizon.BlockHash, outputSegment, bitmapMmr.GetRoot())));
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateRangeProofs(
		horizon.BlockHash, rangeProofSegment)));
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateKernels(
		horizon.BlockHash, kernelSegment)));

	REQUIRE(session.GetDownload().IsComplete());
	REQUIRE(session.GetDownload().GetReceivedCount() == 4);
	REQUIRE(session.GetDownload().GetOutputs().GetStoredLeafCount() > 0);
	REQUIRE(session.GetDownload().GetOutputs().GetStoredLeafCount() < numOutputs);   // pruned
	REQUIRE(session.GetDownload().GetKernels().GetStoredLeafCount() == numKernels);  // never pruned
	REQUIRE(session.GetDownload().GetRangeProofs().GetStoredLeafCount() ==
		session.GetDownload().GetOutputs().GetStoredLeafCount());
	REQUIRE(session.CreateRequests(15).empty());
}

TEST_CASE("PibdSession::RejectsMismatchedPayload")
{
	const uint64_t numOutputs = 4;
	const InMemoryPmmr<BitmapChunk> bitmapMmr = MakeBitmapMmr({ 0 });
	const InMemoryPmmr<OutputIdentifier> outputs = MakeTree<OutputIdentifier>(numOutputs, MakeOutput);
	const InMemoryPmmr<RangeProof> rangeProofs = MakeTree<RangeProof>(numOutputs, MakeRangeProof);
	const InMemoryPmmr<TransactionKernel> kernels = MakeTree<TransactionKernel>(2, MakeKernel);

	PibdDownload::Horizon horizon;
	horizon.BlockHash = Hash::ValueOf(11);
	horizon.OutputMmrSize = MmrSizeForLeaves(numOutputs);
	horizon.KernelMmrSize = MmrSizeForLeaves(2);
	horizon.OutputRoot = MMRHashUtil::HashParentWithIndex(outputs.GetRoot(), bitmapMmr.GetRoot(), outputs.GetSize());
	horizon.RangeProofRoot = rangeProofs.GetRoot();
	horizon.KernelRoot = kernels.GetRoot();

	PibdSession session(horizon);
	const Segment<BitmapChunk> bitmapSegmentPmmr = SegmentGenerator<BitmapChunk>::Generate(
		bitmapMmr, SegmentIdentifier(PibdParams::BITMAP_SEGMENT_HEIGHT, 0));
	REQUIRE(session.AcceptResponse(SegmentResponseMessage::CreateOutputBitmap(
		horizon.BlockHash, BitmapSegment::FromSegment(bitmapSegmentPmmr), outputs.GetRoot())));

	// A tampered kernel segment (valid shape, wrong leaf data) is routed to the
	// kernel tree and rejected by the proof check.
	const Segment<TransactionKernel> kernelSegment = SegmentGenerator<TransactionKernel>::Generate(
		kernels, SegmentIdentifier(PibdParams::KERNEL_SEGMENT_HEIGHT, 0));
	std::vector<TransactionKernel> tamperedLeaves = kernelSegment.GetLeafData();
	REQUIRE(!tamperedLeaves.empty());
	tamperedLeaves[0] = MakeKernel(200);
	const Segment<TransactionKernel> tampered(
		kernelSegment.GetIdentifier(),
		kernelSegment.GetHashPositions(),
		kernelSegment.GetHashes(),
		kernelSegment.GetLeafPositions(),
		tamperedLeaves,
		kernelSegment.GetProof());
	REQUIRE_THROWS_AS(
		session.AcceptResponse(SegmentResponseMessage::CreateKernels(horizon.BlockHash, tampered)),
		SegmentException);

	// The untouched kernel segment is still accepted afterwards.
	REQUIRE(session.AcceptResponse(
		SegmentResponseMessage::CreateKernels(horizon.BlockHash, kernelSegment)));
}
