#pragma once

#include "Messages/RawMessage.h"
#include "Seed/PeerManager.h"

#include <BlockChain/BlockChain.h>
#include <P2P/AdvertisedTip.h>
#include <P2P/ConnectedPeer.h>
#include <memory>
#include <optional>
#include <chrono>

// Forward Declarations
class ConnectionManager;
class Connection;
class RawMessage;
class Pipeline;
class TxHashSetArchiveMessage;
class TxHashSetRequestMessage;
class PibdSync;
class PihdClient;

class MessageProcessor
{
public:
	enum EStatus
	{
		SUCCESS,
		SOCKET_FAILURE,
		UNKNOWN_ERROR,
		RESOURCE_NOT_FOUND,
		UNKNOWN_MESSAGE,
		SYNCING,
		DISCONNECT
	};

	MessageProcessor(
		ConnectionManager& connectionManager,
		Locked<PeerManager> peerManager,
		const IBlockChain::Ptr& pBlockChain,
		const std::shared_ptr<Pipeline>& pipeline,
		SyncStatusConstPtr pSyncStatus,
		std::shared_ptr<PibdSync> pPibdSync = nullptr,
		std::shared_ptr<PihdClient> pPihdClient = nullptr
	);

	void ProcessMessage(const std::shared_ptr<Connection>& pConnection, const RawMessage& rawMessage);

	// ONE coherent header-head snapshot for advertisement (height + TD from the
	// same tip; CANDIDATE because that is the header chain this node serves).
	// nullopt means no tip header is currently readable: callers must withhold
	// the advertisement instead of mixing values from two tips.
	std::optional<P2P::AdvertisedTip> GetAdvertisedTip() const;

private:
	void ProcessMessageInternal(const std::shared_ptr<Connection>& pConnection, const RawMessage& rawMessage);

	ConnectionManager& m_connectionManager;
	Locked<PeerManager> m_peerManager;
	IBlockChain::Ptr m_pBlockChain;
	std::shared_ptr<Pipeline> m_pPipeline;
	SyncStatusConstPtr m_pSyncStatus;
	std::shared_ptr<PibdSync> m_pPibdSync;
	std::shared_ptr<PihdClient> m_pPihdClient;
};