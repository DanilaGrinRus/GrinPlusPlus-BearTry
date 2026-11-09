#pragma once

#include "../ConnectionManager.h"
#include "../Messages/GetHeaderSegmentMessage.h"
#include "../Messages/HeaderSegmentMessage.h"

#include <BlockChain/BlockChain.h>
#include <P2P/PihdParams.h>
#include <Core/Config.h>
#include <Core/Global.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

// Client-only PIHD implementation.
//
// The reference v5.5.2 tag has no PIHD messages. This class is dormant unless
// P2P.PIHD_MODE=client/full, does not advertise PIHD_HIST, and always falls back
// to the legacy header sync path on timeout, invalid data or lack of a peer.
class PihdClient
{
public:
	using Clock = std::chrono::steady_clock;

	PihdClient(const std::weak_ptr<ConnectionManager>& pConnectionManager, const IBlockChain::Ptr& pBlockChain)
		: m_pConnectionManager(pConnectionManager),
		m_pBlockChain(pBlockChain),
		m_waiting(false),
		m_requestedIdx(0),
		m_requestedConnectionId(0),
		m_failures(0)
	{
	}

	// Returns true while PIHD owns header sync (request sent or waiting for a
	// response). Returns false when the caller must use legacy header sync.
	bool TrySync()
	{
		const auto mode = Global::GetConfig().GetPihdMode();
		if (mode == Pihd::EPihdMode::OFF) {
			return false;
		}

		std::lock_guard<std::mutex> lock(m_mutex);
		const auto now = Clock::now();
		if (m_disabledUntil != Clock::time_point{} && now >= m_disabledUntil) {
			// Cooldown expired: allow PIHD to try again instead of staying off
			// until process restart.
			m_disabledUntil = Clock::time_point{};
			m_failures = 0;
		}
		if (now < m_disabledUntil) {
			return false;
		}

		if (m_waiting) {
			if (now < m_deadline) {
				return true;
			}

			LOG_WARNING_F("PIHD segment {} timed out from peer {}", m_requestedIdx, m_pPeer);
			++m_failures;
			m_waiting = false;
			m_pPeer = nullptr;
			if (m_failures >= Pihd::FAILURES_BEFORE_FALLBACK) {
				m_disabledUntil = now + std::chrono::seconds(Pihd::FALLBACK_COOLDOWN_SECONDS);
				LOG_WARNING_F("PIHD disabled for {}s after {} failures; using legacy header sync",
					Pihd::FALLBACK_COOLDOWN_SECONDS,
					m_failures);
				return false;
			}
			return true;
		}

		if (m_failures >= Pihd::FAILURES_BEFORE_FALLBACK) {
			return false;
		}

		return RequestNextLocked();
	}

	void OnHeaderSegment(const ConnectionPtr& pConnection, const HeaderSegmentMessage& message)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_waiting || m_pPeer == nullptr) {
			return;
		}

		if (pConnection == nullptr || pConnection->GetId() != m_requestedConnectionId ||
			pConnection->GetPeer() != m_pPeer) {
			LOG_WARNING("Ignoring PIHD response from an unexpected connection");
			return;
		}

		const auto& identifier = message.GetIdentifier();
		if (identifier.GetHeight() != Pihd::HEADER_SEGMENT_HEIGHT ||
			identifier.GetIdx() != m_requestedIdx) {
			LOG_WARNING_F("Unexpected PIHD segment {} h{} from {}", identifier.GetIdx(), identifier.GetHeight(), pConnection);
			return;
		}

		const uint64_t start = Pihd::SegmentStartHeight(identifier.GetIdx());
		const auto& headers = message.GetHeaders();
		if (headers.empty() || headers.size() > Pihd::HEADERS_PER_SEGMENT) {
			LOG_WARNING_F("Invalid PIHD segment {} size {} from {}", identifier.GetIdx(), headers.size(), pConnection);
			BanAndFailLocked(pConnection);
			return;
		}

		std::vector<BlockHeaderPtr> blockHeaders;
		blockHeaders.reserve(headers.size());
		uint64_t expectedHeight = start;
		for (const BlockHeader& header : headers) {
			if (header.GetHeight() != expectedHeight) {
				LOG_WARNING_F("PIHD segment {} has non-contiguous height {} (expected {}) from {}",
					identifier.GetIdx(), header.GetHeight(), expectedHeight, pConnection);
				BanAndFailLocked(pConnection);
				return;
			}
			blockHeaders.push_back(std::make_shared<const BlockHeader>(header));
			++expectedHeight;
		}

		const uint64_t previousHeight = m_pBlockChain->GetHeight(EChainType::CANDIDATE);
		const EBlockChainStatus status = m_pBlockChain->AddBlockHeaders(blockHeaders);
		if (status == EBlockChainStatus::INVALID) {
			LOG_WARNING_F("PIHD segment {} rejected by chain from {}", identifier.GetIdx(), pConnection);
			BanAndFailLocked(pConnection);
			return;
		}

		const uint64_t currentHeight = m_pBlockChain->GetHeight(EChainType::CANDIDATE);
		if (status != EBlockChainStatus::SUCCESS || currentHeight <= previousHeight) {
			// A local failure or a valid but non-advancing response is not proof
			// of peer misconduct. Give legacy header sync an opportunity to run.
			LOG_WARNING_F("PIHD segment {} did not advance the candidate chain; falling back", identifier.GetIdx());
			m_waiting = false;
			m_pPeer = nullptr;
			m_failures = 0;
			m_disabledUntil = Clock::now() + std::chrono::seconds(Pihd::FALLBACK_COOLDOWN_SECONDS);
			return;
		}

		LOG_DEBUG_F("PIHD segment {} accepted: {} headers, height {}..{}",
			identifier.GetIdx(), headers.size(), start, expectedHeight - 1);
		m_waiting = false;
		m_pPeer = nullptr;
		m_failures = 0;
		m_disabledUntil = Clock::time_point{};
		m_lastProgress = Clock::now();
	}

