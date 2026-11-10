#pragma once

#include "PibdDownload.h"
#include "PibdParams.h"
#include "PibdSession.h"

#include "../Messages/SegmentResponseMessage.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Drives a PIBD session over time: issues requests up to an in-flight limit,
// retries segments that time out, and gives up (for the legacy txhashset.zip
// fallback) after too many attempts or too long without progress.
//
// Time is injected so the retry/timeout behaviour is deterministic in tests; the
// network layer passes its own clock.
class PibdScheduler
{
public:
	using Clock = std::chrono::steady_clock;

	struct Config
	{
		size_t MaxInFlight = PibdParams::SEGMENT_REQUEST_COUNT;
		std::chrono::seconds Timeout = PibdParams::SEGMENT_REQUEST_TIMEOUT;
		size_t MaxAttempts = 3;
		std::chrono::seconds FallbackAfter = PibdParams::TXHASHSET_ZIP_FALLBACK_TIME;
		// A peer that is alive but too slow must also trigger rotation: if fewer
		// than MinSegmentsPerWindow are accepted during RateWindow we fail the
		// attempt so the caller can pick another peer.
		// A slow but alive peer often sends in bursts; a short window used to
		// abort the whole attempt after one quiet 180s period. Keep the rate
		// guard only for a genuinely dead peer: 10 minutes and at least one
		// accepted segment, while the no-progress timer (660s) still bounds a
		// completely silent peer.
		std::chrono::seconds RateWindow{ 600 };
		size_t MinSegmentsPerWindow = 1;
	};

	PibdScheduler(PibdDownload::Horizon horizon, const Clock::time_point now, Config config)
		: m_session(std::move(horizon)),
		m_config(config),
		m_lastProgress(now),
		m_rateWindowStart(now)
	{
	}

	PibdScheduler(PibdDownload::Horizon horizon, const Clock::time_point now)
		: PibdScheduler(std::move(horizon), now, Config())
	{
	}

	const PibdSession& GetSession() const noexcept { return m_session; }
	PibdSession& GetSession() noexcept { return m_session; }

	bool IsComplete() const noexcept { return m_session.GetDownload().IsComplete(); }
	bool IsFailed() const noexcept { return m_failed; }
	const std::string& GetFailure() const noexcept { return m_failure; }
	size_t GetInFlightCount() const noexcept { return m_inFlight.size(); }
	size_t GetTotalRequestsSent() const noexcept { return m_requestsSent; }
	size_t GetReceivedSegments() const noexcept { return m_session.GetDownload().GetReceivedCount(); }
	size_t GetRequiredSegments() const noexcept { return m_session.GetDownload().GetRequiredCount(); }

	struct AssignedRequest
	{
		IMessagePtr Message;
		PibdDownload::SegmentRequest Request;
		std::string Peer;
	};

	// Multi-peer path: assign each request to the least-loaded eligible peer,
	// bounded by the global MaxInFlight and perPeerLimit. An empty peer list
	// returns no requests (single-peer callers keep using TakeRequests()).
	// Defined inline below, after the single-peer path.

	// Release a request whose send failed, or all requests assigned to a peer
	// that disconnected. The segment becomes eligible for reassignment.
	size_t GetWrongPeerResponses() const noexcept { return m_wrongPeerResponses; }
	size_t GetLateResponses() const noexcept { return m_lateResponses; }

