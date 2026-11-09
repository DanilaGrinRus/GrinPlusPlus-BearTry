#include "PeerManager.h"

#include <Core/Context.h>
#include <P2P/Common.h>
#include <Common/Util/StringUtil.h>
#include <Common/Util/TimeUtil.h>
#include <Common/Util/ThreadUtil.h>
#include <Core/Config.h>
#include <Common/Logger.h>
#include <Crypto/CSPRNG.h>

PeerManager::PeerManager(const Context::Ptr& pContext, std::shared_ptr<Locked<IPeerDB>> pPeerDB)
    : m_taskId(0), m_pContext(pContext), m_pPeerDB(pPeerDB)
{

}

PeerManager::~PeerManager()
{
    LOG_INFO("Shutting down peer manager");

    m_pContext->GetScheduler()->RemoveTask(m_taskId);
    Thread_ManagePeers(*this);
}

std::shared_ptr<Locked<PeerManager>> PeerManager::Create(const Context::Ptr& pContext, std::shared_ptr<Locked<IPeerDB>> pPeerDB)
{
    std::shared_ptr<PeerManager> pPeerManager(new PeerManager(pContext, pPeerDB));

    pPeerManager->m_peersByAddress.clear();

    if (Global::GetConfig().GetPreferredPeers().size() > 0) {
        LOG_INFO("Preferred peers found.");
        for (const IPAddress& ipAddress : Global::GetConfig().GetPreferredPeers()) {
            try {
                // Preserve a preferred peer's persisted ban and metadata.
                const auto storedPeer = pPeerDB->Read()->GetPeer(ipAddress, std::nullopt);
                const PeerPtr peer = storedPeer.has_value() ? storedPeer.value() : std::make_shared<Peer>(ipAddress);
                pPeerManager->m_peersByAddress.emplace(peer->GetIPAddress(), PeerEntry(peer));
            }
            catch (std::exception& e) {
                LOG_ERROR_F("Exception thrown: {}", e.what());
            }
        }
    } 
    else {
        LOG_INFO("Getting peers from database...");
        const std::vector<PeerPtr> peers = pPeerDB->Read()->LoadAllPeers();
        for (const PeerPtr& peer : peers) {
            pPeerManager->m_peersByAddress.emplace(peer->GetIPAddress(), PeerEntry(peer));
        }
    }
    
    std::shared_ptr<Locked<PeerManager>> pLocked = std::make_shared<Locked<PeerManager>>(Locked<PeerManager>(pPeerManager));

    std::weak_ptr<Locked<PeerManager>> pLockedWeak(pLocked);
    const uint64_t taskId = pContext->GetScheduler()->interval(std::chrono::seconds(15), [pLockedWeak]() {
        auto pLocked = pLockedWeak.lock();
        if (pLocked != nullptr) {
            auto pWriter = pLocked->Write();
            PeerManager::Thread_ManagePeers(*pWriter.GetShared());
        }
    });

    pPeerManager->SetTaskId(taskId);

    return pLocked;
}

void PeerManager::Thread_ManagePeers(PeerManager& peerManager)
{
    LoggerAPI::SetThreadName("PEER_MANAGER");
    LOG_TRACE("BEGIN");

    try {
        std::vector<PeerPtr> peersToUpdate;
        std::vector<PeerPtr> peersToDelete;

        const time_t minimumContactTime = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now() - std::chrono::hours(24 * 7)
        );

        for (auto& iter : peerManager.m_peersByAddress)
        {
            try {
                PeerEntry& peerEntry = iter.second;
                if (peerEntry.m_peer->IsDirty())
                {
                    peersToUpdate.push_back(peerEntry.m_peer);
                    peerEntry.m_peer->SetDirty(false);
                } else if (peerEntry.m_peer->GetLastContactTime() > 0 && peerEntry.m_peer->GetLastContactTime() < minimumContactTime)
                {
                    peersToDelete.push_back(peerEntry.m_peer);
                }
            } 
            catch (std::exception& e) {
                LOG_ERROR_F("Exception thrown: {}", e.what());
            }
        }

        for (PeerPtr pPeer : peersToDelete)
        {
            peerManager.m_peersByAddress.erase(pPeer->GetIPAddress());
        }

        peerManager.m_pPeerDB->Write()->SavePeers(peersToUpdate);
        peerManager.m_pPeerDB->Write()->DeletePeers(peersToDelete);
    }
    catch (std::exception& e) {
        LOG_ERROR_F("Exception thrown: {}", e.what());
    }

    LOG_TRACE("END");
}

