#pragma once

#include "PibdDownload.h"
#include "PibdParams.h"

#include "../Messages/SegmentRequestMessage.h"
#include "../Messages/SegmentResponseMessage.h"

#include <P2P/Capabilities.h>

#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Hash.h>
#include <Crypto/Models/RangeProof.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Transport-facing wrapper around PibdDownload:
//   * builds the request messages (ids 21/23/25/27) for the next batch;
//   * consumes segment responses (ids 22/24/26/28), rejecting responses for a
//     different horizon block and routing payloads to the right tree assembler.
//
// Peer/timer policy stays outside: the caller decides which peers to ask and
// when a request has timed out.
class PibdSession
{
public:
	explicit PibdSession(PibdDownload::Horizon horizon)
		: m_download(std::move(horizon))
	{
	}

	// PIBD_HIST predates the PIBD serialization fix (#3705); only PIBD_HIST_1
	// peers can be asked for segments.
	static bool PeerSupportsPibd(const Capabilities& capabilities)
	{
		return capabilities.HasCapability(Capabilities::PIBD_HIST_1);
	}

	static MessageTypes::EMessageType RequestType(const PibdDownload::ETree tree)
	{
		switch (tree) {
			case PibdDownload::ETree::Bitmap:
				return MessageTypes::GetOutputBitmapSegment;
			case PibdDownload::ETree::Output:
				return MessageTypes::GetOutputSegment;
			case PibdDownload::ETree::RangeProof:
				return MessageTypes::GetRangeProofSegment;
			case PibdDownload::ETree::Kernel:
				return MessageTypes::GetKernelSegment;
		}

		return MessageTypes::Error;
	}

	const PibdDownload& GetDownload() const noexcept { return m_download; }
	PibdDownload& GetDownload() noexcept { return m_download; }

	const Hash& GetHorizonBlockHash() const noexcept { return m_download.GetHorizon().BlockHash; }

	// The request a response answers, derived from its message type and payload.
	// Empty for message types that are not segment responses.
	static std::optional<PibdDownload::SegmentRequest> ResponseRequest(const SegmentResponseMessage& message)
	{
		switch (message.GetMessageType()) {
			case MessageTypes::OutputBitmapSegment:
			{
				const BitmapSegment* segment = std::get_if<BitmapSegment>(&message.GetPayload());
				return segment == nullptr
					? std::nullopt
					: std::optional<PibdDownload::SegmentRequest>(
						PibdDownload::SegmentRequest{ PibdDownload::ETree::Bitmap, segment->GetIdentifier() });
			}
			case MessageTypes::OutputSegment:
			{
				const Segment<OutputIdentifier>* segment =
					std::get_if<Segment<OutputIdentifier>>(&message.GetPayload());
				return segment == nullptr
					? std::nullopt
					: std::optional<PibdDownload::SegmentRequest>(
						PibdDownload::SegmentRequest{ PibdDownload::ETree::Output, segment->GetIdentifier() });
			}
			case MessageTypes::RangeProofSegment:
			{
				const Segment<RangeProof>* segment = std::get_if<Segment<RangeProof>>(&message.GetPayload());
				return segment == nullptr
					? std::nullopt
					: std::optional<PibdDownload::SegmentRequest>(
						PibdDownload::SegmentRequest{ PibdDownload::ETree::RangeProof, segment->GetIdentifier() });
			}
			case MessageTypes::KernelSegment:
			{
				const Segment<TransactionKernel>* segment =
					std::get_if<Segment<TransactionKernel>>(&message.GetPayload());
				return segment == nullptr
					? std::nullopt
					: std::optional<PibdDownload::SegmentRequest>(
						PibdDownload::SegmentRequest{ PibdDownload::ETree::Kernel, segment->GetIdentifier() });
			}
			default:
				return std::nullopt;
		}
	}

	// Build a single request message (used by the scheduler).
	IMessagePtr CreateRequest(const PibdDownload::SegmentRequest& request) const
	{
		return std::make_shared<SegmentRequestMessage>(
			RequestType(request.Tree), m_download.GetHorizon().BlockHash, request.Identifier);
	}

	// Request messages for the next batch, already addressed to the horizon block.
	std::vector<IMessagePtr> CreateRequests(const size_t maxCount) const
	{
		std::vector<IMessagePtr> messages;
		for (const PibdDownload::SegmentRequest& request : m_download.NextRequests(maxCount)) {
			messages.push_back(CreateRequest(request));
		}

		return messages;
	}

	// Consume a segment response. Returns false when the response is for another
	// horizon block (late or wrong peer) and therefore ignored; throws
	// SegmentException for a malformed payload.
	bool AcceptResponse(const SegmentResponseMessage& message)
	{
		if (message.GetBlockHash() != m_download.GetHorizon().BlockHash) {
			return false;
		}

		switch (message.GetMessageType()) {
			case MessageTypes::OutputBitmapSegment:
			{
				const BitmapSegment* segment = std::get_if<BitmapSegment>(&message.GetPayload());
				if (segment == nullptr) {
					throw SegmentException(ESegmentError::Mismatch, "OutputBitmapSegment without a bitmap segment payload");
				}

				const std::optional<Hash>& outputMmrRoot = message.GetExtraRoot();
				if (!outputMmrRoot.has_value()) {
					throw SegmentException(
						ESegmentError::Mismatch, "OutputBitmapSegment without the output root");
				}

				return m_download.AcceptBitmapSegment(*segment, outputMmrRoot.value());
			}
			case MessageTypes::OutputSegment:
			{
				const Segment<OutputIdentifier>* segment =
					std::get_if<Segment<OutputIdentifier>>(&message.GetPayload());
				if (segment == nullptr) {
					throw SegmentException(ESegmentError::Mismatch, "OutputSegment without an output segment payload");
				}

				return m_download.AcceptOutputSegment(*segment);
			}
			case MessageTypes::RangeProofSegment:
			{
				const Segment<RangeProof>* segment = std::get_if<Segment<RangeProof>>(&message.GetPayload());
				if (segment == nullptr) {
					throw SegmentException(ESegmentError::Mismatch, "RangeProofSegment without a range proof payload");
				}

				return m_download.AcceptRangeProofSegment(*segment);
			}
			case MessageTypes::KernelSegment:
			{
				const Segment<TransactionKernel>* segment =
					std::get_if<Segment<TransactionKernel>>(&message.GetPayload());
				if (segment == nullptr) {
					throw SegmentException(ESegmentError::Mismatch, "KernelSegment without a kernel payload");
				}

				return m_download.AcceptKernelSegment(*segment);
			}
			default:
				throw SegmentException(
					ESegmentError::Mismatch,
					"Message type " + std::to_string((uint32_t)message.GetMessageType()) +
						" is not a segment response");
		}
	}

private:
	PibdDownload m_download;
};
