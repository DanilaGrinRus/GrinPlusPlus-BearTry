# Architecture — the node in this fork

This page describes the node module (`GrinNode`) as it exists in the
`node-beta.1-candidate` tree. Wallet and UI are out of scope. The description is
based on the source at that tag and on the recorded runs; where a behaviour was
not verified, it is marked as such.

## 1. Component map

| Layer | Main components | Source |
|---|---|---|
| Entry point | option parsing, config load, logging, main loop, graceful shutdown | `src/Server/Main.cpp`, `src/Server/opt.h` |
| Node assembly | early health server, chain/P2P open, full REST/RPC server | `src/Server/Node/Node.cpp`, `NodeRestServer.cpp` |
| REST API | `/v1` handlers: status, chain, headers, blocks, peers, txhashset, shutdown, resync | `src/Server/Node/API/*.cpp` |
| JSON-RPC API | `/v2/foreign`, `/v2/owner`, access levels, secret | `src/API/Node/*`, `src/Net/Servers/RPC/*` |
| P2P | connections, handshake, seeding, peer manager, message processing | `src/P2P/*`, `include/P2P/*` |
| Sync | header sync, state sync, block sync, status state machine | `src/P2P/Sync/*` |
| PIBD | download, segment service, scheduler, session, activation, optional archive serving | `src/P2P/PIBD/*` |
| Chain | chain state machine, processors, validators, reorg | `src/BlockChain/*`, `src/BlockChain/Processors/*` |
| Storage | RocksDB block DB, chain files/checkpoints, header MMR, TxHashSet/PMMR | `src/Database/*`, `src/BlockChain/ChainState.cpp`, `src/PMMR/*` |
| Validation | header/PoW/transaction/range-proof verification, worker selection | `src/BlockChain/Validators/*`, `src/PoW/*`, `src/PMMR/RangeProofBatchVerifier.*` |
| Transaction pool | mempool and stem pool | `src/TxPool/*` |

Wallets are compiled from the same tree but are not part of Node Beta
acceptance. The wallet code path is exercised only by unit tests.

## 2. Startup and restart

The startup order is deliberate: the process should be observable while it is
still opening a multi-GB database.

1. Parse options and load `server_config.json` (or defaults). Create the data and
   log directories.
2. Start the early HTTP listener with `GET /v1/health` returning
   `{"status":"starting"}`. On the profiled state the first 200 arrived at about
   `0.56–0.62 s`.
3. Open the chain: chain files, header MMR, RocksDB block DB, TxHashSet manager.
   Verify or rebuild the KernelIndex if it is missing, tied to another tip, or
   past its trusted extension limits.
4. Create the full REST/RPC server (all `/v1`, `/v2/foreign`, `/v2/owner`
   handlers) and set health to `ready`. `/v1/status` exists only from this point.
5. Start P2P and the sync state machine. `ready` does not mean `fully_synced`:
   on the profiled state `ready` preceded full sync by roughly 19–25 s.

A restart on a consistent state re-opens the existing files, checks the kernel
index against the tip and continues. A restart on an inconsistent or
partially-written state is supposed to recover from the last atomic checkpoint
or fail closed rather than publish a partial state. Power-loss and real storage
failure are **NOT VERIFIED**; the kill-harness evidence covers forced process
termination only.

## 3. Initial sync and the sync state machine

`SyncStatus`/`StateSyncer` drives the phases:

1. **Peers.** DNS seeds and peer book produce outbound connections; the seeder
   targets the configured minimum/maximum peers and tolerates individual peer
   failures. A slow or failed peer is rotated without a ban. Header requests are
   rate-limited to one per 200 ms.
2. **Headers.** Header sync advances the header chain and header MMR toward the
   network height. `SYNCING_HEADERS` exposes current/highest height.
3. **State.** When the node is far enough behind, it needs a state snapshot
   (TxHashSet). The preferred path is **PIBD** (see below). If no capable peer is
   available, or PIBD cannot proceed, the node falls back to the legacy
   `txhashset.zip` request path (`SYNCING_TXHASHSET`, byte counters).
4. **Validation and activation.** The downloaded state is validated (kernel
   history, kernel sums, range proofs, kernel signatures), then installed. The
   KernelIndex is built for the installed tip.
5. **Catch-up.** `SYNCING_BLOCKS` requests the remaining blocks, applies them and
   verifies the resulting tip.
6. **Fully synced.** `NOT_SYNCING` means the block height caught up with the
   network. `FULLY_SYNCED` in the legacy `/v1/status` field.

The observed live Floonet run on the combined build
(`16C24DB4…`) followed exactly this sequence: headers `0 → 3 324 027`, PIBD
`4998/4998` segments, state validation about `1987 s`, KernelIndex generation 2
with `3 356 610` kernels, block catch-up to `3 324 064`, and a final height of
`3 324 082` after further blocks.