	// Expire timed-out requests and return the messages to send now.
	std::vector<IMessagePtr> TakeRequests(const Clock::time_point now)
	{
		if (m_failed || IsComplete()) {
			return {};
		}

		Expire(now);
		if (m_failed) {
			return {};
		}

		// Like the Rust fallback timer: a stalled peer must not abort the whole
		// download after a few segment retries; only a long period without any
		// accepted response gives up.
		if (now - m_lastProgress >= m_config.FallbackAfter) {
			Fail("no PIBD progress for " +
				std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
					now - m_lastProgress).count()) + "s");
			return {};
		}

		if (now - m_rateWindowStart >= m_config.RateWindow) {
			if (m_windowAccepted < m_config.MinSegmentsPerWindow) {
				Fail("PIBD download rate too low: " +
					std::to_string(m_windowAccepted) + " segments in " +
					std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
						now - m_rateWindowStart).count()) + "s");
				return {};
			}

			m_rateWindowStart = now;
			m_windowAccepted = 0;
		}

		const size_t capacity = (m_config.MaxInFlight > m_inFlight.size())
			? m_config.MaxInFlight - m_inFlight.size()
			: 0;
		if (capacity == 0) {
			return {};
		}

		// Ask the session for more candidates than we can send, then drop the ones
		// that are already in flight.
		const std::vector<PibdDownload::SegmentRequest> candidates =
			m_session.GetDownload().NextRequests(capacity + m_inFlight.size());
		std::vector<IMessagePtr> messages;
		for (const PibdDownload::SegmentRequest& candidate : candidates) {
			if (messages.size() >= capacity) {
				break;
			}

			if (IsInFlight(candidate)) {
				continue;
			}

			uint32_t attempts = m_attempts[Key(candidate)] + 1;
			if (attempts > m_config.MaxAttempts) {
				// Keep retrying with a capped backoff: another peer or a slower
				// response may still complete this segment. The no-progress timer
				// above is the only condition that ends the attempt.
				attempts = 1;
			}

			m_attempts[Key(candidate)] = attempts;
			const auto backoff = (std::min)(
				m_config.Timeout * (int64_t)attempts,
				std::chrono::seconds(60));
			m_inFlight.push_back(Pending{ candidate, now + backoff, std::string() });
			messages.push_back(m_session.CreateRequest(candidate));
			++m_requestsSent;
		}

		return messages;
	}

	std::vector<AssignedRequest> TakeRequestsForPeers(
		const Clock::time_point now,
		const std::vector<std::string>& peers,
		const size_t perPeerLimit)
	{
		if (peers.empty() || m_failed || IsComplete()) {
			return {};
		}

		Expire(now);
		if (m_failed) {
			return {};
		}
		if (now - m_lastProgress >= m_config.FallbackAfter) {
			Fail("no PIBD progress for " +
				std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
					now - m_lastProgress).count()) + "s");
			return {};
		}
		if (now - m_rateWindowStart >= m_config.RateWindow) {
			if (m_windowAccepted < m_config.MinSegmentsPerWindow) {
				Fail("PIBD download rate too low");
				return {};
			}
			m_rateWindowStart = now;
			m_windowAccepted = 0;
		}

		const size_t capacity = (m_config.MaxInFlight > m_inFlight.size())
			? m_config.MaxInFlight - m_inFlight.size()
			: 0;
		if (capacity == 0) {
			return {};
		}

		const size_t peerLimit = std::max<size_t>(1, perPeerLimit);
		const std::vector<PibdDownload::SegmentRequest> candidates =
			m_session.GetDownload().NextRequests(capacity + m_inFlight.size());
		std::vector<AssignedRequest> assigned;
		for (const PibdDownload::SegmentRequest& candidate : candidates) {
			if (assigned.size() >= capacity) {
				break;
			}
			if (IsInFlight(candidate)) {
				continue;
			}

			size_t bestPeer = peers.size();
			size_t bestLoad = peerLimit;
			for (size_t i = 0; i < peers.size(); ++i) {
				const size_t load = GetPeerInFlight(peers[i]);
				if (load < bestLoad) {
					bestLoad = load;
					bestPeer = i;
				}
			}
			if (bestPeer == peers.size()) {
				continue;
			}

			uint32_t attempts = m_attempts[Key(candidate)] + 1;
			if (attempts > m_config.MaxAttempts) {
				attempts = 1;
			}
			m_attempts[Key(candidate)] = attempts;
			const auto backoff = (std::min)(
				m_config.Timeout * (int64_t)attempts,
				std::chrono::seconds(60));
			const std::string& peer = peers[bestPeer];
			m_inFlight.push_back(Pending{ candidate, now + backoff, peer });
			assigned.push_back(AssignedRequest{
				m_session.CreateRequest(candidate), candidate, peer });
			++m_requestsSent;
		}

		return assigned;
	}

	bool ReleaseAssignment(const AssignedRequest& assigned)
	{
		for (auto iter = m_inFlight.begin(); iter != m_inFlight.end(); ++iter) {
			if (iter->Request == assigned.Request && iter->Peer == assigned.Peer) {
				m_inFlight.erase(iter);
				return true;
			}
		}
		return false;
	}

	size_t ReleasePeer(const std::string& peer)
	{
		size_t removed = 0;
		for (auto iter = m_inFlight.begin(); iter != m_inFlight.end();) {
			if (iter->Peer == peer) {
				iter = m_inFlight.erase(iter);
				++removed;
			} else {
				++iter;
			}
		}
		return removed;
	}

	size_t GetPeerInFlight(const std::string& peer) const
	{
		size_t count = 0;
		for (const Pending& pending : m_inFlight) {
			if (pending.Peer == peer) {
				++count;
			}
		}
		return count;
	}

	// Feed a segment response. Returns false when the response is stale/duplicate
	// or for another horizon; throws SegmentException for an invalid payload.
	bool OnResponse(const SegmentResponseMessage& message, const Clock::time_point now)
	{
		return OnResponse(message, now, std::string());
	}

	// peer is the sender identity. Empty means "legacy/single peer"; otherwise a
	// response for an in-flight request assigned to a different peer is rejected
	// without touching the segment. A validated late response (no pending
	// assignment) is accepted to avoid losing progress and is counted as late.
	bool OnResponse(
		const SegmentResponseMessage& message,
		const Clock::time_point now,
		const std::string& peer)
	{
		const std::optional<PibdDownload::SegmentRequest> request =
			PibdSession::ResponseRequest(message);
		if (!request.has_value()) {
			throw SegmentException(ESegmentError::Mismatch, "Message is not a segment response");
		}

		bool pending = false;
		for (const Pending& entry : m_inFlight) {
			if (entry.Request == request.value()) {
				pending = true;
				if (!peer.empty() && !entry.Peer.empty() && entry.Peer != peer) {
					++m_wrongPeerResponses;
					LOG_WARNING_F("PIBD response for {} came from {} but was assigned to {}; rejecting",
						Describe(request.value()), peer, entry.Peer);
					return false;
				}
				break;
			}
		}

		const bool applied = m_session.AcceptResponse(message);
		if (applied) {
			if (!pending) {
				++m_lateResponses;
			}
			RemoveInFlight(request.value());
			m_lastProgress = now;
			++m_windowAccepted;
		}

		return applied;
	}

