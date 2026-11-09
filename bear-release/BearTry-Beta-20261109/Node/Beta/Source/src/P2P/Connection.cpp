#include "Connection.h"
#include "MessageProcessor.h"
#include "ConnectionManager.h"
#include "Messages/PingMessage.h"
#include <P2P/AdvertisedTip.h>
#include "Messages/GetPeerAddressesMessage.h"
#include "Seed/HandShake.h"
#include "Sync/PeerRatePolicy.h"

#include <Net/SocketException.h>
#include <Common/Util/ThreadUtil.h>
#include <Common/Logger.h>
#include <thread>
#include <chrono>
#include <memory>

void Connection::Connect()
{
    std::thread connect_thr(Thread_Connect, shared_from_this());
    ThreadUtil::Detach(connect_thr);
}

void Connection::Disconnect()
{
    std::unique_lock<std::mutex> write_lock(m_mutex);

    if (!m_terminate) {
        m_terminate = true;
        m_pSocket->CloseSocket();
        m_connectedPeer.GetPeer()->SetConnected(false);
    }
}

void Connection::CloseAfterMessage()
{
    // ProcessMessage runs under HandleReceivedBody's m_mutex. Do not call
    // Disconnect() here, as that would acquire the same mutex again.
    m_receivingDisabled = true;
    m_terminate = true;
    m_pSocket->CloseSocket();
    GetPeer()->SetConnected(false);
}

bool Connection::IsConnectionActive() const
{
    if (m_terminate) {
        LOG_DEBUG_F("Socket has been terminated for {}", GetPeer());
        return false;
    }

    if ((m_lastReceived + std::chrono::seconds(60)) < system_clock::now()) {
        LOG_DEBUG_F("{} has not received a message in more than a minute", GetPeer());
        return false;
    }

    if (GetPeer()->IsBanned()) {
        LOG_DEBUG_F("{} is banned", GetPeer());
        return false;
    }

    return m_pSocket->IsActive();
}

void Connection::SendAsync(const IMessage& message)
{
    if (!m_sendingDisabled) {
        std::vector<uint8_t> serialized = message.Serialize(GetProtocolVersion());
        if (message.GetMessageType() != MessageTypes::Ping && message.GetMessageType() != MessageTypes::Pong) {
            LOG_TRACE_F(
                "Sending {}b '{}' message to {}",
                serialized.size(),
                MessageTypes::ToString(message.GetMessageType()),
                m_pSocket
            );
        }

        m_pSocket->SendAsync(serialized);
    }
}

bool Connection::ExceedsRateLimit() const
{
    // Our own outbound requests are under local control and are not evidence
    // that the remote peer is abusive (Grin 5.5.2 checks incoming only).
    return PeerRatePolicy::IsAbusive(m_pSocket->GetRateCounter().GetReceivedInLastMinute());
}

bool Connection::AllowPibdRequest()
{
    std::lock_guard<std::mutex> lock(m_pibdRateMutex);
    const auto now = std::chrono::steady_clock::now();
    while (!m_pibdRequests.empty() &&
        m_pibdRequests.front() + std::chrono::seconds(60) <= now) {
        m_pibdRequests.pop_front();
    }

    if (m_pibdRequests.size() >= P2P::MAX_PIBD_REQUESTS_PER_MINUTE) {
        return false;
    }

    m_pibdRequests.push_back(now);
    return true;
}

void Connection::Thread_Connect(std::shared_ptr<Connection> pConnection)
{
    LoggerAPI::SetThreadName("PEER");

    try {
        if (pConnection->GetDirection() == EDirection::OUTBOUND) {
            pConnection->ConnectOutbound();
        }

        pConnection->GetSocket()->SetDefaultOptions();
        pConnection->GetSocket()->SetBlocking(true);
        
        HandShake(pConnection->m_connectionManager, pConnection->m_pSyncStatus)
            .PerformHandshake(pConnection->m_pSocket, pConnection->m_connectedPeer);

        pConnection->m_lastPing = system_clock::now();
        pConnection->m_lastReceived = system_clock::now();

        LOG_DEBUG_F("Connected to {}", pConnection->m_connectedPeer);
        pConnection->GetPeer()->SetConnected(true);
        pConnection->m_connectionManager.AddConnection(pConnection);

        if (pConnection->GetDirection() == EDirection::OUTBOUND) {
            pConnection->SendSync(GetPeerAddressesMessage(Capabilities::ECapability::FAST_SYNC_NODE));
            LOG_DEBUG_F("Capabilities sent to {}", pConnection->m_connectedPeer);
        }

        // Initiate the asynchronous read on the long-lived I/O executor, not
        // on this detached handshake thread. Windows may cancel overlapped I/O
        // when its initiating thread exits.
        pConnection->ResumeReceives();
    }
    catch (const std::exception& e) {
        LOG_ERROR_F("Connection {} {} failed: {}", pConnection->GetDirection() == EDirection::OUTBOUND ? "outbound" : "inbound", pConnection->GetSocket()->GetSocketAddress().Format(), e);
    }

    pConnection->NotifyHandshakeComplete();
}

