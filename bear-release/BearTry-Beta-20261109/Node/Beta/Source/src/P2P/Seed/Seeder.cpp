#include "Seeder.h"
#include <P2P/Seed/SeederPolicy.h>
#include "DNSSeeder.h"
#include "PeerManager.h"
#include "../ConnectionManager.h"
#include "../Messages/GetPeerAddressesMessage.h"
#include "../PIBD/PibdArchiveServer.h"

#include <Core/Context.h>
#include <Core/Global.h>
#include <P2P/Capabilities.h>
#include <Net/SocketAddress.h>
#include <Net/Socket.h>
#include <Common/Util/StringUtil.h>
#include <Common/Util/ThreadUtil.h>
#include <Common/Logger.h>
#include <functional>
#include <algorithm>

Seeder::~Seeder()
{
    LOG_INFO("Shutting down seeder");

    // The archive worker only reads the chain; stop it before the connections
    // and the chain teardown. Shutdown is idempotent and safe when disabled.
    PibdArchiveServer::Shutdown();

    m_terminate = true;

    m_pAsioContext->stop();

    std::unique_lock<std::mutex> lock(m_mutex);
    ThreadUtil::Join(m_seedThread);
    ThreadUtil::Join(m_asioThread);
}

std::unique_ptr<Seeder> Seeder::Create(
    ConnectionManager& connectionManager,
    Locked<PeerManager> peerManager,
    const IBlockChain::Ptr& pBlockChain,
    std::shared_ptr<Pipeline> pPipeline,
    SyncStatusConstPtr pSyncStatus,
    std::shared_ptr<PibdSync> pPibdSync,
    std::shared_ptr<PihdClient> pPihdClient)
{
    // Own the archive serving/capability state for the lifetime of the P2P
    // server. Disabled by default; an isolated test may opt in for this process
    // only via GRINPP_PIBD_ARCHIVE_SERVING=1 (see
    // docs/modernization/G09_ARCHIVE_SERVING.md). Never opt in on mainnet.
    bool pibdArchiveServing = PibdArchiveServer::ResolveServingOptIn(
        PibdArchiveServer::ENABLED_BY_DEFAULT);
    if (pibdArchiveServing && Global::IsMainnet()) {
        LOG_ERROR("PIBD archive serving refused on MAINNET; staying disabled.");
        pibdArchiveServing = false;
    }
    LOG_INFO_F(
        "PIBD archive serving: {} ({})",
        pibdArchiveServing ? "ENABLED" : "DISABLED",
        pibdArchiveServing ? "opt-in GRINPP_PIBD_ARCHIVE_SERVING" : "default");
    PibdArchiveServer::Initialize(pBlockChain, pibdArchiveServing);

    auto pMessageProcessor = std::make_shared<MessageProcessor>(
        connectionManager,
        peerManager,
        pBlockChain,
        pPipeline,
        pSyncStatus,
        pPibdSync,
        pPihdClient
    );
    std::unique_ptr<Seeder> pSeeder(new Seeder(
        connectionManager,
        peerManager,
        pBlockChain,
        pMessageProcessor,
        pSyncStatus
    ));

    pSeeder->m_asioThread = std::thread(Thread_AsioContext, std::ref(*pSeeder.get()));
    pSeeder->m_seedThread = std::thread(Thread_Seed, std::ref(*pSeeder.get()));

    return pSeeder;
}

void Seeder::Thread_AsioContext(Seeder& seeder)
{
    LoggerAPI::SetThreadName("ASIO");

    seeder.StartListener();

    while (!seeder.m_terminate) {
        asio::error_code ec;
        seeder.m_pAsioContext->run(ec);
    }
}

//
// Continuously checks the number of connected peers, and connects to additional peers when the number of connections drops below the minimum.
// This function operates in its own thread.
//
void Seeder::Thread_Seed(Seeder& seeder)
{
    LoggerAPI::SetThreadName("SEED");
    LOG_TRACE("BEGIN");

    auto lastConnectTime = std::chrono::system_clock::now() - std::chrono::seconds(5);
    auto lastNoPeerLog = std::chrono::system_clock::now() - std::chrono::seconds(60);
    uint32_t seedBackoffSeconds = 5;

    const size_t minimumConnections = (size_t)(std::max)(0, Global::GetConfig().GetMinPeers());
    const size_t maximumConnections = (size_t)(std::max)(0, Global::GetConfig().GetMaxPeers());
    const size_t targetConnections = SeederPolicy::TargetOutbound(minimumConnections, maximumConnections);
    LOG_INFO_F("Peer target: {} outbound (MIN_PEERS={}, MAX_PEERS={})", targetConnections, minimumConnections, maximumConnections);
    while (!seeder.m_terminate && Global::IsRunning()) {
        try {
            seeder.m_connectionManager.PruneConnections(true);

            auto now = std::chrono::system_clock::now();
            const size_t numOutbound = seeder.m_connectionManager.GetNumOutbound();
            if (numOutbound < targetConnections &&
                lastConnectTime + std::chrono::seconds(seedBackoffSeconds) < now) {
                lastConnectTime = now;

                const size_t connectionsToAdd = SeederPolicy::ConnectionsToAdd(numOutbound, targetConnections, 15);
                bool attempted = false;
                for (size_t i = 0; i < connectionsToAdd; i++) {
                    attempted = seeder.SeedNewConnection() || attempted;
                }

                if (attempted) {
                    seedBackoffSeconds = 5;
                } else {
                    seedBackoffSeconds = (std::min)(seedBackoffSeconds * 2, (uint32_t)60);
                    if (lastNoPeerLog + std::chrono::seconds(60) < now) {
                        lastNoPeerLog = now;
                        LOG_WARNING_F(
                            "No peer candidates: {}",
                            seeder.m_peerManager.Read()->GetNoPeerReason(Capabilities::FAST_SYNC_NODE));
                    }
                }
            }
        }
        catch (std::exception& e) {
            LOG_WARNING_F("Exception thrown: {}", e.what());
        }

        ThreadUtil::SleepFor(std::chrono::milliseconds(100));
    }

    LOG_TRACE("END");
}