PeerPtr PeerManager::GetPeer(const IPAddress& address)
{
    auto iter = m_peersByAddress.find(address);
    if (iter != m_peersByAddress.cend()) {
        return iter->second.m_peer;
    }

    PeerPtr pPeer = std::make_shared<Peer>(address);
    m_peersByAddress.emplace(address, PeerEntry(pPeer));
    return pPeer;
}

std::optional<PeerConstPtr> PeerManager::GetPeer(const IPAddress& address) const
{
    auto iter = m_peersByAddress.find(address);
    if (iter != m_peersByAddress.cend()) {
        return std::make_optional(iter->second.m_peer);
    }

    return m_pPeerDB->Read()->GetPeer(address, std::nullopt);
}

PeerPtr PeerManager::GetNewPeer(const Capabilities::ECapability& preferredCapability)
{
    std::vector<PeerPtr> peers = GetPeersWithCapability(preferredCapability, 1, true);
    if (peers.empty()) {
        peers = GetPeersWithCapability(Capabilities::UNKNOWN, 1, true);
    }

    if (peers.empty()) {
        return nullptr;
    }

    return peers.front();
}

std::string PeerManager::GetNoPeerReason(const Capabilities::ECapability& preferredCapability) const
{
    const time_t currentTime = TimeUtil::Now();
    const time_t maxBanTime = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now() - std::chrono::seconds(P2P::BAN_WINDOW)
    );

    size_t banned = 0;
    size_t retryWindow = 0;
    size_t withoutCapability = 0;
    size_t connected = 0;

    for (const auto& entry : m_peersByAddress) {
        const PeerPtr& peer = entry.second.m_peer;
        if (peer->GetLastBanTime() > maxBanTime) {
            banned++;
        }
        if (peer->IsConnected()) {
            connected++;
        }
        if (!peer->GetCapabilities().HasCapability(preferredCapability)) {
            withoutCapability++;
        }
        if (std::difftime(currentTime, entry.second.m_lastAttempt) <= (double)P2P::RETRY_WINDOW) {
            retryWindow++;
        }
    }

    return StringUtil::Format(
        "known={}, banned={}, no_capability=0x{:x}:{}, retry_window_all={}s:{}, connected={}",
        m_peersByAddress.size(),
        banned,
        (uint32_t)preferredCapability,
        withoutCapability,
        P2P::RETRY_WINDOW,
        retryWindow,
        connected);
}

std::vector<PeerPtr> PeerManager::GetAllPeers()
{
    std::vector<PeerPtr> peers;
    for (auto iter = m_peersByAddress.begin(); iter != m_peersByAddress.end(); iter++) {
        PeerPtr peer = iter->second.m_peer;
        if (peer->GetLastContactTime() > 0 || peer->IsBanned()) {
            peers.push_back(peer);
        }
    }

    return peers;
}

std::vector<PeerConstPtr> PeerManager::GetAllPeers() const
{
    std::vector<PeerConstPtr> peers;
    for (auto iter = m_peersByAddress.begin(); iter != m_peersByAddress.end(); iter++) {
        PeerConstPtr peer = iter->second.m_peer;
        if (peer->GetLastContactTime() > 0 || peer->IsBanned()) {
            peers.push_back(peer);
        }
    }

    return peers;
}

std::vector<PeerPtr> PeerManager::GetPeers(
    const Capabilities::ECapability& preferredCapability,
    const uint16_t maxPeers) const
{
    std::vector<PeerPtr> peers = GetPeersWithCapability(preferredCapability, maxPeers, false);
    if (peers.empty()) {
        peers = GetPeersWithCapability(Capabilities::UNKNOWN, maxPeers, false);
    }

    return peers;
}

void PeerManager::AddFreshPeers(const std::vector<SocketAddress>& peerAddresses)
{
    for (auto& socketAddress : peerAddresses) {
        const IPAddress& ipAddress = socketAddress.GetIPAddress();
        if (m_peersByAddress.find(ipAddress) == m_peersByAddress.end()) {
            m_peersByAddress.emplace(ipAddress, PeerEntry(std::make_shared<Peer>(ipAddress)));
        }
    }
}

