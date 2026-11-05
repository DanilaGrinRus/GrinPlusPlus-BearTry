#include <catch.hpp>
#include <TestServer.h>
#include <TestMiner.h>
#include <BlockChain/BlockChain.h>
#include <Core/Global.h>
#include <Database/Database.h>
#include <Database/BlockDb.h>
#include <PMMR/KernelIndex.h>
#include <PMMR/TxHashSet.h>
#include <Wallet/Keychain/KeyChain.h>
#include <chrono>
#include <cstdlib>
#include <filesystem.h>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
std::string CrashEnv(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    size_t size = 0;
    _dupenv_s(&value, &size, name);
    const std::string result = value ? value : "";
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

// Test-only proxy: real writes go to the caller's RocksDB transaction. The
// external driver kills this process while it is held after a selected write.
class PausingBlockDB final : public IBlockDB
{
public:
    PausingBlockDB(std::shared_ptr<IBlockDB> db, std::string marker, std::string point, std::string metadata, std::string excesses)
        : m_db(std::move(db)), m_marker(std::move(marker)), m_point(std::move(point)), m_metadata(std::move(metadata)), m_excesses(std::move(excesses)) {}
    uint8_t GetVersion() const final { throw std::logic_error("unexpected GetVersion"); }
    void SetVersion(const uint8_t) final { throw std::logic_error("unexpected SetVersion"); }
    void MigrateBlocks() final { throw std::logic_error("unexpected MigrateBlocks"); }
    void Compact(const std::shared_ptr<const Chain>&) final { throw std::logic_error("unexpected Compact"); }
    BlockHeaderPtr GetBlockHeader(const Hash&) const final { throw std::logic_error("unexpected GetBlockHeader"); }
    void AddBlockHeader(BlockHeaderPtr) final { throw std::logic_error("unexpected AddBlockHeader"); }
    void AddBlockHeaders(const std::vector<BlockHeaderPtr>&) final { throw std::logic_error("unexpected AddBlockHeaders"); }
    void AddBlock(const FullBlock&) final { throw std::logic_error("unexpected AddBlock"); }
    std::unique_ptr<FullBlock> GetBlock(const Hash&) const final { throw std::logic_error("unexpected GetBlock"); }
    void ClearBlocks() final { throw std::logic_error("unexpected ClearBlocks"); }
    void AddBlockSums(const Hash&, const BlockSums&) final { throw std::logic_error("unexpected AddBlockSums"); }
    std::unique_ptr<BlockSums> GetBlockSums(const Hash&) const final { throw std::logic_error("unexpected GetBlockSums"); }
    void ClearBlockSums() final { throw std::logic_error("unexpected ClearBlockSums"); }
    void AddOutputPosition(const Commitment&, const OutputLocation&) final { throw std::logic_error("unexpected AddOutputPosition"); }
    std::unique_ptr<OutputLocation> GetOutputPosition(const Commitment&) const final { throw std::logic_error("unexpected GetOutputPosition"); }
    void RemoveOutputPositions(const std::vector<Commitment>&) final { throw std::logic_error("unexpected RemoveOutputPositions"); }
    void ClearOutputPositions() final { throw std::logic_error("unexpected ClearOutputPositions"); }
    void AddSpentPositions(const Hash&, const std::vector<SpentOutput>&) final { throw std::logic_error("unexpected AddSpentPositions"); }
    std::unordered_map<Commitment, OutputLocation> GetSpentPositions(const Hash&) const final { throw std::logic_error("unexpected GetSpentPositions"); }
    void ClearSpentPositions() final { throw std::logic_error("unexpected ClearSpentPositions"); }
    std::unique_ptr<KernelIndexState> GetKernelIndexState() const final { return m_db->GetKernelIndexState(); }
    void SetKernelIndexState(const KernelIndexState& state) final { m_db->SetKernelIndexState(state); Pause("state"); }
    void AddKernelPosition(uint64_t g, const Commitment& c, const OutputLocation& l) final { m_db->AddKernelPosition(g, c, l); ++m_writes; Pause("entry"); }
    std::unique_ptr<OutputLocation> GetKernelPosition(uint64_t g, const Commitment& c) const final { return m_db->GetKernelPosition(g, c); }
    void RemoveKernelPosition(uint64_t g, const Commitment& c, const OutputLocation& l) final { m_db->RemoveKernelPosition(g, c, l); }
    void ClearKernelPositions(uint64_t g) final { m_db->ClearKernelPositions(g); }
    void ClearAllKernelPositions() final { m_db->ClearAllKernelPositions(); }
    uint64_t GetKernelPositionCount(uint64_t g) const final { return m_db->GetKernelPositionCount(g); }
    Hash GetKernelPositionChecksum(uint64_t g) const final { return m_db->GetKernelPositionChecksum(g); }
    std::pair<uint64_t, Hash> GetKernelPositionDigest(uint64_t g) const final { return m_db->GetKernelPositionDigest(g); }
    void Commit() final { m_db->Commit(); }
    void Rollback() noexcept final { m_db->Rollback(); }
private:
    void Pause(const char* point)
    {
        if (m_point != point) return;
        if (m_writes == 0) throw std::logic_error("marker reached without staged entries");
        std::ofstream out(m_marker, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open marker");
        out << m_metadata << point << '\n' << m_writes << '\n' << m_excesses;
        out.close();
        if (!out) throw std::runtime_error("cannot flush marker");
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::shared_ptr<IBlockDB> m_db;
    std::string m_marker, m_point, m_metadata, m_excesses;
    uint64_t m_writes = 0;
};

void RestoreCrashData(const fs::path& backup)
{
    const fs::path data = Global::GetConfig().GetDataDirectory();
    REQUIRE(fs::exists(backup));
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(backup, ec)) {
        REQUIRE_FALSE(ec);
        const fs::path relative = fs::relative(entry.path(), backup, ec);
        REQUIRE_FALSE(ec);
        if (relative.begin() != relative.end() && *relative.begin() == fs::path("LOGS")) continue;
        const fs::path destination = data / relative;
        if (entry.is_directory()) fs::create_directories(destination, ec);
        else {
            fs::create_directories(destination.parent_path(), ec);
            REQUIRE_FALSE(ec);
            fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing, ec);
        }
        INFO("restore " << destination.string() << ": " << ec.message());
        REQUIRE_FALSE(ec);
    }
}
}

