#pragma once

#include <P2P/SyncStatus.h>

#include <cstdint>
#include <memory>
#include <optional>

// Grin 5.5.2 peers feed the advertised height/total difficulty into header-sync
// peer accounting. This node serves headers from the CANDIDATE chain (the header
// head, see BlockLocator::LocateHeaders), so the advertised values must come
// from the same header head. Advertising the CONFIRMED body tip instead lets a
// syncing peer overshoot the advertised difficulty with candidate headers and
// then stop requesting headers (G-09 L1 defect, see
// docs/modernization/G09_ARCHIVE_SERVING.md §8.9).
//
// Height and total difficulty must be read as ONE snapshot. A split read
// (GetHeight then GetTotalDifficulty, or two atomic SyncStatus fields) can mix
// two different tips while the chain advances. Advertisers therefore either use
// the single-snapshot accessors below or withhold the message when no snapshot
// is readable.
namespace P2P
{
	struct AdvertisedTip
	{
		uint64_t Height = 0;
		uint64_t TotalDifficulty = 0;
	};

	// Invokes the provider exactly once and derives both advertised values from
	// that one header/tip snapshot. A null snapshot yields nullopt so the caller
	// withholds the advertisement instead of mixing two tips.
	template<class TProvider>
	std::optional<AdvertisedTip> ReadAdvertisedTip(TProvider&& provider)
	{
		const auto pTip = provider();
		if (!pTip)
		{
			return std::nullopt;
		}

		return AdvertisedTip{ pTip->GetHeight(), pTip->GetTotalDifficulty() };
	}

	// Hand/Shake carry total difficulty only. ChainState::UpdateSyncStatus
	// refreshes the header status from the candidate head; block status is the
	// confirmed body tip and must not be advertised for header sync.
	inline uint64_t GetAdvertisedDifficulty(const SyncStatus& syncStatus)
	{
		return syncStatus.GetHeaderTip().TotalDifficulty;
	}

	// Snapshot view for Ping/Pong fallbacks and handshake code that only has the
	// shared SyncStatus. HeaderTip is stored copy-on-write under one lock.
	inline AdvertisedTip GetAdvertisedTip(const SyncStatus& syncStatus)
	{
		const HeaderTip headerTip = syncStatus.GetHeaderTip();
		return AdvertisedTip{ headerTip.Height, headerTip.TotalDifficulty };
	}
}