void PeerManager::BanPeer(const IPAddress& address, const EBanReason banReason)
{
    auto iter = m_peersByAddress.find(address);
    PeerPtr current;
    if (iter != m_peersByAddress.end()) {
        current = iter->second.m_peer;
    } else {
        const auto stored = m_pPeerDB->Read()->GetPeer(address, std::nullopt);
        current = stored.has_value() ? stored.value() : std::make_shared<Peer>(address);
    }
    const auto persisted = std::make_shared<Peer>(address);
    *persisted = *current;
    persisted->Ban(banReason);
    m_pPeerDB->Write()->SavePeers({ persisted });
    if (iter != m_peersByAddress.end()) current->Ban(banReason);
    else m_peersByAddress.emplace(address, PeerEntry(persisted, TimeUtil::Now()));
}

void PeerManager::UnbanAllPeers()
{
    // Include records not loaded into memory (e.g. with preferred peers set).
    // In-memory state is authoritative for peers modified since the last save.
    std::map<IPAddress, PeerPtr> peers;
    for (const PeerPtr& peer : m_pPeerDB->Read()->LoadAllPeers()) {
        peers[peer->GetIPAddress()] = peer;
    }
    for (const auto& entry : m_peersByAddress) {
        peers[entry.first] = entry.second.m_peer;
    }

    std::vector<PeerPtr> copies;
    std::vector<PeerPtr> live;
    for (const auto& entry : peers) {
        const PeerPtr& peer = entry.second;
        if (peer->GetLastBanTime() == 0 && peer->GetBanReason() == EBanReason::None) continue;
        const auto copy = std::make_shared<Peer>(entry.first);
        *copy = *peer;
        copy->Unban();
        copies.push_back(copy);
        const auto iter = m_peersByAddress.find(entry.first);
        if (iter != m_peersByAddress.end()) live.push_back(iter->second.m_peer);
    }
    if (copies.empty()) return;
    // One DB batch first; failure leaves every live peer unchanged.
    m_pPeerDB->Write()->SavePeers(copies);
    for (const PeerPtr& peer : live) peer->Unban();
}

void PeerManager::UnbanPeer(const IPAddress& address)
{
    const auto iter = m_peersByAddress.find(address);
    if (iter != m_peersByAddress.end()) {
        const PeerPtr peer = iter->second.m_peer;
        // Persist a copy first, so a storage error cannot leave the in-memory
        // peer unbanned while the API reports failure.
        const PeerPtr unbanned = std::make_shared<Peer>(address);
        *unbanned = *peer;
        unbanned->Unban();
        m_pPeerDB->Write()->SavePeers({ unbanned });
        peer->Unban();
        return;
    }

    // Preferred-peer mode does not load every database entry into memory.
    // Still honour the administrator's request for a stored-only peer.
    const auto storedPeer = m_pPeerDB->Read()->GetPeer(address, std::nullopt);
    if (storedPeer.has_value()) {
        const PeerPtr peer = storedPeer.value();
        peer->Unban();
        m_pPeerDB->Write()->SavePeers({ peer });
    }
}

std::vector<PeerPtr> PeerManager::GetPeersWithCapability(
    const Capabilities::ECapability& preferredCapability,
    const uint16_t maxPeers,
    const bool connectingToPeer) const
{
    const size_t numPeers = m_peersByAddress.size();
    if (numPeers == 0) {
        return {};
    }

    std::vector<PeerPtr> peersFound;
    const time_t currentTime = TimeUtil::Now();
    const time_t maxBanTime = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now() - std::chrono::seconds(P2P::BAN_WINDOW)
    );

    auto iter = m_peersByAddress.begin();
    std::advance(iter, CSPRNG::GenerateRandom(0, numPeers));

    for (size_t i = 0; i < numPeers; i++) {
        if (iter == m_peersByAddress.end()) {
            iter = m_peersByAddress.begin();
        }

        PeerEntry& peerEntry = (iter++)->second;
        const PeerPtr& peer = peerEntry.m_peer;

        if (connectingToPeer && peer->GetLastBanTime() > maxBanTime) {
            continue;
        }

        if (peer->GetCapabilities().HasCapability(preferredCapability)) {
            if (!connectingToPeer) {
                peersFound.push_back(peer);
            } else if (!peer->IsConnected() && std::difftime(currentTime, peerEntry.m_lastAttempt) > P2P::RETRY_WINDOW) {
                peerEntry.m_lastAttempt = currentTime;
                peersFound.push_back(peer);
            }

            if (peersFound.size() == maxPeers) {
                return peersFound;
            }
        }

    }

    return peersFound;
}