TEST_CASE("KernelIndexCrashChild", "[.]")
{
    const std::string marker = CrashEnv("GPLUS_KERNEL_CRASH_MARKER");
    const std::string point = CrashEnv("GPLUS_KERNEL_CRASH_POINT");
    REQUIRE_FALSE(marker.empty());
    REQUIRE((point == "entry" || point == "state"));
    TestServer::Ptr server = TestServer::Create();
    server->SetPreserveDataOnDestruct(true);
    const auto chain = server->GetBlockChain();
    const auto db = server->GetBlockDB();
    REQUIRE(chain->EnsureKernelIndex());
    const auto old = db->Read()->GetKernelIndexState();
    REQUIRE(old != nullptr);
    TestMiner miner(server);
    KeyChain keyChain = KeyChain::FromRandom();
    const auto blocks = miner.MineChain(keyChain, 8);
    REQUIRE(blocks.size() == 8);

    // Normal block processing now advances the index in the block transaction.
    // Rewind the published state to the pre-mining ancestor so this test exercises
    // the verified startup extension path (and can pause it at a write point).
    {
        const auto afterMining = db->Read()->GetKernelIndexState();
        REQUIRE(afterMining != nullptr);
        auto writer = db->BatchWrite();
        const auto removed = writer->RemoveKernelPositionsFrom(
            old->GetGeneration(), old->GetKernelCount());
        REQUIRE(removed.first == afterMining->GetKernelCount() - old->GetKernelCount());
        writer->SetKernelIndexState(*old);
        writer->Commit();
    }

    std::string excesses;
    for (size_t height = 1; height < blocks.size(); ++height) {
        excesses += std::to_string(height) + " " + blocks[height].block.GetKernels().front().GetExcessCommitment().ToHex() + "\n";
    }
    const auto tip = chain->GetTipBlockHeader(EChainType::CONFIRMED);
    REQUIRE(tip != nullptr);
    std::vector<BlockHeaderPtr> headers;
    for (uint64_t h = 0; h <= tip->GetHeight(); ++h) {
        headers.push_back(chain->GetBlockHeaderByHeight(h, EChainType::CONFIRMED));
        REQUIRE(headers.back() != nullptr);
    }
    const std::string metadata = std::to_string(old->GetGeneration()) + "\n" + old->GetTipHash().ToHex() + "\n" +
        std::to_string(old->GetKernelCount()) + "\n" + old->GetChecksum().ToHex() + "\n" + tip->GetHash().ToHex() + "\n" +
        std::to_string(tip->GetNumKernels()) + "\n";
    const auto txHashSet = chain->GetTxHashSet();
    REQUIRE(txHashSet != nullptr);
    auto writer = db->BatchWrite();
    const auto proxy = std::make_shared<PausingBlockDB>(writer.GetShared(), marker, point, metadata, excesses);
    const auto lookup = [&headers](uint64_t h) -> BlockHeaderPtr { return h < headers.size() ? headers[static_cast<size_t>(h)] : nullptr; };
    KernelIndex::Ensure(proxy, txHashSet, tip, lookup);
    FAIL("extension returned without reaching the requested crash point");
}