void Connection::NotifyHandshakeComplete()
{
    std::function<void()> callback = std::move(m_handshakeCallback);
    if (callback) {
        callback();
    }
}

void Connection::ConnectOutbound()
{
    auto pAsioSocket = m_pSocket->GetAsioSocket();
    pAsioSocket->async_connect(
        m_pSocket->GetEndpoint(),
        std::bind(&Connection::HandleConnected, shared_from_this(), std::placeholders::_1)
    );

    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!m_pSocket->IsConnectFailed() && !m_pSocket->IsOpen() && std::chrono::steady_clock::now() < timeout && Global::IsRunning()) {
        ThreadUtil::SleepFor(std::chrono::milliseconds(5));
    }

    // Capture the result before cancelling: operation_aborted can arrive on
    // the I/O thread immediately after cancel() and must not mask a timeout.
    const bool failed = m_pSocket->IsConnectFailed();
    const bool connected = m_pSocket->IsOpen();
    const int connectError = m_connectError.load();
    if (failed || !connected) {
        asio::error_code ignored;
        pAsioSocket->cancel(ignored);
        if (!Global::IsRunning()) throw std::runtime_error("TCP connection cancelled during shutdown");
        if (failed) {
            throw std::runtime_error("TCP connect failed; error code=" + std::to_string(connectError));
        }
        throw std::runtime_error("TCP connect timed out after 5 seconds");
    }
}

void Connection::HandleConnected(const asio::error_code& ec)
{
    if (m_pSocket) {
        if (!ec) {
            m_pSocket->SetOpen(true);
        } else {
            m_connectError.store(ec.value());
            LOG_DEBUG_F("TCP connect callback {}: {} (code={})", m_pSocket->GetSocketAddress().Format(), ec.message(), ec.value());
            m_pSocket->SetConnectFailed(true);
        }
    }
}

void Connection::HandleReceivedHeader(const asio::error_code& ec, const size_t bytes_received)
{
    std::unique_lock<std::mutex> write_lock(m_mutex);

    if (!ec && m_pSocket->IsOpen() && bytes_received == 11) {
        assert(m_received.size() == 11);

        try {
            MessageHeader msg_header = ByteBuffer(m_received).Read<MessageHeader>();

            m_received.clear();
            m_received.resize(msg_header.GetLength());
            if (m_pSocket->IsOpen()) {
                asio::async_read(
                    *m_pSocket->GetAsioSocket(),
                    asio::buffer(m_received, m_received.size()),
                    std::bind(&Connection::HandleReceivedBody, shared_from_this(), msg_header, std::placeholders::_1, std::placeholders::_2)
                );
            }

            return;
        }
        catch (std::exception& e) {
            LOG_INFO_F("Error while receiving from {}: {}", GetPeer(), e.what());
        }
    }

    LOG_INFO_F("Disconnecting from socket {}. Error: {}", GetSocket(), ec ? ec.message() : "<none>");
    m_pSocket->CloseSocket();
    GetPeer()->SetConnected(false);
}

