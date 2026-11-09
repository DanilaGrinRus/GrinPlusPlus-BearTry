#include "HandShake.h"

#include "../Messages/HandMessage.h"
#include "../Messages/ShakeMessage.h"
#include "../Messages/BanReasonMessage.h"
#include "../ConnectionManager.h"
#include "../PIBD/PibdArchiveServer.h"

#include <Core/Exceptions/ProtocolException.h>
#include <Crypto/CSPRNG.h>
#include <Common/Logger.h>
#include <P2P/AdvertisedTip.h>

#include <memory>
#include <chrono>
#include <thread>
#include <algorithm>

static const uint64_t SELF_NONCE = CSPRNG::GenerateRandom(0, UINT64_MAX);

namespace
{
	// The capability mask is fixed for the lifetime of a handshake. It must come
	// from the serving component, not from a static constant: a node that cannot
	// serve the archive horizon must not claim PIBD (G-09 capability contract).
	Capabilities GetLocalCapabilities()
	{
		const std::shared_ptr<PibdArchiveServer> pArchiveServer = PibdArchiveServer::Instance();
		return Capabilities::Advertised(
			pArchiveServer != nullptr && pArchiveServer->ShouldAdvertise());
	}
}

void HandShake::PerformHandshake(const Socket::Ptr& pSocket, ConnectedPeer& connectedPeer) const
{
    EDirection direction = connectedPeer.GetDirection();
    LOG_TRACE_F(
        "Performing handshake with {}({})",
        pSocket,
        (direction == EDirection::INBOUND ? "inbound" : "outbound")
    );

    if (direction == EDirection::OUTBOUND) {
        PerformOutboundHandshake(pSocket, connectedPeer);
    } else {
        PerformInboundHandshake(pSocket, connectedPeer);
    }
}

void HandShake::PerformOutboundHandshake(const Socket::Ptr& pSocket, ConnectedPeer& connectedPeer) const
{
    // Send Hand Message
    TransmitHandMessage(pSocket);

    // Get Shake Message
    auto pReceived = RetrieveMessage(pSocket, *connectedPeer.GetPeer(), MessageTypes::Shake);
    if (pReceived == nullptr) {
        throw PROTOCOL_EXCEPTION("Shake message not received");
    }

    if (pReceived->GetMessageType() != MessageTypes::Shake) {
        throw PROTOCOL_EXCEPTION_F("Expected shake but received {}.", *pReceived);
    }

    ByteBuffer buffer(pReceived->GetPayload());
    ShakeMessage shakeMessage = ShakeMessage::Deserialize(buffer);

    // Only peers on the same chain may connect (Grin 5.5.2: BadHandshake).
    if (shakeMessage.GetHash() != Global::GetGenesisHash()) {
        throw PROTOCOL_EXCEPTION_F(
            "Shake message genesis block does not match ours. Peer {}",
            *connectedPeer.GetPeer()
        );
    }

    uint32_t version = (std::min)(P2P::PROTOCOL_VERSION, shakeMessage.GetVersion());
    connectedPeer.UpdateVersion(version);
    connectedPeer.UpdateCapabilities(shakeMessage.GetCapabilities());
    connectedPeer.UpdateUserAgent(shakeMessage.GetUserAgent());
    connectedPeer.UpdateTotals(shakeMessage.GetTotalDifficulty(), 0);
}

void HandShake::PerformInboundHandshake(const Socket::Ptr& pSocket, ConnectedPeer& connectedPeer) const
{
    // Get Hand Message
    auto pReceived = RetrieveMessage(pSocket, *connectedPeer.GetPeer(), MessageTypes::Hand);
    if (pReceived == nullptr) {
        throw PROTOCOL_EXCEPTION("Hand message not received.");
    }

    if (pReceived->GetMessageType() != MessageTypes::Hand) {
        throw PROTOCOL_EXCEPTION_F("Expected hand but received {}", *pReceived);
    }

    ByteBuffer byteBuffer(pReceived->GetPayload());
    HandMessage hand_message = HandMessage::Deserialize(byteBuffer);
    
    if (hand_message.GetNonce() == SELF_NONCE) {
        throw PROTOCOL_EXCEPTION("Connected to self");
    }

    // Only peers on the same chain may connect (Grin 5.5.2: BadHandshake).
    if (hand_message.GetHash() != Global::GetGenesisHash()) {
        throw PROTOCOL_EXCEPTION_F(
            "Hand message genesis block does not match ours. Peer {}",
            *connectedPeer.GetPeer()
        );
    }

    if (m_connectionManager.IsConnected(connectedPeer.GetIPAddress())) {
        throw PROTOCOL_EXCEPTION("Already connected to peer");
    }

    connectedPeer.UpdateCapabilities(hand_message.GetCapabilities());
    connectedPeer.UpdateUserAgent(hand_message.GetUserAgent());
    connectedPeer.UpdateTotals(hand_message.GetTotalDifficulty(), 0);

    uint32_t version = (std::min)(P2P::PROTOCOL_VERSION, hand_message.GetVersion());
    connectedPeer.UpdateVersion(version);

    // Send Shake Message
    TransmitShakeMessage(pSocket, version);
}

