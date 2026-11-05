# From upstream v1.2.9 to the current candidate

This document explains what changed between upstream GrinPlusPlus `v1.2.9` and
the `node-beta.1-candidate` tree, why it changed, what was verified and what
remains open. The canonical language is English; faithful Russian and German
versions are published as `.ru.md` and `.de.md`.

## Provenance and reconstruction (read this first)

- Upstream repository: https://github.com/GrinPlusPlus/GrinPlusPlus, tag
  `v1.2.9`, commit `a0958a391c30cf630e3e08534fbc6fe9fb194326` (2024-10-22).
  Upstream history and authorship are preserved in this repository.
- The 2026 development work happened in a local workspace whose source tree
  began from a **single squashed import commit**. The original development
  chronology, branch names and intermediate commits were not preserved.
- The commits in this fork after `v1.2.9` are **RECONSTRUCTED** from the verified
  final diff and the project evidence. They are grouped by purpose, not by the
  order in which the work actually happened. Reconstructed commits use an
  honest fork-preparation identity and carry evidence references in their
  message trailers; they do not reuse upstream authors or invent dates.
- Therefore: "commit A introduced X" in this document means "the reconstructed
  commit group that owns X", not a claim about the original chronology.
- The exact final source identity is the manifest
  `docs/evidence/files/candidate-source-manifest-final.sha256` (1528 files,
  SHA-256 `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627`).

### Reconstructed commit groups

Tag `node-beta.1-candidate` is an annotated tag (object
`317d143d2fc0d8b7ca8a7190521bca3e2f211a38`) whose tagged commit is
`7efdb02e2a4d714d406b88b5610cb6e1c9d2bf59`. The short SHAs below are the
reconstructed commit series; if the tag is moved before publication, re-read
`git log v1.2.9..node-beta.1-candidate` and update this table in the same change.

| Group | Scope | Commit |
|---|---|---|
| A | Build/test infrastructure and repository hygiene | `55b4d114`, `079fce77` |
| B | Wire-format, locking, logging and file I/O hardening | `1939ace0` |
| C | Durable chain stores, recovery, checkpoints and fail-closed rewind | `5cd26f88` |
| D | PIBD models, messages and MMR traversal helpers | `ebe67ed3` |
| E | PIBD download, verification, resume and atomic activation | `b0e60e86` |
| F | Peer hygiene, multi-peer scheduling and PIBD/PIHD client wiring | `5d83acef` |
| G | Persistent kernel index and fast kernel lookup | `fe4ea6c2` |
| H | NRD validation, PoW edge-bit checks and parallel range-proof verification | `4edc061a` |
| I | Rust-compatible node REST/owner API, access control and status schema | `b6b39413` |
| J | FastInit experimental opt-in header state checkpoint | `67ed50d3` |
| K | Wallet node client, SQLite store and lifecycle fixes | `9c289387` |
| L | Third-party notices, dependency licenses and omitted-tools note | `0f3e3e65` |
| M | Integration: wire PIBD, KernelIndex and FastInit into the sync paths | `7efdb02e` |

## Status vocabulary

**PASS** — observed on the stated binary/scenario; **PARTIAL** — implemented and
partly verified; **FAIL** — observed failure; **NOT VERIFIED** — no qualifying
evidence; **PLANNED** — design only; **EXPERIMENTAL** — opt-in code, not
production.

## 1. Wire format and error-handling hardening

- **Initial behaviour (v1.2.9).** Message parsing used readers that could
  over-read or overflow on hostile lengths; several failure paths threw generic
  exceptions; capabilities and message tables were incomplete relative to Grin
  5.5.2.
- **Problem and conditions.** Malformed or truncated peer messages, oversized
  varints/byte buffers, wrong magic bytes and unknown message types could abort
  processing instead of rejecting one message cleanly. The problem was
  established by code analysis and by malformed-input tests; some historical
  crashes were observed but their root cause was not fully established.
