#pragma once

#include "Connection.h"

#include <Common/ConcurrentQueue.h>
#include <Core/Traits/Lockable.h>
#include <memory>
#include <vector>
#include <thread>
#include <optional>
#include <unordered_map>
#include <functional>

class ConnectionManager
{
public:
	static std::shared_ptr<ConnectionManager> Create();
	~ConnectionManager();

	void Shutdown();

	void UpdateSyncStatus(SyncStatus& syncStatus) const;

	size_t GetNumInbound() const { return m_numInbound; }
	size_t GetNumOutbound() const { return m_numOutbound; }
	size_t GetNumberOfActiveConnections() const { return m_connections.Read()->size(); }

	bool IsConnected(const IPAddress& address) const;
	std::vector<PeerPtr> GetMostWorkPeers() const;
	// Most-work peers with their per-connection height, needed to avoid asking a
	// peer for blocks above the height it reported in Ping (REQ_G38_FIX.md §3.3).
	std::vector<ConnectionPtr> GetMostWorkConnections() const;
	ConnectionPtr GetConnectionByAddress(const IPAddress& address) const;
	std::vector<ConnectedPeer> GetConnectedPeers() const;
	uint64_t GetMostWork() const;
	uint64_t GetHighestHeight() const;

	PeerPtr SendMessageToMostWorkPeer(
		const IMessage& message,
		const std::function<void(const PeerPtr&)>& beforeSend = {});
	bool SendMessageToPeer(const IMessage& message, PeerConstPtr pPeer);
	void BroadcastMessage(const IMessage& message, const uint64_t sourceId);

	void PruneConnections(const bool bInactiveOnly);
	void AddConnection(ConnectionPtr pConnection);
	ConnectionPtr GetConnection(const uint64_t connectionId) const;

private:
	ConnectionManager();

	ConnectionPtr GetMostWorkPeer(const std::vector<ConnectionPtr>& connections) const;
	static void ThreadPing(ConnectionManager& connectionManager);
	
	Locked<std::vector<ConnectionPtr>> m_connections;
	std::thread m_pingThread;

	std::atomic<size_t> m_numOutbound;
	std::atomic<size_t> m_numInbound;
};

typedef std::shared_ptr<ConnectionManager> ConnectionManagerPtr;
typedef std::shared_ptr<const ConnectionManager> ConnectionManagerConstPtr;