void Connection::HandleReceivedBody(MessageHeader msg_header, const asio::error_code& ec, const size_t bytes_received)
{
    std::unique_lock<std::mutex> write_lock(m_mutex);

    if (!ec && m_pSocket->IsOpen() && bytes_received == msg_header.GetLength()) {
        assert(m_received.size() == msg_header.GetLength());

        try {
            const auto type = msg_header.GetMessageType();
            if (type == MessageTypes::Ping || type == MessageTypes::Pong) {
                m_lastPing = system_clock::now();
            } else {
                LOG_TRACE_F("Received '{}' from {}", msg_header, GetPeer());
            }

            GetPeer()->UpdateLastContactTime();
            m_lastReceived = std::chrono::system_clock::now();
            // The async receive path is the normal path after the handshake; the
            // synchronous ReceiveSync counter is not used here. Count every fully
            // received message so ExceedsRateLimit sees actual traffic.
            m_pSocket->GetRateCounter().AddMessageReceived();

            auto pMessageProcessor = m_pMessageProcessor.lock();
            if (pMessageProcessor != nullptr) {
                RawMessage message(std::move(msg_header), std::move(m_received));
                pMessageProcessor->ProcessMessage(shared_from_this(), message);
            }

            if (!m_receivingDisabled && m_pSocket->IsOpen()) {
                m_received.clear();
                m_received.resize(11);
                asio::async_read(
                    *m_pSocket->GetAsioSocket(),
                    asio::buffer(m_received, m_received.size()),
                    std::bind(&Connection::HandleReceivedHeader, shared_from_this(), std::placeholders::_1, std::placeholders::_2)
                );
            }

            return;
        }
        catch (std::exception& e) {
            LOG_INFO_F("Error while receiving from {}: {}", GetPeer(), e.what());
        }
    }

    LOG_INFO_F("Disconnecting from socket {}. Error: {}", GetSocket(), ec ? ec.message() : "<none>");
    m_pSocket->CloseSocket();
    GetPeer()->SetConnected(false);
}

void Connection::CheckPing()
{
    std::unique_lock<std::mutex> write_lock(m_mutex);

    if (!GetSocket()->IsActive()) {
        return;
    }

    if (ExceedsRateLimit()) {
        LOG_WARNING_F("Banning {} for exceeding rate limit.", GetPeer());
        GetPeer()->Ban(EBanReason::Abusive);
        return;
    }

    if (m_lastPing + std::chrono::seconds(10) < system_clock::now()) {
        // ONE coherent header-head snapshot (height+TD from the same tip). If no
        // snapshot is readable, withhold the Ping rather than mixing two tips.
        std::optional<P2P::AdvertisedTip> tip;
        if (const auto pMessageProcessor = m_pMessageProcessor.lock()) {
            tip = pMessageProcessor->GetAdvertisedTip();
        }
        if (!tip) {
            tip = P2P::GetAdvertisedTip(*m_pSyncStatus);
        }
        if (tip) {
            SendAsync(PingMessage{ tip->TotalDifficulty, tip->Height });
        } else {
            LOG_DEBUG("Withholding Ping: no coherent header-tip snapshot available.");
        }

        m_lastPing = system_clock::now();
    }
}

bool Connection::SendSync(const IMessage& message)
{
    std::vector<uint8_t> serialized_message = message.Serialize(GetProtocolVersion());
    if (message.GetMessageType() != MessageTypes::Ping && message.GetMessageType() != MessageTypes::Pong) {
        LOG_TRACE_F(
            "Sending {}b '{}' message to {}",
            serialized_message.size(),
            MessageTypes::ToString(message.GetMessageType()),
            GetSocket()
        );
    }

    return GetSocket()->SendSync(serialized_message, true);
}

bool Connection::ReceiveSync(std::vector<uint8_t>& bytes, const size_t num_bytes)
{
    std::unique_lock<std::mutex> write_lock(m_mutex);

    bytes = m_pSocket->ReceiveSync(num_bytes, false);
    if (bytes.size() != num_bytes) {
        return false;
    }

    m_lastReceived = system_clock::now();
    GetPeer()->UpdateLastContactTime();
    return true;
}

void Connection::ResumeReceives()
{
    const auto pConnection = shared_from_this();
    asio::post(m_pSocket->GetAsioSocket()->get_executor(), [pConnection]() {
        std::unique_lock<std::mutex> lock(pConnection->m_mutex);
        if (!pConnection->m_pSocket->IsOpen() || pConnection->m_terminate) {
            return;
        }

        pConnection->m_receivingDisabled = false;
        pConnection->m_lastReceived = system_clock::now();
        pConnection->m_received.clear();
        pConnection->m_received.resize(11);
        asio::async_read(
            *pConnection->m_pSocket->GetAsioSocket(),
            asio::buffer(pConnection->m_received, pConnection->m_received.size()),
            std::bind(&Connection::HandleReceivedHeader, pConnection,
                std::placeholders::_1, std::placeholders::_2));
    });
}

void Connection::BanPeer(const EBanReason reason)
{
    LOG_WARNING_F("Banning peer {} for '{}'.", GetIPAddress(), BanReason::Format(reason));
    GetPeer()->Ban(reason);
    m_terminate = true;
}