- **Research.** The wire contract was compared with Rust Grin 5.5.2 (`p2p/`,
  `core/` serialization) and exercised with fuzz corpora and negative cases.
- **Implemented change.** Bounded reads and explicit serialization errors in
  `ByteBuffer`, message header/type/capability tables, block/compact-block and
  kernel parsing, and fail-closed handling at the P2P message boundary. Header
  MMR and block-header parsing got explicit limits.
- **Changed components.** `include/Core/Serialization/ByteBuffer.h`,
  `include/P2P/{Capabilities,Common,MessageTypes}.h`,
  `src/P2P/Messages/*`, `src/Core/Models/*`, `src/P2P/MessageProcessor.cpp`.
- **Tests and results.** Unit and negative tests are part of the 373-case suite;
  historical ASan/fuzz campaigns are recorded in the local project evidence. A
  fresh fuzz/ASan campaign on the final candidate is **NOT VERIFIED**.
- **Compatibility / performance / data impact.** No wire-format change for valid
  peers; invalid messages now fail closed. No data migration.
- **Remaining limits.** Full sanitizer coverage and a fresh fuzz campaign on the
  candidate are open; some historical crash reports remain unexplained.
- **Links.** [../evidence/INDEX.md](../evidence/INDEX.md), [../API.md](../API.md).

## 2. Peer management, rate limiting and block-sync diagnostics

- **Initial behaviour.** The node banned a peer for "exceeding rate limit" based
  on a counter that also included the node's own outbound messages; rapid header
  sync could therefore ban a well-behaved peer. Block sync counted a timeout
  every time a request was not answered, without separating early cooldown
  reassignment, and a manual unban did not clear peers that existed only in the
  DB.
- **Problem and conditions.** Observed on local and public Floonet runs
  (2026-11-04: `Banning 127.0.0.1 for exceeding rate limit` during header
  download). Header requests were sent back-to-back.
- **Research.** Compared with Rust Grin 5.5.2: abuse accounting is based on
  inbound messages; header requests are paced. The block-sync counters and the
  peer book were re-read against the Rust sync policy.
- **Implemented change.** Rate limiting counts inbound messages only; header
  requests are paced (one per 200 ms); a timeout reassigns to another peer
  without a ban; `timeouts`, `retries`, `reassignments` and
  `cooldown_reassignments` are distinct counters; manual ban/unban works for DB-
  only peers and rolls back on write failure; diagnostics expose per-peer and
  download state.
- **Changed components.** `src/P2P/Connection.cpp`, `BlockSyncer.*`,
  `PeerManager.*`, `Seeder.*`, `src/Server/Node/API/PeersAPI.cpp`,
  `src/P2P/SyncStatus.*`.
- **Tests and results.** Focused tests: rate 5 assertions, BlockSyncPolicy 35,
  unban 19. Live short catch-up on Floonet passed (22 blocks, `FULLY_SYNCED`);
  the combined-build full run passed with 10 connections and no invalid-data/fraud
  bans. Status counter meanings were verified on intermediate candidates.
- **Compatibility / performance / data impact.** No API removal; counter
  semantics are clearer and some names are new. A peer may be retried sooner.
- **Remaining limits.** Single-peer recovery, long-duration catch-up,
  adversarial peers and `/v1/status` latency under load are **NOT VERIFIED**.
  Timeouts can exceed requests because of repeated checks; this is not proof of
  a network fault.
- **Links.** [../evidence/INDEX.md](../evidence/INDEX.md).

## 3. PIBD state sync

- **Initial behaviour.** State download used the monolithic `txhashset.zip`
  request/extract path with byte counters and a long validation phase.
- **Problem and conditions.** The ZIP path is slow, all-or-nothing and has weak
  resumability; it also made restart during state download expensive. The goal
  was to implement the Grin 5.5.2 PIBD protocol and keep the legacy path as a
  fallback.