TEST_CASE("KernelIndexCrashRecovery", "[.]")
{
    const std::string marker = CrashEnv("GPLUS_KERNEL_CRASH_MARKER");
    const std::string backup = CrashEnv("GPLUS_KERNEL_CRASH_BACKUP");
    REQUIRE_FALSE(marker.empty());
    REQUIRE_FALSE(backup.empty());
    std::ifstream in(marker);
    REQUIRE(in.good());
    std::string generation, oldTip, oldCount, oldChecksum, newTip, newCount, point, writes;
    REQUIRE(static_cast<bool>(std::getline(in, generation)));
    REQUIRE(static_cast<bool>(std::getline(in, oldTip)));
    REQUIRE(static_cast<bool>(std::getline(in, oldCount)));
    REQUIRE(static_cast<bool>(std::getline(in, oldChecksum)));
    REQUIRE(static_cast<bool>(std::getline(in, newTip)));
    REQUIRE(static_cast<bool>(std::getline(in, newCount)));
    REQUIRE(static_cast<bool>(std::getline(in, point)));
    REQUIRE(static_cast<bool>(std::getline(in, writes)));
    REQUIRE((point == "entry" || point == "state"));
    REQUIRE(std::stoull(writes) > 0);
    std::vector<std::pair<uint64_t, Commitment>> expected;
    std::string item;
    while (std::getline(in, item)) {
        const auto separator = item.find(' ');
        REQUIRE(separator != std::string::npos);
        expected.emplace_back(std::stoull(item.substr(0, separator)), Commitment::FromHex(item.substr(separator + 1)));
    }
    REQUIRE(expected.size() == 7);
    RestoreCrashData(backup);
    {
        const auto rawDB = DatabaseAPI::OpenDatabase(Global::GetConfig());
        const auto state = rawDB->GetBlockDB()->Read()->GetKernelIndexState();
        REQUIRE(state != nullptr);
        REQUIRE(state->GetGeneration() == std::stoull(generation));
        REQUIRE(state->GetTipHash().ToHex() == oldTip);
        REQUIRE(state->GetKernelCount() == std::stoull(oldCount));
        REQUIRE(state->GetChecksum().ToHex() == oldChecksum);
        const auto digest = rawDB->GetBlockDB()->Read()->GetKernelPositionDigest(state->GetGeneration());
        REQUIRE(digest.first == state->GetKernelCount());
        REQUIRE(digest.second == state->GetChecksum());
        for (const auto& entry : expected) {
            REQUIRE(rawDB->GetBlockDB()->Read()->GetKernelPosition(state->GetGeneration(), entry.second) == nullptr);
        }
    }
    TestServer::Ptr server = TestServer::OpenExisting();
    const auto state = server->GetBlockDB()->Read()->GetKernelIndexState();
    REQUIRE(state != nullptr);
    REQUIRE(state->GetGeneration() == std::stoull(generation));
    REQUIRE(state->GetTipHash().ToHex() == newTip);
    REQUIRE(state->GetKernelCount() == std::stoull(newCount));
    const auto digest = server->GetBlockDB()->Read()->GetKernelPositionDigest(state->GetGeneration());
    REQUIRE(digest.first == state->GetKernelCount());
    REQUIRE(digest.second == state->GetChecksum());
    for (const auto& entry : expected) {
        const auto location = server->GetBlockDB()->Read()->GetKernelPosition(state->GetGeneration(), entry.second);
        REQUIRE(location != nullptr);
        REQUIRE(location->GetBlockHeight() == entry.first);
    }
}
