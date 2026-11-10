#pragma once

#include "../ConnectionManager.h"
#include "../MessageProcessor.h"
#include <P2P/Seed/InboundAdmission.h>

#include <Core/Config.h>
#include <BlockChain/BlockChain.h>
#include <P2P/Direction.h>
#include <cstdint>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <asio.hpp>

// Forward Declarations
class PeerManager;
class Pipeline;

class Seeder
{
public:
	static std::unique_ptr<Seeder> Create(
		ConnectionManager& connectionManager,
		Locked<PeerManager> peerManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<Pipeline> pPipeline,
		SyncStatusConstPtr pSyncStatus,
		const Config& config,
		std::shared_ptr<PibdSync> pPibdSync = nullptr,
		std::shared_ptr<PihdClient> pPihdClient = nullptr
	);
	~Seeder();

private:
	Seeder(
		ConnectionManager& connectionManager,
		Locked<PeerManager> peerManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<MessageProcessor> pMessageProcessor,
		SyncStatusConstPtr pSyncStatus,
		const Config& config
	) : m_connectionManager(connectionManager),
		m_config(config),
		m_peerManager(peerManager),
		m_pBlockChain(pBlockChain),
		m_pMessageProcessor(pMessageProcessor),
		m_pSyncStatus(pSyncStatus),
		m_pAsioContext(std::make_shared<asio::io_context>()),
		m_terminate(false) { }

	static void Thread_AsioContext(Seeder& seeder);
	static void Thread_Seed(Seeder& seeder);
	void StartListener();

	void Accept(const asio::error_code& ec);
	bool SeedNewConnection();

	ConnectionManager& m_connectionManager;
	Locked<PeerManager> m_peerManager;
	IBlockChain::Ptr m_pBlockChain;
	std::shared_ptr<MessageProcessor> m_pMessageProcessor;
	SyncStatusConstPtr m_pSyncStatus;
	const Config& m_config;

	std::atomic<bool> m_terminate = true;
	// DNS discovery is retried when no candidate is found; a single attempt at
	// startup leaves the node peerless when the seed list happens to be stale.
	std::chrono::system_clock::time_point m_lastDnsAttempt =
		std::chrono::system_clock::time_point::min();

	std::shared_ptr<asio::io_context> m_pAsioContext;
	std::shared_ptr<asio::ip::tcp::acceptor> m_pAcceptor;
	std::shared_ptr<asio::ip::tcp::socket> m_pSocket;

	// Bounds pre-handshake inbound connections. MAX_PEERS alone only bounds
	// established connections, so a peer could otherwise open an unbounded
	// number of sockets that each hold a handshake thread. Shared ownership
	// lets a detached handshake thread release its slot after Seeder shutdown.
	std::shared_ptr<P2P::InboundAdmission> m_inboundAdmission =
		std::make_shared<P2P::InboundAdmission>();

	std::thread m_asioThread;
	std::thread m_seedThread;

	std::mutex m_mutex;
	mutable uint64_t m_nextId = { 1 };
};