- **Research.** Wire format, segment arithmetic, bitmap chunk layout, root
  computation and activation order were taken from Rust Grin 5.5.2 and checked
  with an oracle/vector suite where fixtures were available.
- **Implemented change.** Segment models and messages, download and verification,
  staged assembly of the four MMRs, validation, atomic activation with marker
  recovery, resume, peer rotation on timeouts/zero progress, and legacy ZIP
  fallback. Archive serving exists only as an opt-in diagnostic mode.
- **Changed components.** `src/P2P/PIBD/*`, `src/P2P/Sync/StateSyncer.*`,
  `src/PMMR/*`, `src/BlockChain/Processors/TxHashSetProcessor.*`,
  `src/P2P/Pipeline/TxHashSetPipe.*`.
- **Tests and results.** Synthetic archive install + index + restart: 25
  assertions PASS. On the earlier combined build, a live Floonet run downloaded
  `4998/4998` segments at horizon `3 320 640`, validated the state and reached
  `fully_synced`. On the candidate itself, a fresh isolated Floonet run started
  PIBD at `3 321 360`, downloaded `4999/4999` segments, verified
  `2 948 812/2 948 812` range proofs in `1 215 538 ms` with 1 worker, validated
  the TxHashSet, built KernelIndex generation 2 (`3 357 330` kernels), caught up,
  restarted at 00:53 with `kernel_index_check result=current` and no rebuild, and
  reached `FULLY_SYNCED` at `3 324 513` before a graceful stop. The one-segment
  difference between `4998/4998` and `4999/4999` follows from the different
  horizon heights and is not a defect; counts are not transferred between builds.
  The hidden `FullSweepPpCkpt` test can pass with an empty fixture warning, so it
  is not evidence of a populated sweep.
- **Compatibility / performance / data impact.** PIBD state is written in the
  same on-disk layout as the ZIP path; the staging/activation directories are
  new. Validation dominates initial sync (about 1987 s in the measured run,
  range proofs being the largest part).
- **Remaining limits.** Mainnet, restart at each PIBD stage, adversarial peers,
  and a populated full-sweep fixture are **NOT VERIFIED**. Legacy ZIP fallback on
  the candidate is not re-verified.
- **Links.** [../ARCHITECTURE.md](../ARCHITECTURE.md), [../evidence/INDEX.md](../evidence/INDEX.md).

## 4. Multi-peer PIBD scheduler

- **Initial behaviour.** Early PIBD sent segment requests in a basic
  round-robin/first-peer fashion; responses were accepted without binding them
  to the peer that was asked.
- **Problem and conditions.** A slow or malicious peer could hold segments;
  send failures could leave requests stranded; duplicate/late responses could
  confuse accounting. This was established by design review and deterministic
  tests, not by a live attack.
- **Research.** A peer-binding/per-peer-limit contract was written and compared
  with the Rust request/response model.
- **Implemented change.** `PibdScheduler` assigns requests to the least-loaded
  peer within global/per-peer limits; releases on send failure and disconnect;
  rejects wrong-peer responses without dropping the segment; accepts validated
  late responses; treats duplicates as stale; invalid payloads fail closed
  without a false ban.
- **Changed components.** `src/P2P/PIBD/PibdScheduler.h`, `PibdSync.h`,
  `src/P2P/Sync/StateSyncer.*`, `src/P2P/MessageProcessor.cpp`.
- **Tests and results.** `PibdScheduler*`: 68 assertions / 9 cases PASS.
  Normal live multi-peer download was observed on the combined build (9 peers,
  `4998/4998`) and on the candidate (10 peers, `4999/4999` at a different
  horizon). Live adversarial behavior and false-ban at the P2P layer are **NOT
  VERIFIED**.
- **Compatibility / performance / data impact.** No format change; improved
  resilience. Live multi-peer throughput is not benchmarked against one peer.
- **Remaining limits.** Live N-peer vs 1-peer matrix, per-peer throughput and
  ban behavior under malicious segments.