The candidate `4039086C…` then completed the same sequence on a fresh isolated
profile: PIBD from `3 321 360` (`17d0428f…`), `2 948 812/2 948 812` range proofs
in `1 215 538 ms` with 1 worker, TxHashSet validated, KernelIndex generation 2
(`3 357 330` kernels), catch-up, a graceful restart at 00:53 with
`kernel_index_check result=current` and no rebuild, and `FULLY_SYNCED` at
`3 324 513` before a graceful stop.

## 4. PIBD: segments, peers, resume and recovery

PIBD (peer-initiated block download) replaces the monolithic ZIP archive with
independent MMR segments. The implementation follows the Rust Grin 5.5.2 wire
format.

- **Segment heights** (`PibdParams.h`): bitmap segments `2^9` chunks, output /
  range-proof / kernel segments `2^11` leaves. Segment requests are bounded
  (`SEGMENT_REQUEST_COUNT = 15` in flight, `20 s` timeout, a small cache).
- **Download.** `PibdDownload`/`PibdSegmentService` request missing segments
  from capable peers, validate each payload against the expected roots/sizes and
  assemble the output/range-proof/kernel/bitmap MMRs in a staging directory.
- **Scheduler.** `PibdScheduler` assigns each request to the least-loaded peer,
  bounded by a global in-flight limit and a per-peer limit. Send failures release
  the assignment; disconnects release the peer's assignments; a response from an
  unexpected peer is rejected without losing the segment; validated late
  responses are accepted when no assignment exists; duplicates are stale. These
  policies are covered by a deterministic test matrix. Live adversarial
  behaviour at the P2P ban layer is **NOT VERIFIED**.
- **Resume.** Progress is persisted on disk; a restart can continue from the
  last valid segments rather than re-downloading everything. The precise
  restart-at-each-stage matrix is **PARTIAL/NOT VERIFIED** on the candidate.
- **Activation.** A complete staging state carries a completion marker. It is
  swapped into place with two renames and a marker file
  (`pibd_activation.pending`). If the process dies during the swap, the next
  start either finishes the activation or restores the backup; live files are
  never truncated in place. A failed recovery is bound to the horizon hash with
  `pibd_state.recovery_failed` so the node does not retry the same impossible
  recovery forever.
- **Legacy fallback.** If PIBD cannot be used, `txhashset.zip` download/extract
  with staging and atomic swap remains available. This path is separate from
  PIBD and has its own evidence history.
- **Archive serving** is an opt-in diagnostic mode
  (`GRINPP_PIBD_ARCHIVE_SERVING=1`) and is **not** part of normal node
  operation; public PIBD serving is NOT VERIFIED.

## 5. KernelIndex lifecycle

`KernelIndex` maps a kernel excess commitment to its PMMR position and block
height so `get_kernel` and validation do not scan the whole kernel MMR.

- The index is **bound to a generation and a specific tip**: tip hash/height,
  kernel root, kernel count and entry count. A lookup is used only when every
  bound value matches the current tip.
- On block application the index is updated inside the same chain write batch as
  the block: new kernel positions are appended, reorgs rewind the affected
  positions. If the append/rewind transaction fails, the old state remains and
  the chain write fails closed — no partial index is published.
- On startup `Ensure` checks the persisted state. If the saved tip is a verified
  ancestor within bounded limits (recently: 4096 heights / 32768 kernels), only
  the missing kernels are appended in one transaction. Otherwise a full rebuild
  runs and publishes a new generation.
- Orphan blocks are repaired after commit (`EnsureKernelIndexLocked`), so a
  valid orphan cannot advance the chain while leaving the index stale.
- A rebuild of a ~3.36M-kernel state measured `101 870 ms` for entries,
  `16 435 ms` commit and `3 833 ms` read-back; peak working set reached
  `5 715 MB`. The chunked rebuild that would lower the peak is **PLANNED**, not
  implemented.
- Fault injection at the index transaction boundary and forced kills are covered
  by tests. Real RocksDB failure, post-commit failure and power loss are
  **NOT VERIFIED**.

## 6. Validation and thread selection

- Headers are verified for chain linkage, PoW, header MMR leaves and internal
  nodes, timestamps, version and total difficulty. Header/MMR validation takes
  `57.8–88.0 s` on the profiled ~3.32M-header state; an integrity-preserving
  parallel-chunk speedup is **PLANNED**.
