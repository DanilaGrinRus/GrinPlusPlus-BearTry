#pragma once

#include "Message.h"

#include "../../PMMR/Models/SegmentIdentifier.h"

#include <Crypto/Models/Hash.h>

// Request for a PMMR segment: block hash and segment identifier.
// Shared by GetOutputBitmapSegment (21), GetOutputSegment (23),
// GetRangeProofSegment (25) and GetKernelSegment (27); the wire body is identical.
class SegmentRequestMessage : public IMessage
{
public:
	SegmentRequestMessage(
		const MessageTypes::EMessageType messageType,
		Hash blockHash,
		SegmentIdentifier identifier
	)
		: m_messageType(messageType),
		m_blockHash(std::move(blockHash)),
		m_identifier(identifier)
	{
	}

	IMessagePtr Clone() const final { return IMessagePtr(new SegmentRequestMessage(*this)); }

	MessageTypes::EMessageType GetMessageType() const final { return m_messageType; }

	const Hash& GetBlockHash() const noexcept { return m_blockHash; }
	const SegmentIdentifier& GetIdentifier() const noexcept { return m_identifier; }

	static SegmentRequestMessage Deserialize(ByteBuffer& byteBuffer, const MessageTypes::EMessageType messageType)
	{
		Hash blockHash = byteBuffer.ReadBigInteger<32>();
		SegmentIdentifier identifier = SegmentIdentifier::Deserialize(byteBuffer);

		return SegmentRequestMessage(messageType, std::move(blockHash), identifier);
	}

protected:
	void SerializeBody(Serializer& serializer) const final
	{
		serializer.AppendBigInteger(m_blockHash);
		m_identifier.Serialize(serializer);
	}

private:
	MessageTypes::EMessageType m_messageType;
	Hash m_blockHash;
	SegmentIdentifier m_identifier;
};