- **Links.** [../ARCHITECTURE.md](../ARCHITECTURE.md).

## 5. KernelIndex

- **Initial behaviour.** `get_kernel` scanned the kernel MMR; the index either
  did not exist or was not updated during sync, so a large state could trigger a
  full index rebuild at every relevant tip change.
- **Problem and conditions.** Orphan processing could commit a block while the
  index append was refused, leaving a stale index; a restart after state install
  could rebuild the whole index. The orphan defect was reproduced with a
  regression test.
- **Research.** The index was bound to a tip/generation/root/count contract, and
  the RocksDB transaction boundary and reorg/recovery semantics were designed
  around that contract.
- **Implemented change.** Persistent generation-based index; apply/rewind inside
  the chain write batch; tip-boundary checks; bounded incremental extension
  (4096 heights / 32768 kernels) with full-rebuild fallback; orphan repair;
  fault-injection test proxy; kill recovery harness.
- **Changed components.** `src/PMMR/KernelIndex.*`, `Database/KernelIndexState.*`,
  `src/Database/BlockDBImpl.*`, `src/BlockChain/BlockChainImpl.*`,
  `Processors/BlockProcessor.*`, `src/API/Node/Handlers/GetKernelHandler.h`.
- **Tests and results.** Orphan regression 17 PASS (failed before the fix); write
  faults 81 PASS; install + restart 25 PASS; `get_kernel` direct-scan and indexed
  paths tested. Live combined run: generation 2, `3 356 610` kernels, index
  current at the tip; restart kept generation 2 and did not rebuild. Kill
  recovery on the candidate: 241 assertions, exit 0.
- **Compatibility / performance / data impact.** The index is additional on-disk
  state in the block DB; a mismatch causes a safe rebuild. Measured rebuild:
  ~122 s total `Ensure` and 5 715 MB peak. Startup with a current index was
  ~65 s to REST readiness on the legacy profiled state.
- **Remaining limits.** Real RocksDB failure, post-commit failure, power loss and
  a fresh-profile timing series are **NOT VERIFIED**. Chunked rebuild is
  **PLANNED**. `get_kernel` schema parity is partially checked only.
- **Links.** [../ARCHITECTURE.md](../ARCHITECTURE.md), [../evidence/INDEX.md](../evidence/INDEX.md).

## 6. Range-proof validation workers

- **Initial behaviour.** Range proofs were verified sequentially; historical
  experiments showed that parallel verification could reduce wall time.
- **Problem and conditions.** Full-state validation is the dominant cost (over
  30 minutes in the measured run). Parallelism must not weaken verification or
  over-commit memory on a busy workstation.
- **Research.** G-49 measured 2- and 4-worker variants; the worker policy was
  reworked around CPU affinity, physical cores, available memory and Windows Job
  Object limits.
- **Implemented change.** Batch verifier with configurable workers (default 1),
  explicit `1|2|…`, and opt-in `auto`; Job Object accounting fixed (per-process
  and job-wide committed memory, real zero limit, conservative fallback);
  benchmark reports actual verified proofs, not PMMR size.
- **Changed components.** `src/PMMR/RangeProofBatchVerifier.*`,
  `include/Core/Global.h`, `src/Server/Main.cpp`, CPU/memory probes.
- **Tests and results.** Worker policy and Job accounting tests pass; a synthetic
  10 001-proof benchmark exists (`1 worker` fastest on the small state). A
  2-worker full-state validation passed historically; the 4-worker full-state
  run was under different conditions. Full-state 1/2/3/4/auto is **NOT
  VERIFIED**.
- **Compatibility / performance / data impact.** Default behaviour is unchanged
  (1 worker), so no automatic performance promise is made. `auto` is explicit.
- **Remaining limits.** Comparable full-state benchmark, automatic default
  decision, memory thresholds, non-Windows limits.
- **Links.** [../evidence/INDEX.md](../evidence/INDEX.md).

