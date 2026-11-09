#pragma once

#include <algorithm>
#include <cstddef>

// Peer connection policy. Historically MIN_PEERS was used as the *target*
// number of outbound connections, so old configs with 2-3 peers kept the node
// at 2-3 connections. Rust grin keeps PEER_MAX_OUTBOUND_COUNT (8) outbound
// connections regardless. Keep MIN_PEERS as the shortage threshold, but aim for
// 8 outbound connections (bounded by MAX_PEERS) so the PIBD scheduler has peers
// to rotate.
namespace SeederPolicy
{
	static constexpr size_t DEFAULT_TARGET_OUTBOUND = 8;

	inline size_t TargetOutbound(const size_t minPeers, const size_t maxPeers) noexcept
	{
		if (maxPeers == 0) {
			return 0;
		}

		const size_t defaultTarget = (std::min)(DEFAULT_TARGET_OUTBOUND, maxPeers);
		const size_t configuredMin = (std::min)(minPeers, maxPeers);
		return (std::max)(defaultTarget, configuredMin);
	}

	inline size_t ConnectionsToAdd(const size_t current, const size_t target, const size_t maxPerRound) noexcept
	{
		if (current >= target) {
			return 0;
		}

		return (std::min)(maxPerRound, target - current);
	}
}