void HandShake::TransmitHandMessage(const Socket::Ptr& pSocket) const
{
    IPAddress localHostIP = IPAddress::CreateV4({ 0x7F, 0x00, 0x00, 0x01 });
    HandMessage hand(
        P2P::PROTOCOL_VERSION,
        GetLocalCapabilities(),
        SELF_NONCE,
        Global::GetGenesisHash(),
        P2P::GetAdvertisedDifficulty(*m_pSyncStatus),
        SocketAddress(localHostIP, Global::GetConfig().GetP2PPort()),
        SocketAddress(localHostIP, pSocket->GetPort()),
        P2P::USER_AGENT
    );

    if (!pSocket->SendSync(hand.Serialize(ProtocolVersion::Local()), true)) {
        throw PROTOCOL_EXCEPTION("Failed to send hand message.");
    }
}

void HandShake::TransmitShakeMessage(const Socket::Ptr& pSocket, const uint32_t protocolVersion) const
{
    ShakeMessage shakeMessage(
        protocolVersion,
        GetLocalCapabilities(),
        Global::GetGenesisHash(),
        P2P::GetAdvertisedDifficulty(*m_pSyncStatus),
        P2P::USER_AGENT
    );

    if (!pSocket->SendSync(shakeMessage.Serialize(ProtocolVersion::ToEnum(protocolVersion)), true)) {
        throw PROTOCOL_EXCEPTION("Failed to send shake message.");
    }
}

std::unique_ptr<RawMessage> HandShake::RetrieveMessage(
    const Socket::Ptr& pSocket, const Peer& peer, MessageTypes::EMessageType expectedType) const
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    const auto readUntilDeadline = [&](const size_t length, const bool incrementCount) {
        std::vector<uint8_t> data(length);
        size_t offset = 0;
        const auto& socket = pSocket->GetAsioSocket();
        while (offset < length && std::chrono::steady_clock::now() < deadline && Global::IsRunning()) {
            asio::error_code ec;
            const size_t available = socket->available(ec);
            if (ec) return std::vector<uint8_t>{};
            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            const size_t amount = (std::min)(available, length - offset);
            const size_t received = socket->read_some(asio::buffer(data.data() + offset, amount), ec);
            if (ec || received == 0) return std::vector<uint8_t>{};
            offset += received;
        }
        if (offset != length) return std::vector<uint8_t>{};
        if (incrementCount) pSocket->GetRateCounter().AddMessageReceived();
        return data;
    };

    std::vector<uint8_t> headerBuffer = readUntilDeadline(11, true);
    if (headerBuffer.size() != 11) return nullptr;
    MessageHeader messageHeader = ByteBuffer(std::move(headerBuffer)).Read<MessageHeader>();
    // Preserve the protocol's existing 4x forward-compatibility allowance,
    // but do not read an unrelated type or an oversized handshake body.
    if (messageHeader.GetMessageType() != expectedType ||
        messageHeader.GetMessageLength() > 4 * MessageTypes::GetMaximumSize(expectedType)) {
        throw PROTOCOL_EXCEPTION("Invalid handshake message type or length");
    }
    std::vector<uint8_t> payload = readUntilDeadline(messageHeader.GetMessageLength(), false);
    if (payload.size() != messageHeader.GetMessageLength()) {
        throw PROTOCOL_EXCEPTION("Expected payload not received");
    }
    peer.UpdateLastContactTime();
    return std::make_unique<RawMessage>(std::move(messageHeader), std::move(payload));
}