## 7. Storage consistency and recovery

- **Initial behaviour.** Chain files and RocksDB could be left inconsistent by a
  crash; DB repair counts were not exposed; bodyless reorg could mutate state
  before discovering missing bodies; Rust variable-size kernel PMMR archives
  were not readable.
- **Problem and conditions.** Observed through recovery testing and synthetic
  kill/reorg scenarios; the exact power-loss behaviour was never established.
- **Research.** Atomic checkpoint ordering, db-recovery/repair, reorg preflight,
  and Rust v2 kernel PMMR storage were reviewed against the reference.
- **Implemented change.** Atomic `chain.checkpoint`, repair counting
  (`db_repaired`, `db_last_repair`), recovery copies, fail-closed reorg preflight
  with typed `LOCAL_REWIND_DATA_MISSING`, variable-size kernel PMMR loading,
  file-sync ordering, and checked chain/MMR invariants.
- **Changed components.** `src/BlockChain/ChainState.cpp`, `ChainConsistency.h`,
  `src/BlockChain/BlockChainImpl.cpp`, `src/Database/BlockDBImpl.*`,
  `src/Core/File/*`, `src/PMMR/KernelMMR.*`.
- **Tests and results.** Reorg/rollback 100 assertions / 5 cases; bodyless
  safe-refusal scenario passes; fresh local legacy state cycle and restart pass
  historically; combined live run restarted without state re-download. Kill
  harnesses pass but are not power loss.
- **Compatibility / performance / data impact.** Files are compatible with the
  upstream layout; a stale/missing index triggers a rebuild. The checkpoint is
  additional state and is designed to be atomic.
- **Remaining limits.** Power loss, disk-full, real RocksDB I/O errors, public
  reorg and an actual bodyless **reorg** are **NOT VERIFIED**; only safe refusal
  is supported.
- **Links.** [../ARCHITECTURE.md](../ARCHITECTURE.md).

## 8. API, authentication and diagnostics

- **Initial behaviour.** Owner endpoints and `/v1/shutdown` had no secret; the
  foreign endpoint exposed `update_config`; there was no owner `get_config`;
  `/v1/status` used only the legacy schema; four Rust owner chain-control methods
  were absent.
- **Problem and conditions.** Established by static audit of routes, handlers and
  the Rust reference; a foreign/read-only client could attempt an admin write and
  any local client could shut the node down.
- **Research.** Access levels and secret handling were ported from the API kit
  design but re-implemented; the status schema was mapped method by method.
- **Implemented change.** `ApiAccess` with a 64-hex owner-only secret, Bearer /
  `X-API-Secret`, PUBLIC/PRIVATE/ADMIN enforcement in `RPCServer`, loopback
  Host/Origin checks, protected shutdown/update_config/resync/ban, owner
  `ban_peer`/`unban_peer`, Rust-compatible status object plus legacy fields,
  fixed REST peer/lastkernels shapes, and transactional config update.
- **Changed components.** `src/Net/Servers/RPC/*`, `src/API/Node/*`,
  `src/Server/Node/NodeRestServer.cpp`, `src/Server/Node/API/*`.
- **Tests and results.** 27 local HTTP/RPC checks on intermediate candidates
  (auth, empty arrays, invalid numbers, resync, IPv6 ban/unban, shutdown) passed;
  full suite covers handlers; owner auth without a secret returns the expected
  error. The final candidate has not been re-run through the full HTTP matrix.
- **Compatibility / performance / data impact.** `GET /v1/peers/<ip>` is an
  object on newer candidates and `lastkernels` returns `{hash}` objects; external
  consumers may need adaptation. Auth is a breaking change for clients that
  relied on unauthenticated owner/shutdown access — intentionally.
- **Remaining limits.** Full Rust parity is **NOT VERIFIED**; four owner
  chain-control methods are **NOT IMPLEMENTED**; PIBD/validation status fields
  and peer identity (IP vs SocketAddr) differ; live API on the candidate and
  Mainnet API are not verified.