- Range proofs are verified in batches (`RangeProofBatchVerifier`). The default
  worker count is **1**. `GRINPP_RANGEPROOF_WORKERS=auto` enables a bounded
  automatic selection based on process affinity, physical cores, available
  memory and Windows Job Object limits; `GRINPP_RANGEPROOF_WORKERS=1|2|3|4|…`
  forces a count.
- Job Object accounting uses per-process and job-wide committed memory, takes
  the minimum remaining limit, treats `< 1 MiB` exactly, and fails to the
  conservative one-worker path when the limit is unknown. A full-state
  1/2/3/4/auto benchmark is **OPEN**; the default stays 1.
- A local error during block processing (for example an index write failure)
  must not ban the peer: peer validity is unknown. This is covered by the
  BlockPipe contract test.

## 7. FastInit header checkpoint (experimental)

`HeaderStateCheckpoint` is an **experimental, opt-in** acceleration for header
verification. It is not part of normal startup and must not be presented as
production.

Trust model as implemented:

1. The checkpoint contains a full header-chain state and is only accepted after
   a full verification pass (leaf and internal MMR nodes, sequence, version,
   timestamp, previous root, difficulty monotonicity, confirmed-chain prefix,
   chain/MMR sizes).
2. A **first** checkpoint is written only when the operator explicitly sets
   `GRINPP_HEADER_CKPT_BOOTSTRAP=1` **and** supplies a published aggregate anchor
   through `GRINPP_HEADER_CKPT_TRUST_ANCHOR`. Without the anchor the node logs
   `header_checkpoint_not_written reason=no_trusted_anchor` and keeps doing full
   verification.
3. An existing checkpoint is reused only when its metadata still matches the
   chain files; otherwise the node falls back to full verification.
4. After a successful sync the checkpoint may be refreshed. The measured refresh
   on the profiled state was `9.56 s`; a normal restart using the checkpoint was
   `7.8 s`.
5. If the process is killed during a checkpoint refresh, the previous checkpoint
   remains intact (observed once, 31 081 bytes, unchanged hash).

Limits: a real reorg on a valid alternative branch, profile-lock behaviour and
post-kill continuation were not verified. The timing numbers are not a final,
controlled series. Treat every FastInit result as **EXPERIMENTAL / NOT
VERIFIED**.

## 8. On-disk layout

Default root: `%USERPROFILE%\.GrinPP\<NETWORK>` (override with `DATA_PATH` in the
config). Everything below is under that root.

| Path | Content |
|---|---|
| `server_config.json` | Node configuration (unless `--config-path` is used) |
| `.api_secret` | 64-hex API secret, owner-only; generated on first start |
| `LOGS\` | Node logs |
| `NODE\CHAIN\` | `confirmed.chain`, `candidate.chain`, `header_mmr.bin`, `chain.checkpoint` (atomic chain checkpoint), optional `header.ckpt` (FastInit) |
| `NODE\DB\` | RocksDB block/kernel database |
| `NODE\TXHASHSET\` | Live state: `output/`, `rangeproof/`, `kernel/` MMR files, `txhashset.bin` and related files |
| `NODE\pibd_staging\` | In-progress PIBD state (same tree layout) |
| `NODE\pibd_activation.pending` | Marker for an in-flight activation swap |
| `NODE\TXHASHSET\pibd_state.complete` | Completion marker inside a verified staging/live state |
| `NODE\TXHASHSET\pibd_state.recovery_failed` | Bound marker preventing infinite recovery of one horizon |
| `TOR\` | Tor data when Tor is enabled |
| `logs\` next to the executable | Diagnostic node logs; falls back to `LOGS\node-diagnostics` if not writable |

Profiles are not portable between machines without care: the database lock, the
API secret ACL and absolute paths in the config matter. Never copy a profile
that contains real wallet keys into a test environment.

## 9. API and trust boundaries

- The HTTP server binds locally. PRIVATE/ADMIN methods require the API secret and
  a loopback `Host`/`Origin`; PUBLIC methods are read-only. See
  [API.md](API.md).
- Peer input is untrusted: message parsers have explicit size limits, malformed
  or wrong-root segments fail closed, and a peer is not banned for a local
  processing error.
- The node never logs the API secret, seed words or private keys. Do not add
  them to diagnostics.

## 10. Known architectural limits

- No verified Mainnet run; no power-loss or real disk-failure verification.
- Public reorg only synthetic; real-network reorg is NOT VERIFIED.
- Full-state worker benchmark and automatic default selection are open.
- KernelIndex rebuild peak memory (~5.7 GB) and header validation time
  (~58–88 s) are unresolved performance items.
- FastInit and PIHD are experimental/planned and disabled by default.
- Wallet and UI are separate, unverified tracks.

---

[Русская версия](ARCHITECTURE.ru.md) · [Deutsche Fassung](ARCHITECTURE.de.md)