private:
	struct KeyValue
	{
		int Tree;
		uint8_t Height;
		uint64_t Idx;

		bool operator<(const KeyValue& rhs) const noexcept
		{
			if (Tree != rhs.Tree) {
				return Tree < rhs.Tree;
			}

			if (Height != rhs.Height) {
				return Height < rhs.Height;
			}

			return Idx < rhs.Idx;
		}
	};

	struct Pending
	{
		PibdDownload::SegmentRequest Request;
		Clock::time_point Deadline;
		std::string Peer;
	};

	static KeyValue Key(const PibdDownload::SegmentRequest& request)
	{
		return KeyValue{
			(int)request.Tree,
			request.Identifier.GetHeight(),
			request.Identifier.GetIdx()
		};
	}

	static std::string Describe(const PibdDownload::SegmentRequest& request)
	{
		return "tree " + std::to_string((int)request.Tree) + " height " +
			std::to_string((int)request.Identifier.GetHeight()) + " idx " +
			std::to_string(request.Identifier.GetIdx());
	}

	bool IsInFlight(const PibdDownload::SegmentRequest& request) const
	{
		for (const Pending& pending : m_inFlight) {
			if (pending.Request == request) {
				return true;
			}
		}

		return false;
	}

	void RemoveInFlight(const PibdDownload::SegmentRequest& request)
	{
		for (auto iter = m_inFlight.begin(); iter != m_inFlight.end(); ++iter) {
			if (iter->Request == request) {
				m_inFlight.erase(iter);
				return;
			}
		}
	}

	void Expire(const Clock::time_point now)
	{
		for (auto iter = m_inFlight.begin(); iter != m_inFlight.end();) {
			if (iter->Deadline <= now) {
				iter = m_inFlight.erase(iter);
			} else {
				++iter;
			}
		}

		if (m_lastProgress + m_config.FallbackAfter <= now) {
			Fail("no PIBD progress within the fallback window");
		}
	}

	void Fail(const std::string& reason)
	{
		if (!m_failed) {
			m_failed = true;
			m_failure = reason;
		}
	}

	PibdSession m_session;
	Config m_config;
	std::map<KeyValue, uint32_t> m_attempts;
	std::vector<Pending> m_inFlight;
	Clock::time_point m_lastProgress;
	Clock::time_point m_rateWindowStart;
	size_t m_windowAccepted = 0;
	size_t m_requestsSent = 0;
	size_t m_wrongPeerResponses = 0;
	size_t m_lateResponses = 0;
	bool m_failed = false;
	std::string m_failure;
};