- **Links.** [../API.md](../API.md), [../STATUS.md](../STATUS.md).

## 9. Early health and startup observability

- **Initial behaviour.** The HTTP server was created only after the chain and
  database were open, so a long initialization looked like a dead process.
- **Problem and conditions.** Multi-minute startup on large profiles; operators
  and tools had no readiness signal.
- **Implemented change.** An early `/v1/health` listener (`starting`/`ready`)
  created before the chain, followed by the full REST/RPC server. `/v1/status`
  appears later by design. Startup timing is logged.
- **Tests and results.** Health first 200 at `0.56–0.62 s`; `ready` preceded
  `fully_synced` by roughly 19–25 s. `/v1/status` once exceeded 5 s after state
  validation; cause not established.
- **Remaining limits.** Observing `starting` under a consistently heavy reload is
  partially verified; status latency under load is open.
- **Links.** [../API.md](../API.md).

## 10. FastInit header checkpoint (experimental)

- **Initial behaviour.** Every start performed full header-chain and MMR
  validation, taking roughly 58–88 s on the profiled state.
- **Problem and conditions.** Repeated restarts spend most of their time
  re-verifying an unchanged header chain.
- **Research.** A checkpoint is safe only if its state is bound to the exact
  chain files and to a trusted anchor. Reorg and crash consistency were the main
  risks.
- **Implemented change.** Full `VerifyFull` (leaves and internal MMR nodes,
  sequence, fields, difficulty, prefixes, sizes), versioned checkpoint,
  trust-anchor-gated first creation, metadata-bound reuse, refresh after sync,
  full-verification fallback, and intact old checkpoint on interrupted refresh.
- **Changed components.** `src/BlockChain/HeaderStateCheckpoint.*`,
  `BlockChainImpl.*`, `src/Server/Main.cpp`, `src/Server/Node/Node.cpp`.
- **Tests and results.** Field corruption detection (version, timestamp, output
  root), internal MMR corruption rejection, stale-checkpoint fallback, refresh
  and normal restart were verified on the prototype stand. Fast path measured
  `7.8–14.2 s`. Merge/compile conflict was resolved in the combined source, but
  a merged live scenario, real reorg and profile lock were not verified.
- **Compatibility / performance / data impact.** Opt-in, disabled by default;
  adds `header.ckpt`; no consensus change.
- **Remaining limits.** **EXPERIMENTAL**; reorg, profile lock, post-kill
  continuation and final repeated timings are NOT VERIFIED. Do not use in
  production.
- **Links.** [../STATUS.md](../STATUS.md).

## 11. Wallet changes in the final diff

The final diff touches wallet files (send/finalize/cancel/refresh, SQLite
schema/session handling, slate handling). These changes are included because the
repository is a full Grin++ fork, but the wallet is **NOT READY** and is not part
of Node Beta acceptance. They are covered only by unit tests in the automated
suite; there is no live wallet send/receive/recovery acceptance. Do not present
them as a wallet release.

## What did not change

- Consensus rules, genesis, PoW verification and block reward logic were not
  intentionally changed; the node follows Grin 5.5.2 rules (NRD enabled on
  non-Mainnet, as upstream).
- The upstream license (MIT) and copyright are preserved; see `LICENSE`,
  `THIRD_PARTY_NOTICES.md`.
- The UI is not part of this repository.

## Evidence boundaries

- Every PASS in this document is bound to a specific binary and scenario; see
  [../evidence/INDEX.md](../evidence/INDEX.md).
- Historical results from earlier build IDs are labelled as historical and are
  never transferred to the candidate binary.
- Article "problem established by analysis" is marked as such: not every defect
  was reproduced live.

---

[Русская версия](FROM-UPSTREAM-TO-CURRENT.ru.md) ·
[Deutsche Fassung](FROM-UPSTREAM-TO-CURRENT.de.md)
