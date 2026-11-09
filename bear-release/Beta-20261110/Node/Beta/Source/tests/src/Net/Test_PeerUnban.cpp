#include <catch.hpp>
#include <TestServer.h>
#include <Core/Context.h>
#include <Core/Global.h>
#include <Database/Database.h>
#include <Database/PeerDB.h>
#include <P2P/Seed/PeerManager.h>
#include <stdexcept>

namespace {
class RestorePreferredPeers
{
public:
    RestorePreferredPeers() : m_original(Global::GetConfig().GetPreferredPeers()) {}
    ~RestorePreferredPeers() { Global::GetConfig().UpdatePreferredPeers(m_original); }
private:
    std::unordered_set<IPAddress> m_original;
};
}

TEST_CASE("Unban persists preferred and database-only peers across restart")
{
    RestorePreferredPeers restore;
    const IPAddress preferred = IPAddress::Parse("192.0.2.11");
    const IPAddress storedOnly = IPAddress::Parse("192.0.2.12");
    Global::GetConfig().UpdatePreferredPeers({ preferred });
    {
        auto server = TestServer::Create();
        server->SetPreserveDataOnDestruct(true);
        auto db = server->GetDatabase()->GetPeerDB();
        const auto peerA = std::make_shared<Peer>(preferred);
        const auto peerB = std::make_shared<Peer>(storedOnly);
        peerA->Ban(EBanReason::Abusive);
        peerB->Ban(EBanReason::Abusive);
        db->Write()->SavePeers({ peerA, peerB });
        const auto context = Global::GetContext();
        auto manager = PeerManager::Create(context, db);
        const auto loadedPreferred = manager->Read()->GetPeer(preferred);
        REQUIRE(loadedPreferred.has_value());
        REQUIRE(loadedPreferred.value()->GetLastBanTime() > 0);
        REQUIRE(loadedPreferred.value()->GetBanReason() == EBanReason::Abusive);

        // The second address is deliberately only in RocksDB, not the preferred map.
        manager->Write()->UnbanPeer(storedOnly);
        const auto stored = db->Read()->GetPeer(storedOnly, std::nullopt);
        REQUIRE(stored.has_value());
        REQUIRE(stored.value()->GetLastBanTime() == 0);
        REQUIRE(stored.value()->GetBanReason() == EBanReason::None);
        manager->Write()->UnbanPeer(preferred);
        const auto persistedPreferred = db->Read()->GetPeer(preferred, std::nullopt);
        REQUIRE(persistedPreferred.has_value());
        REQUIRE(persistedPreferred.value()->GetLastBanTime() == 0);
        REQUIRE(persistedPreferred.value()->GetBanReason() == EBanReason::None);
        manager.reset();
    }
    {
        auto reopened = TestServer::OpenExisting();
        const auto db = reopened->GetDatabase()->GetPeerDB();
        for (const auto& address : { preferred, storedOnly }) {
            const auto peer = db->Read()->GetPeer(address, std::nullopt);
            REQUIRE(peer.has_value());
            REQUIRE(peer.value()->GetLastBanTime() == 0);
            REQUIRE(peer.value()->GetBanReason() == EBanReason::None);
        }
    }
}

namespace {
class FailingUnbanPeerDB final : public IPeerDB
{
public:
    explicit FailingUnbanPeerDB(PeerPtr peer) : m_peer(std::move(peer)) {}
    bool failWrites = false;
    std::vector<PeerPtr> LoadAllPeers() const final { return { m_peer }; }
    std::optional<PeerPtr> GetPeer(const IPAddress& address, const std::optional<uint16_t>&) const final
    {
        return address == m_peer->GetIPAddress() ? std::optional<PeerPtr>(m_peer) : std::nullopt;
    }
    void SavePeers(const std::vector<PeerPtr>& peers) final
    {
        if (failWrites) throw std::runtime_error("injected peer DB write failure");
        if (!peers.empty()) m_peer = peers.front();
    }
    void DeletePeers(const std::vector<PeerPtr>&) final {}
    void Commit() final {}
    void Rollback() noexcept final {}
private:
    PeerPtr m_peer;
};
}

TEST_CASE("Unban leaves an active ban intact when persistence fails")
{
    RestorePreferredPeers restore;
    const IPAddress address = IPAddress::Parse("192.0.2.21");
    Global::GetConfig().UpdatePreferredPeers({ address });
    const auto peer = std::make_shared<Peer>(address);
    peer->Ban(EBanReason::Abusive);
    const auto fake = std::make_shared<FailingUnbanPeerDB>(peer);
    const auto db = std::make_shared<Locked<IPeerDB>>(fake);
    const auto context = Global::GetContext();
    auto manager = PeerManager::Create(context, db);
    fake->failWrites = true;
    REQUIRE_THROWS(manager->Write()->UnbanPeer(address));
    const auto after = manager->Read()->GetPeer(address);
    REQUIRE(after.has_value());
    REQUIRE(after.value()->GetLastBanTime() > 0);
    REQUIRE(after.value()->GetBanReason() == EBanReason::Abusive);
    fake->failWrites = false;
}

