#include "HeaderSyncer.h"
#include "../BlockLocator.h"
#include "../Messages/GetHeadersMessage.h"

#include <Common/Logger.h>

bool HeaderSyncer::SyncHeaders(const SyncStatus& syncStatus, const bool startup)
{
	// Client-only PIHD: dormant unless P2P.PIHD_MODE is explicitly client/full.
	// On any failure/timeout PihdClient returns false and legacy sync takes over.
	if (m_pPihdClient != nullptr && m_pPihdClient->TrySync()) {
		return true;
	}

	const uint64_t chainHeight = syncStatus.GetHeaderHeight();
	const uint64_t networkHeight = syncStatus.GetNetworkHeight();

	// This solves an issue that occurs due to the handshake not containing height.
	if (networkHeight == 0)
	{
		return true;
	}

	if (networkHeight >= (chainHeight + 5) || (startup && networkHeight > chainHeight))
	{
		if (IsHeaderSyncDue(syncStatus))
		{
			RequestHeaders(syncStatus);
		}

		return true;
	}

	m_pPeer = nullptr;
	m_retried = false;

	return false;
}

bool HeaderSyncer::IsHeaderSyncDue(const SyncStatus& syncStatus)
{
	if (m_pPeer == nullptr)
	{
		return true;
	}

	const uint64_t height = syncStatus.GetHeaderHeight();

	// Any growth counts as progress; a peer that keeps sending duplicates or
	// nothing must not be mistaken for fraud.
	if (height > m_lastProgressHeight) {
		m_lastProgressHeight = height;
		m_lastProgress = BlockSyncPolicy::Clock::now();
	}

	// Check if headers were received, and we're ready to request next batch.
	if (height >= (m_lastHeight + P2P::MAX_BLOCK_HEADERS - 1))
	{
		LOG_TRACE("Headers received. Requesting next batch.");
		m_retried = false;
		return true;
	}

	if (!m_pConnectionManager.lock()->IsConnected(m_pPeer->GetIPAddress()))
	{
		LOG_TRACE("Peer disconnected. Requesting from new peer.");
		m_pPeer = nullptr;
		return true;
	}

	// Check if header download timed out.
	if (m_timeout < std::chrono::system_clock::now())
	{
		if (m_pPeer != nullptr)
		{
			uint64_t peerHeight = 0;
			uint64_t peerDifficulty = 0;
			if (const auto pConnection =
				m_pConnectionManager.lock()->GetConnectionByAddress(m_pPeer->GetIPAddress())) {
				peerHeight = pConnection->GetHeight();
				peerDifficulty = pConnection->GetTotalDifficulty();
			}

			const auto secondsWithoutProgress =
				std::chrono::duration_cast<std::chrono::seconds>(
					BlockSyncPolicy::Clock::now() - m_lastProgress);
			const bool provenFraud = BlockSyncPolicy::IsProvenFraudHeight(
				peerHeight,
				peerDifficulty,
				syncStatus.GetHeaderHeight(),
				syncStatus.GetHeaderDifficulty(),
				secondsWithoutProgress);

			if (provenFraud)
			{
				LOG_ERROR_F(
					"Banning peer {} for fraud height: claimed height {} / difficulty {}, "
					"no header progress for {}s (ours: height {}, difficulty {}).",
					m_pPeer,
					peerHeight,
					peerDifficulty,
					secondsWithoutProgress.count(),
					syncStatus.GetHeaderHeight(),
					syncStatus.GetHeaderDifficulty());
				m_pPeer->Ban(EBanReason::FraudHeight);
				syncStatus.IncrementBansFraudHeight();
			}
			else
			{
				LOG_WARNING_F(
					"Header sync timeout from peer {} (claimed height {}, no progress for {}s); "
					"switching peer without ban.",
					m_pPeer,
					peerHeight,
					secondsWithoutProgress.count());
				m_retried = true;
			}
		}

		m_pPeer = nullptr;
		return true;
	}

	return false;
}

bool HeaderSyncer::RequestHeaders(const SyncStatus& syncStatus)
{
	if (!PeerRatePolicy::HeaderRequestDue(PeerRatePolicy::Clock::now(), m_nextHeaderRequest)) {
		return false;
	}
	LOG_TRACE("Requesting headers.");

	std::vector<Hash> locators = BlockLocator(m_pBlockChain).GetLocators(syncStatus);

	const GetHeadersMessage getHeadersMessage(std::move(locators));

	bool messageSent = false;
	if (m_pPeer != nullptr)
	{
		messageSent = m_pConnectionManager.lock()->SendMessageToPeer(getHeadersMessage, m_pPeer);
	}
	
	if (!messageSent)
	{
		m_pPeer = m_pConnectionManager.lock()->SendMessageToMostWorkPeer(getHeadersMessage);
	}

	if (m_pPeer != nullptr)
	{
		m_nextHeaderRequest = PeerRatePolicy::Clock::now() + PeerRatePolicy::MIN_HEADER_REQUEST_INTERVAL;
		LOG_TRACE("Headers requested.");
		m_timeout = std::chrono::system_clock::now() + std::chrono::seconds(10);
		m_lastHeight = syncStatus.GetHeaderHeight();
	}

	return m_pPeer != nullptr;
}
