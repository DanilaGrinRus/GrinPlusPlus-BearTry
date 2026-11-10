#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace P2P
{
	//
	// Bounds pre-handshake inbound connections independently of MAX_PEERS.
	//
	// MAX_PEERS only counts established connections. Before this guard, an
	// unauthenticated peer could open an unbounded number of TCP connections
	// that were waiting in the handshake path, each holding a thread/buffer.
	// This limiter enforces:
	//   - a global cap on simultaneously pending handshakes;
	//   - an accept-rate token bucket;
	//   - a per-IP pending cap (catches one host opening many sockets).
	//
	// Release() must be called exactly once for every successful TryAcquire().
	// The class is header-only and clock-injectable so the limits are unit
	// testable without starting a node.
	//
	class InboundAdmission
	{
	public:
		using Clock = std::chrono::steady_clock;

		static constexpr std::size_t DEFAULT_MAX_PENDING = 32;
		static constexpr std::size_t DEFAULT_MAX_PER_SECOND = 16;
		static constexpr std::size_t DEFAULT_MAX_PER_IP = 4;

		InboundAdmission(
			const std::size_t maxPending = DEFAULT_MAX_PENDING,
			const std::size_t maxPerSecond = DEFAULT_MAX_PER_SECOND,
			const std::size_t maxPerIp = DEFAULT_MAX_PER_IP)
			: m_maxPending(maxPending),
			m_maxPerSecond(maxPerSecond),
			m_maxPerIp(maxPerIp),
			m_tokens(static_cast<double>(maxPerSecond)),
			m_lastRefill(Clock::now())
		{
		}

		bool TryAcquire(const std::string& ip)
		{
			return TryAcquire(ip, Clock::now());
		}

		bool TryAcquire(const std::string& ip, const Clock::time_point now)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Refill(now);

			if (m_pending >= m_maxPending || m_tokens < 1.0)
			{
				return false;
			}

			const auto iter = m_perIp.find(ip);
			const std::size_t ipPending = (iter == m_perIp.end()) ? 0 : iter->second;
			if (ipPending >= m_maxPerIp)
			{
				return false;
			}

			++m_pending;
			m_perIp[ip] = ipPending + 1;
			m_tokens -= 1.0;
			return true;
		}

		void Release(const std::string& ip)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_pending == 0)
			{
				return;
			}

			--m_pending;
			const auto iter = m_perIp.find(ip);
			if (iter != m_perIp.end())
			{
				if (iter->second <= 1)
				{
					m_perIp.erase(iter);
				}
				else
				{
					--iter->second;
				}
			}
		}

		std::size_t Pending() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_pending;
		}

	private:
		void Refill(const Clock::time_point now)
		{
			if (now <= m_lastRefill || m_maxPerSecond == 0)
			{
				m_lastRefill = (std::max)(m_lastRefill, now);
				return;
			}

			const double elapsed =
				std::chrono::duration<double>(now - m_lastRefill).count();
			m_tokens = (std::min)(
				static_cast<double>(m_maxPerSecond),
				m_tokens + elapsed * static_cast<double>(m_maxPerSecond));
			m_lastRefill = now;
		}

		const std::size_t m_maxPending;
		const std::size_t m_maxPerSecond;
		const std::size_t m_maxPerIp;

		mutable std::mutex m_mutex;
		std::size_t m_pending = 0;
		double m_tokens;
		Clock::time_point m_lastRefill;
		std::unordered_map<std::string, std::size_t> m_perIp;
	};
}