private:
	bool RequestNextLocked()
	{
		const auto pConnectionManager = m_pConnectionManager.lock();
		if (pConnectionManager == nullptr) {
			return false;
		}

		const uint64_t localHeight = m_pBlockChain->GetHeight(EChainType::CANDIDATE);
		const uint64_t localDifficulty = m_pBlockChain->GetTotalDifficulty(EChainType::CANDIDATE);
		const uint64_t nextHeight = localHeight + 1;
		const uint64_t idx = Pihd::FirstSegmentIdxForHeight(nextHeight);

		ConnectionPtr selected = nullptr;
		for (const ConnectionPtr& pConnection : pConnectionManager->GetMostWorkConnections()) {
			if (pConnection == nullptr ||
				!pConnection->GetCapabilities().HasCapability(Capabilities::PIHD_HIST) ||
				pConnection->GetHeight() < nextHeight) {
				continue;
			}
			if (pConnection->GetTotalDifficulty() <= localDifficulty &&
				pConnection->GetHeight() <= localHeight) {
				continue;
			}

			if (selected == nullptr || pConnection->GetDirection() == EDirection::OUTBOUND) {
				selected = pConnection;
				if (pConnection->GetDirection() == EDirection::OUTBOUND) {
					break;
				}
			}
		}

		if (selected == nullptr) {
			LOG_DEBUG("PIHD: no peer with PIHD_HIST above our height; using legacy header sync");
			return false;
		}

		const GetHeaderSegmentMessage request(
			SegmentIdentifier(Pihd::HEADER_SEGMENT_HEIGHT, idx));
		if (!pConnectionManager->SendMessageToPeer(request, selected->GetPeer())) {
			LOG_DEBUG_F("PIHD: could not send segment {} to {}", idx, selected);
			return false;
		}

		m_pPeer = selected->GetPeer();
		m_requestedConnectionId = selected->GetId();
		m_requestedIdx = idx;
		m_waiting = true;
		m_deadline = Clock::now() + std::chrono::seconds(Pihd::SEGMENT_TIMEOUT_SECONDS);
		LOG_INFO_F("PIHD: requested header segment {} (start height {}) from {}",
			idx, Pihd::SegmentStartHeight(idx), selected);
		return true;
	}

	void BanAndFailLocked(const ConnectionPtr& pConnection)
	{
		pConnection->BanPeer(EBanReason::BadBlockHeader);
		m_waiting = false;
		m_pPeer = nullptr;
		++m_failures;
		if (m_failures >= Pihd::FAILURES_BEFORE_FALLBACK) {
			m_disabledUntil = Clock::now() + std::chrono::seconds(Pihd::FALLBACK_COOLDOWN_SECONDS);
			LOG_WARNING_F("PIHD disabled for {}s after invalid segments; using legacy header sync",
				Pihd::FALLBACK_COOLDOWN_SECONDS);
		}
	}

	std::weak_ptr<ConnectionManager> m_pConnectionManager;
	IBlockChain::Ptr m_pBlockChain;

	std::mutex m_mutex;
	bool m_waiting;
	uint64_t m_requestedIdx;
	uint64_t m_requestedConnectionId;
	PeerPtr m_pPeer;
	Clock::time_point m_deadline;
	Clock::time_point m_lastProgress;
	Clock::time_point m_disabledUntil;
	uint32_t m_failures;
};
