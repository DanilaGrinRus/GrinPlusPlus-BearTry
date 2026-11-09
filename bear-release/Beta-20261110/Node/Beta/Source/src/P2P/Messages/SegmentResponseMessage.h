#pragma once

#include "Message.h"

#include "../../PMMR/Models/BitmapSegment.h"
#include "../../PMMR/Models/Segment.h"

#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Hash.h>
#include <Crypto/Models/RangeProof.h>

#include <optional>
#include <utility>
#include <variant>

// Segment payloads. Each concrete message type carries exactly one of these.
// BitmapSegment only appears in OutputBitmapSegment (22); the three PMMR
// segment variants appear in OutputSegment (24), RangeProofSegment (26) and
// KernelSegment (28) respectively.
using SegmentPayload = std::variant<
	BitmapSegment,
	Segment<OutputIdentifier>,
	Segment<RangeProof>,
	Segment<TransactionKernel>>;

// Response to a segment request.
// Wire formats match grin v5.5.2 msg.rs:
//   22 OutputBitmapSegment: block_hash, BitmapSegment, output_root
//   24 OutputSegment:       block_hash, Segment<OutputIdentifier>, output_bitmap_root
//   26 RangeProofSegment:   block_hash, Segment<RangeProof>
//   28 KernelSegment:       block_hash, Segment<TransactionKernel>
class SegmentResponseMessage : public IMessage
{
public:
	static SegmentResponseMessage CreateOutputBitmap(Hash blockHash, BitmapSegment segment, Hash outputRoot)
	{
		return SegmentResponseMessage(
			MessageTypes::OutputBitmapSegment, std::move(blockHash), std::move(outputRoot), std::move(segment));
	}

	static SegmentResponseMessage CreateOutputs(Hash blockHash, Segment<OutputIdentifier> segment, Hash outputBitmapRoot)
	{
		return SegmentResponseMessage(
			MessageTypes::OutputSegment, std::move(blockHash), std::move(outputBitmapRoot), std::move(segment));
	}

	static SegmentResponseMessage CreateRangeProofs(Hash blockHash, Segment<RangeProof> segment)
	{
		return SegmentResponseMessage(
			MessageTypes::RangeProofSegment, std::move(blockHash), std::nullopt, std::move(segment));
	}

	static SegmentResponseMessage CreateKernels(Hash blockHash, Segment<TransactionKernel> segment)
	{
		return SegmentResponseMessage(
			MessageTypes::KernelSegment, std::move(blockHash), std::nullopt, std::move(segment));
	}

	static SegmentResponseMessage Deserialize(ByteBuffer& byteBuffer, const MessageTypes::EMessageType messageType)
	{
		Hash blockHash = byteBuffer.ReadBigInteger<32>();
		switch (messageType) {
			case MessageTypes::OutputBitmapSegment:
			{
				BitmapSegment segment = BitmapSegment::Deserialize(byteBuffer);
				Hash outputRoot = byteBuffer.ReadBigInteger<32>();
				return CreateOutputBitmap(std::move(blockHash), std::move(segment), std::move(outputRoot));
			}
			case MessageTypes::OutputSegment:
			{
				Segment<OutputIdentifier> segment = Segment<OutputIdentifier>::Deserialize(byteBuffer);
				Hash outputBitmapRoot = byteBuffer.ReadBigInteger<32>();
				return CreateOutputs(std::move(blockHash), std::move(segment), std::move(outputBitmapRoot));
			}
			case MessageTypes::RangeProofSegment:
			{
				Segment<RangeProof> segment = Segment<RangeProof>::Deserialize(byteBuffer);
				return CreateRangeProofs(std::move(blockHash), std::move(segment));
			}
			case MessageTypes::KernelSegment:
			{
				Segment<TransactionKernel> segment = Segment<TransactionKernel>::Deserialize(byteBuffer);
				return CreateKernels(std::move(blockHash), std::move(segment));
			}
			default:
				throw DESERIALIZATION_EXCEPTION_F(
					"Message type {} is not a segment response", (uint32_t)messageType);
		}
	}

	IMessagePtr Clone() const final { return IMessagePtr(new SegmentResponseMessage(*this)); }

	MessageTypes::EMessageType GetMessageType() const final { return m_messageType; }

	const Hash& GetBlockHash() const noexcept { return m_blockHash; }

	// Output root (bitmap segment) or output bitmap root (output segment).
	const std::optional<Hash>& GetExtraRoot() const noexcept { return m_extraRoot; }

	const SegmentPayload& GetPayload() const noexcept { return m_payload; }

	// Re-serializes the message body exactly as it appears on the wire. Used by
	// the PIBD resume journal so a restart can replay accepted segments without
	// keeping the network message object alive.
	std::vector<uint8_t> SerializePayload() const
	{
		Serializer serializer;
		serializer.AppendBigInteger(m_blockHash);
		std::visit([&serializer](const auto& payload) { payload.Serialize(serializer); }, m_payload);
		if (m_extraRoot.has_value()) {
			serializer.AppendBigInteger(m_extraRoot.value());
		}
		return serializer.GetBytes();
	}

protected:
	void SerializeBody(Serializer& serializer) const final
	{
		serializer.AppendBigInteger(m_blockHash);
		std::visit([&serializer](const auto& payload) { payload.Serialize(serializer); }, m_payload);
		if (m_extraRoot.has_value()) {
			serializer.AppendBigInteger(m_extraRoot.value());
		}
	}

private:
	SegmentResponseMessage(
		const MessageTypes::EMessageType messageType,
		Hash blockHash,
		std::optional<Hash> extraRoot,
		SegmentPayload payload
	)
		: m_messageType(messageType),
		m_blockHash(std::move(blockHash)),
		m_extraRoot(std::move(extraRoot)),
		m_payload(std::move(payload))
	{
	}

	MessageTypes::EMessageType m_messageType;
	Hash m_blockHash;
	std::optional<Hash> m_extraRoot;
	SegmentPayload m_payload;
};
