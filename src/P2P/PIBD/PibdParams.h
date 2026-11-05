// Derived from grin 5.5.2 (Rust), Copyright The Grin Developers.
// Licensed under the Apache License, Version 2.0. Modified for the Grin++ C++ fork.
// See THIRD_PARTY_NOTICES.md and LICENSES/Apache-2.0.txt for details.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

// Static PIBD/desegmentation parameters.
// Values are copied 1:1 from grin v5.5.2 chain/src/pibd_params.rs so that a
// C++ node segments and schedules exactly like the Rust reference. These are
// protocol constants, not user configuration.
namespace PibdParams
{
	// Segment heights assumed for requests and segment calculation.
	static constexpr uint8_t BITMAP_SEGMENT_HEIGHT = 9;
	static constexpr uint8_t OUTPUT_SEGMENT_HEIGHT = 11;
	static constexpr uint8_t RANGEPROOF_SEGMENT_HEIGHT = 11;
	static constexpr uint8_t KERNEL_SEGMENT_HEIGHT = 11;

	// Maximum number of received segments cached (across all trees) before we
	// stop requesting more.
	static constexpr size_t MAX_CACHED_SEGMENTS = 15;

	// Number of simultaneous segment requests.
	static constexpr size_t SEGMENT_REQUEST_COUNT = 15;

	// How long to wait for a requested segment before re-requesting it.
	static constexpr std::chrono::seconds SEGMENT_REQUEST_TIMEOUT{ 20 };

	// If no max-work PIBD peer is seen for this long, fall back to the legacy
	// txhashset.zip download.
	static constexpr std::chrono::seconds TXHASHSET_ZIP_FALLBACK_TIME{ 660 };

	// Archive-horizon selection, ported from grin 5.5.2
	// chain/src/chain.rs txhashset_archive_header{,_header_only}:
	//   height = head.saturating_sub(threshold)
	//   height -= height % interval
	// The threshold/interval are passed explicitly (callers use
	// Consensus::STATE_SYNC_THRESHOLD / TXHASHSET_ARCHIVE_INTERVAL so this header
	// stays testable without pulling in Consensus/Global). Returns 0 when the
	// head is not above the threshold or the interval is invalid; 0 is the
	// genesis state and is never served.
	static constexpr uint64_t ComputeArchiveHorizonHeight(
		const uint64_t headHeight,
		const uint64_t threshold,
		const uint64_t interval) noexcept
	{
		if (interval == 0) {
			return 0;
		}

		const uint64_t adjusted = headHeight > threshold ? headHeight - threshold : 0;
		return adjusted - (adjusted % interval);
	}

	static constexpr bool IsServableArchiveHorizon(
		const uint64_t height,
		const uint64_t interval) noexcept
	{
		return interval != 0 && height != 0 && (height % interval) == 0;
	}

	// Per-tree accepted segment heights, copied from grin 5.5.2
	// servers/src/common/adapters.rs lines 50-53. Rust ranges are half-open:
	// bitmap/kernel 9..14 -> 9-13, output 11..16 -> 11-15, rangeproof 7..12 ->
	// 7-11. A blanket upper bound is not equivalent and must not be used.
	static constexpr bool IsValidBitmapSegmentHeight(const uint64_t height) noexcept
	{
		return height >= 9ull && height <= 13ull;
	}

	static constexpr bool IsValidKernelSegmentHeight(const uint64_t height) noexcept
	{
		return height >= 9ull && height <= 13ull;
	}

	static constexpr bool IsValidOutputSegmentHeight(const uint64_t height) noexcept
	{
		return height >= 11ull && height <= 15ull;
	}

	static constexpr bool IsValidRangeProofSegmentHeight(const uint64_t height) noexcept
	{
		return height >= 7ull && height <= 11ull;
	}
}
