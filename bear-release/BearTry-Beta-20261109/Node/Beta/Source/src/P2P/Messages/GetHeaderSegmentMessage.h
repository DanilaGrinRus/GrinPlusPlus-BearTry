#pragma once

#include "Message.h"
#include <P2P/PihdParams.h>
#include "../../PMMR/Models/SegmentIdentifier.h"

// PIHD request: type 29. The body is exactly a SegmentIdentifier (height u8,
// idx u64), matching the staging Grin wire format.
class GetHeaderSegmentMessage : public IMessage
{
public:
	explicit GetHeaderSegmentMessage(const SegmentIdentifier& identifier)
		: m_identifier(identifier)
	{
	}

	IMessagePtr Clone() const final { return IMessagePtr(new GetHeaderSegmentMessage(*this)); }

	MessageTypes::EMessageType GetMessageType() const final
	{
		return MessageTypes::GetHeaderSegment;
	}

	const SegmentIdentifier& GetIdentifier() const noexcept { return m_identifier; }

	static GetHeaderSegmentMessage Deserialize(ByteBuffer& byteBuffer)
	{
		return GetHeaderSegmentMessage(SegmentIdentifier::Deserialize(byteBuffer));
	}

protected:
	void SerializeBody(Serializer& serializer) const final
	{
		m_identifier.Serialize(serializer);
	}

private:
	SegmentIdentifier m_identifier;
};