void Seeder::StartListener()
{
    try {
        m_pAcceptor = std::make_shared<asio::ip::tcp::acceptor>(
            *m_pAsioContext,
            asio::ip::tcp::endpoint(asio::ip::tcp::v4(), Global::GetConfig().GetP2PPort())
        );

        m_pSocket = std::make_shared<asio::ip::tcp::socket>(*m_pAsioContext);
        m_pAcceptor->async_accept(*m_pSocket, std::bind(&Seeder::Accept, this, std::placeholders::_1));
    }
    catch (std::exception& e) {
        LOG_ERROR_F("Listener failed with error: {}", e.what());
    }
}

void Seeder::Accept(const asio::error_code& ec)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (!ec && m_connectionManager.GetNumberOfActiveConnections() < Global::GetConfig().GetMaxPeers()) {
        SocketPtr pSocket(new Socket(SocketAddress::FromEndpoint(m_pSocket->remote_endpoint()), m_pAsioContext, m_pSocket));
        pSocket->SetOpen(true);

        if (Global::GetConfig().IsPeerBlocked(pSocket->GetIPAddress())) {
            LOG_TRACE_F("peer is blocked: {}",  pSocket->GetIPAddress());
        } else if (!Global::GetConfig().IsPeerAllowed(pSocket->GetIPAddress())) {
            LOG_TRACE_F("peer is not allowed: {}",  pSocket->GetIPAddress());
        } else {
            PeerPtr pPeer = m_peerManager.Write()->GetPeer(pSocket->GetIPAddress());
            if (!pPeer->IsBanned()) {
                auto pConnection = std::make_shared<Connection>(
                    pSocket,
                    m_nextId++,
                    m_connectionManager,
                    ConnectedPeer(pPeer, EDirection::INBOUND, pSocket->GetPort()),
                    m_pSyncStatus,
                    m_pMessageProcessor
                );
                pConnection->Connect();
            }
        }
    } else {
        asio::error_code ignoreError;
        m_pSocket->close(ignoreError);
    }

    // Re-arm the listener on every path. A rejected or failed inbound peer must
    // never stop the accept loop permanently.
    m_pSocket = std::make_shared<asio::ip::tcp::socket>(*m_pAsioContext);
    m_pAcceptor->async_accept(*m_pSocket, std::bind(&Seeder::Accept, this, std::placeholders::_1));
}

bool Seeder::SeedNewConnection()
{
    PeerPtr pPeer = m_peerManager.Write()->GetNewPeer(Capabilities::FAST_SYNC_NODE);
    if (pPeer != nullptr) {
        LOG_TRACE_F("Attempting to seed: {}", pPeer);

        ConnectedPeer connectedPeer(
            pPeer,
            EDirection::OUTBOUND,
            Global::GetConfig().GetPeerPort()
        );
        SocketPtr pSocket(new Socket(
            connectedPeer.GetSocketAddress(),
            m_pAsioContext,
            std::make_shared<asio::ip::tcp::socket>(*m_pAsioContext)
        ));
        
        const auto& blockedOutbound = Global::GetConfig().GetBlockedOutboundPeers();
        if (std::find(blockedOutbound.begin(), blockedOutbound.end(), pSocket->GetIPAddress()) != blockedOutbound.end()) {
            LOG_TRACE_F("peer is blocked for outbound: {}", pSocket->GetIPAddress());
            return false;
        }

        if (Global::GetConfig().IsPeerBlocked(pSocket->GetIPAddress())) {
            LOG_TRACE_F("peer is blocked: {}", pSocket->GetIPAddress());
            return false;
        }

        if (!Global::GetConfig().IsPeerAllowed(pSocket->GetIPAddress())) {
            LOG_TRACE_F("peer is not allowed: {}", pSocket->GetIPAddress());        
            return false;
        }

        if (!Global::GetConfig().IsPeerPreferred(pSocket->GetIPAddress())) {
            LOG_TRACE_F("peer is not in preferred pool of peers: {}", pSocket->GetIPAddress());        
            return false;
        }
        
        ConnectionPtr pConnection = std::make_shared<Connection>(
            pSocket,
            m_nextId++,
            m_connectionManager,
            connectedPeer,
            m_pSyncStatus,
            m_pMessageProcessor
        );
        
        pConnection->Connect();
        return true;
    } else {
        const auto now = std::chrono::system_clock::now();
        if (m_lastDnsAttempt + std::chrono::minutes(5) <= now) {
            m_lastDnsAttempt = now;
            std::vector<SocketAddress> peerAddresses = DNSSeeder::GetPeersFromDNS();
            LOG_INFO_F("DNS discovery returned {} peer(s)", peerAddresses.size());
            m_peerManager.Write()->AddFreshPeers(peerAddresses);
        }
    }

    return false;
}