TEST_CASE("Unban all persists loaded, stored-only and never-contacted bans")
{
    RestorePreferredPeers restore;
    const IPAddress preferred = IPAddress::Parse("192.0.2.31");
    const IPAddress storedOnly = IPAddress::Parse("192.0.2.32");
    const IPAddress fresh = IPAddress::Parse("192.0.2.33");
    Global::GetConfig().UpdatePreferredPeers({ preferred });
    {
        auto server = TestServer::Create();
        server->SetPreserveDataOnDestruct(true);
        auto db = server->GetDatabase()->GetPeerDB();
        auto a = std::make_shared<Peer>(preferred);
        auto b = std::make_shared<Peer>(storedOnly);
        a->Ban(EBanReason::Abusive);
        b->Ban(EBanReason::Abusive);
        db->Write()->SavePeers({ a, b });
        auto manager = PeerManager::Create(Global::GetContext(), db);
        manager->Write()->BanPeer(fresh, EBanReason::ManualBan);
        const auto listed = manager->Read()->GetAllPeers();
        bool found = false;
        for (const auto& peer : listed) if (peer->GetIPAddress() == fresh) found = true;
        REQUIRE(found);
        manager->Write()->UnbanAllPeers();
        for (const auto& address : { preferred, storedOnly, fresh }) {
            const auto peer = db->Read()->GetPeer(address, std::nullopt);
            REQUIRE(peer.has_value());
            REQUIRE(peer.value()->GetLastBanTime() == 0);
            REQUIRE(peer.value()->GetBanReason() == EBanReason::None);
        }
        manager.reset();
    }
    {
        auto reopened = TestServer::OpenExisting();
        for (const auto& address : { preferred, storedOnly, fresh }) {
            const auto peer = reopened->GetDatabase()->GetPeerDB()->Read()->GetPeer(address, std::nullopt);
            REQUIRE(peer.has_value());
            REQUIRE(peer.value()->GetLastBanTime() == 0);
            REQUIRE(peer.value()->GetBanReason() == EBanReason::None);
        }
    }
}

TEST_CASE("Unban all preserves live bans on database failure")
{
    RestorePreferredPeers restore;
    const IPAddress address = IPAddress::Parse("192.0.2.34");
    Global::GetConfig().UpdatePreferredPeers({ address });
    const auto peer = std::make_shared<Peer>(address);
    peer->Ban(EBanReason::Abusive);
    const auto fake = std::make_shared<FailingUnbanPeerDB>(peer);
    const auto db = std::make_shared<Locked<IPeerDB>>(fake);
    auto manager = PeerManager::Create(Global::GetContext(), db);
    fake->failWrites = true;
    REQUIRE_THROWS(manager->Write()->UnbanAllPeers());
    const auto after = manager->Read()->GetPeer(address);
    REQUIRE(after.has_value());
    REQUIRE(after.value()->GetLastBanTime() > 0);
    REQUIRE(after.value()->GetBanReason() == EBanReason::Abusive);
    fake->failWrites = false;
}

TEST_CASE("Manual ban is durable and remains unchanged on storage failure")
{
    RestorePreferredPeers restore;
    const IPAddress address = IPAddress::Parse("192.0.2.35");
    Global::GetConfig().UpdatePreferredPeers({ address });
    const auto peer = std::make_shared<Peer>(address);
    const auto fake = std::make_shared<FailingUnbanPeerDB>(peer);
    const auto db = std::make_shared<Locked<IPeerDB>>(fake);
    auto manager = PeerManager::Create(Global::GetContext(), db);
    fake->failWrites = true;
    REQUIRE_THROWS(manager->Write()->BanPeer(address, EBanReason::ManualBan));
    REQUIRE(manager->Read()->GetPeer(address).value()->GetLastBanTime() == 0);
    fake->failWrites = false;
    manager->Write()->BanPeer(address, EBanReason::ManualBan);
    REQUIRE(db->Read()->GetPeer(address, std::nullopt).value()->GetLastBanTime() > 0);
    REQUIRE(manager->Read()->GetPeer(address).value()->GetBanReason() == EBanReason::ManualBan);
}

TEST_CASE("Peer retry diagnostics include peers with unknown capabilities")
{
    RestorePreferredPeers restore;
    const IPAddress address = IPAddress::Parse("192.0.2.199");
    Global::GetConfig().UpdatePreferredPeers({ address });
    auto server = TestServer::Create();
    auto manager = PeerManager::Create(Global::GetContext(), server->GetDatabase()->GetPeerDB());
    const auto selected = manager->Write()->GetNewPeer(Capabilities::FAST_SYNC_NODE);
    REQUIRE(selected != nullptr);
    const std::string reason = manager->Read()->GetNoPeerReason(Capabilities::FAST_SYNC_NODE);
    REQUIRE(reason.find("no_capability=0x6:1") != std::string::npos);
    REQUIRE(reason.find("retry_window_all=300s:1") != std::string::npos);
}