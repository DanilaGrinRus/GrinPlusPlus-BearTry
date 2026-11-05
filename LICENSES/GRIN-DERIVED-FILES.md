# Grin-derived files — Apache-2.0 attribution

This is the canonical, verified list of files in this repository that contain material
derived from, or explicitly modelled on, **grin 5.5.2 (Rust node)**, Copyright
**The Grin Developers**, licensed under the **Apache License, Version 2.0**.

Full license text: [`Apache-2.0.txt`](Apache-2.0.txt). Component-level notices:
[`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) §5. The upstream Rust archive is
`grin-5.5.2.zip` (not redistributed); no Rust source or binary is shipped.

## Attribution header

Each file below starts with:

```
// Derived from grin 5.5.2 (Rust), Copyright The Grin Developers.
// Licensed under the Apache License, Version 2.0. Modified for the Grin++ C++ fork.
// See THIRD_PARTY_NOTICES.md and LICENSES/Apache-2.0.txt for details.
```

Headers were added on 2026-11-06 as a comment-only change (30 files, 90 lines); no
executable statement was modified. This satisfies Apache-2.0 §4(c) at file level while
keeping the released binary's source content functionally identical.

## Files (30)

| # | Path | Nature |
|---|---|---|
| 1 | `include/Consensus.h` | PIBD horizon interval from grin `global::TXHASHSET_ARCHIVE_INTERVAL` |
| 2 | `include/P2P/Capabilities.h` | PIHD capability bit and capability layout from Grin |
| 3 | `include/Core/Config.h` | NRD/PIBD configuration surface derived from grin rules |
| 4 | `src/BlockChain/BlockChainImpl.cpp` | chain/init behaviour mirrored from grin 5.5.2 |
| 5 | `src/BlockChain/Validators/BlockValidator.cpp` | NRD/block validation rules derived from grin |
| 6 | `src/BlockChain/Processors/TxHashSetProcessor.cpp` | TxHashSet/PIBD processing rules |
| 7 | `src/Core/Config.cpp` | NRD/PIBD configuration defaults |
| 8 | `src/P2P/MessageProcessor.cpp` | wire message ids/rules mirroring grin |
| 9 | `src/P2P/Messages/MessageTypes.h` | wire message ids mirroring grin |
| 10 | `src/P2P/PIBD/PibdArchiveServer.h` | archive-header selection mirrors grin `chain.rs` |
| 11 | `src/P2P/PIBD/PibdParams.h` | segment heights/archive horizon copied 1:1 from grin |
| 12 | `src/P2P/Seed/DNSSeeder.cpp` | Grin 5.5.2 DNS seed list |
| 13 | `src/P2P/Sync/BlockSyncPolicy.h` | body/header sync policy mirrored from grin |
| 14 | `src/PMMR/Common/MMRUtil.h` | PIBD/segment helpers ported 1:1 |
| 15 | `src/PMMR/Models/BitmapSegment.h` | `BitmapSegment::into_segment` port |
| 16 | `src/PMMR/Models/Segment.h` | `Segment::root` / `first_unpruned_parent` ports |
| 17 | `src/PMMR/Models/SegmentBitmap.h` | `Segment::root` bitmap variants |
| 18 | `src/PMMR/Models/SegmentException.h` | `SegmentError` mirror |
| 19 | `src/PMMR/Models/SegmentGenerator.h` | `Segment::from_pmmr` / `SegmentProof::generate` port |
| 20 | `src/PMMR/Models/SegmentProof.h` | `SegmentProof::reconstruct_root` port |
| 21 | `src/PMMR/TxHashSetImpl.cpp` | TxHashSet/PIBD state rules |
| 22 | `src/TxPool/TransactionPoolImpl.h` | NRD pool policy derived from grin |
| 23 | `tests/fuzz/target_nrd_rules.cpp` | NRD rule vectors mirroring Grin `WEEK_HEIGHT` |
| 24 | `tests/src/Consensus/Test_PoWEdgeBits.cpp` | PoW edge-bit vectors |
| 25 | `tests/src/Net/Test_P2PCompat.cpp` | wire compatibility vectors |
| 26 | `tests/src/PMMR/Test_NrdDifferential.cpp` | NRD differential vectors pinned to grin 5.5.2 |
| 27 | `tests/src/PMMR/Test_NrdPolicy.cpp` | NRD policy vectors |
| 28 | `tests/src/PMMR/Test_PibdLiveVector.cpp` | PIBD live vectors |
| 29 | `tests/src/PMMR/Test_PibdServing.cpp` | archive-header/serving vectors |
| 30 | `tests/src/PMMR/Test_TxPoolNrdPolicy.cpp` | NRD transaction-pool vectors |

## Scope notes

- The fork as a whole remains **MIT** (project `LICENSE`). These 30 files contain
  Apache-2.0-derived portions; the Apache-2.0 text and attribution are retained.
- The upstream grin 5.5.2 archive contains **no `NOTICE` file**; nothing is fabricated.
- No Grin trademark right is granted; this fork is not presented as an official Grin or
  Grin++ product.
- `tools/audit/nrd/README.md` describes the Rust-oracle method; it is documentation, not
  compiled code, and carries the attribution in its text.
