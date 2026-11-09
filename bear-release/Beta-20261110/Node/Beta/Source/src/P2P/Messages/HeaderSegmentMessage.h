#pragma once

#include "Message.h"
#include <P2P/PihdParams.h>
#include "../../PMMR/Models/SegmentIdentifier.h"
#include <Core/Models/BlockHeader.h>

#include <vector>
#include <Core/Exceptions/DeserializationException.h>

// PIHD response: type 30. Body: SegmentIdentifier, u16 count, count headers.
// Headers remain untrusted until the receiver checks sequence, PoW and links.
class HeaderSegmentMessage : public IMessage
{
public:
	HeaderSegmentMessage(SegmentIdentifier identifier, std::vector<BlockHeader> headers)
		: m_identifier(identifier), m_headers(std::move(headers))
	{
	}

	IMessagePtr Clone() const final { return IMessagePtr(new HeaderSegmentMessage(*this)); }

	MessageTypes::EMessageType GetMessageType() const final
	{
		return MessageTypes::HeaderSegment;
	}

	const SegmentIdentifier& GetIdentifier() const noexcept { return m_identifier; }
	const std::vector<BlockHeader>& GetHeaders() const noexcept { return m_headers; }

	static HeaderSegmentMessage Deserialize(ByteBuffer& byteBuffer)
	{
		const SegmentIdentifier identifier = SegmentIdentifier::Deserialize(byteBuffer);
		const uint16_t count = byteBuffer.ReadU16();
		if (count > Pihd::HEADERS_PER_SEGMENT) {
			throw DESERIALIZATION_EXCEPTION_F(
				"PIHD segment declares {} headers, limit is {}",
				count,
				Pihd::HEADERS_PER_SEGMENT);
		}

		std::vector<BlockHeader> headers;
		headers.reserve(count);
		for (uint16_t i = 0; i < count; ++i) {
			headers.push_back(BlockHeader::Deserialize(byteBuffer));
		}

		return HeaderSegmentMessage(identifier, std::move(headers));
	}

protected:
	void SerializeBody(Serializer& serializer) const final
	{
		m_identifier.Serialize(serializer);
		serializer.Append<uint16_t>((uint16_t)m_headers.size());
		for (const BlockHeader& header : m_headers) {
			header.Serialize(serializer);
		}
	}

private:
	SegmentIdentifier m_identifier;
	std::vector<BlockHeader> m_headers;
};
