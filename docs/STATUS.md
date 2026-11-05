# Status — Node Beta 1 candidate

This page is the authoritative status statement for the fork snapshot. It is
English-canonical; Russian and German translations are [`STATUS.ru.md`](STATUS.ru.md)
and [`STATUS.de.md`](STATUS.de.md).

Status vocabulary used everywhere in this documentation:

| Label | Meaning |
|---|---|
| **PASS** | Observed on the stated commit and binary, in the stated environment and scenario; evidence is recorded. |
| **PARTIAL** | Implemented and partly exercised; at least one required scenario, environment or consumer was not verified. |
| **FAIL** | Observed to fail in a defined scenario. Not used when a test was merely skipped. |
| **NOT VERIFIED** | No qualifying evidence exists. A skipped test, a missing fixture, a timeout or a mock is not a PASS. |
| **PLANNED** | Designed or scheduled only; no accepted implementation evidence. |
| **EXPERIMENTAL** | Code exists and is opt-in; not recommended for production or normal operation. |

## Release identity

| Item | Value |
|---|---|
| Release stage | **Node Beta 1 candidate**; the Beta 1 label is not assigned by this document |
| Build/source tag | `node-beta.1-candidate` — annotated object `fc5174ec…`, tagged commit `ccc4f356…` |
| Release tag | `node-beta.1` — points at the final documentation commit; the tag object is recorded with the release |
| Base upstream | GrinPlusPlus `v1.2.9`, commit `a0958a391c30cf630e3e08534fbc6fe9fb194326` (2024-10-22) |
| `GrinNode.exe` SHA-256 | `4039086CE339106DE37D7C6A98D54873171458699507DB18E938AFD938C80EFC` |
| `Tests.exe` SHA-256 | `A9594508FBDF5B48AA123A862CBB68B20882AD98BB8AA437A9E9A9C60D4FAC8C` — verification-only, not in the user package |
| Candidate source manifest | `1528` files, SHA-256 `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627` |
| Verified platform | Windows 11 x64, MSVC 14.44 (Visual Studio 2022), Release, `/W4 /WX`, static CRT |

A result belongs to the binary and source tree that produced it. All results
below are from the candidate unless marked otherwise. The earlier combined build
`GrinNode.exe` SHA-256 `16C24DB404702595EDB4CEDA7A057D2C187339FB92664473D211DFDB5F3FF3A2`
is cited only where it is the tested artifact.

PIBD segment counts are run-specific: the candidate started at horizon
`3 321 360` and reports `4999/4999`; the earlier combined build started at
`3 320 640` and reports `4998/4998`. The one-segment difference follows from the
different horizon height and is not a defect; counts are never transferred
between builds.

## Node Beta 1 acceptance criteria

The criteria come from the Node Beta dossier. They are repeated here so that
"Beta 1 candidate" is not confused with "Beta 1 accepted".

| # | Criterion | Status | Basis / remaining gap |
|---|---|---|---|
| 1 | Full fresh Mainnet sync to the current height and continued block acceptance on one final SHA, no artificial short peer list | **NOT VERIFIED** | Mainnet has not been run on the candidate. Floonet is not Mainnet. |
| 2 | Full PIBD on one final SHA: segments → validation → activation → catch-up, with negative cases and state preservation | **PASS (scoped)** | Candidate `4039086C…`, fresh isolated Floonet profile, 10 peers: PIBD from `3 321 360` (`17d0428f…`), `4999/4999` segments; `2 948 812/2 948 812` range proofs in `1 215 538 ms` with 1 worker; `Successfully validated TxHashSet` at 00:44:15; KernelIndex generation 2 with `3 357 330` kernels; `PIBD state sync completed and validated` at 00:46:40; catch-up; graceful restart 00:53:13→00:53:21 with `kernel_index_check result=current` (generation 2, indexed height `3 324 510`, `3 360 496` kernels) and **no rebuild**; `FULLY_SYNCED` at height `3 324 513` (network `3 324 513`); graceful stop 00:55:53; `bans_fraud_height=0`, `bans_invalid_data=0`. Negative cases and state-preservation are covered by tests, not by this single live run. |
| 3 | Restarts across sync stages: headers, PIBD download, validation/activation, catch-up; normal and controlled-interruption restart | **PARTIAL** | Restart after a complete sync is PASS (scoped) on the candidate (00:53 restart, `kernel_index_check result=current`, no rebuild) and on the combined build. Restart during PIBD download/validation and a stage-by-stage matrix are NOT VERIFIED. |
| 4 | Validation optimization enabled and proven correct in comparable conditions | **PARTIAL** | Parallel range-proof verification exists; the release default is 1 worker. The full-state 1/2/3/4/auto benchmark and automatic default selection remain OPEN. |
| 5 | Full API consistency with all legacy issues closed, live-verified on one SHA | **NOT VERIFIED** | Registered methods work locally; per-method schema parity, several PIBD/validation status fields and four owner chain-control methods are open. See [API.md](API.md). |
| 6 | Release evidence: exact baseline, frozen sources, build parameters, hashes, sufficient regression | **PARTIAL** | Final source manifest, binaries, full test log, targeted logs, candidate live Floonet PASS (scoped) and a clean-copy rebuild PASS exist. Hosted CI was not executed; Mainnet is not part of this criterion but remains untested. |

## Implemented and verified

| Area | Status | Evidence / scope |
|---|---|---|
| Full automated suite on the candidate | **PASS** | `Tests.exe "~[.]"` (`Tests.exe` SHA `A9594508…`): 373 test cases, 4982 assertions, exit 0, run 2026-11-05 23:50–23:51. Fixture-dependent checks are skipped, not passed. |
| Killed-process recovery of the kernel index | **PASS** | Candidate `A9594508…`: forced kill at a transaction point + `RebuildKillRecovery`, **241 assertions, recovery exit 0**. The earlier combined build `16C24DB4…` had an entry/state harness pair at 219 assertions each. Recovery checks the old tip/count/checksum and absence of partial records. This is a forced kill, not power loss. |
| KernelIndex orphan regression | **PASS** | Defect reproduced before the fix; 17/81 assertions after the fix. |
| KernelIndex after TxHashSet install (synthetic archive) | **PASS** | 25 assertions: install, index at installed tip, restart without rebuild. Not a network PIBD run. |
| Multi-peer PIBD scheduler policy | **PASS** | 68 assertions / 9 cases: peer binding, per-peer/global capacity, send-failure and disconnect release, wrong-peer rejection without segment loss, late/duplicate responses. Live adversarial peers are NOT VERIFIED. |
| BlockPipe local-error classification (no false ban) | **PASS** | 41 assertions / 6 cases. A local exception, including an injected index-commit error, does not ban the peer. Real RocksDB failure is NOT VERIFIED. |
| Live Floonet sync on the earlier combined build | **PASS (scoped)** | `16C24DB4…`: 10 connections (PIBD started with 9 capable peers), headers `0 → 3 324 027`, PIBD `4998/4998` at horizon `3 320 640`, state validation `1987 s`, index generation 2 with `3 356 610` kernels, `fully_synced` at `3 324 064`, restart with current index, final height `3 324 082`. Bound to that binary; the count is not comparable to the candidate's different horizon. |
| API registration and local auth behaviour | **PASS (scope-limited)** | 27 local HTTP/RPC checks on intermediate candidates (`C996B748…`, `36AD1C92…`, `D748B90E…`): status consistency, empty arrays, invalid numbers, owner auth failure, resync, shutdown; owner `ban_peer`/`unban_peer` without a secret fails and IPv6 ban/unban works. Not re-run on `4039086C…`; not full schema parity. |
| Fast local HTTP health | **PASS** | `/v1/health` returned first HTTP 200 at `0.56–0.62 s` while initialization was running; `ready` precedes full sync. Startup-phase evidence is partial: the `starting` state was not always observed under heavy load. |
| Candidate Release build | **PASS (local)** | Release x64 `/W4 /WX`; build log recorded. See [docs/evidence/INDEX.md](evidence/INDEX.md) E-06b. |
| Live Floonet sync on the candidate `4039086C…` | **PASS (scoped)** | Fresh isolated Floonet profile, 10 peers, PIBD from `3 321 360`: `4999/4999` segments, `2 948 812/2 948 812` range proofs (1 worker, `1 215 538 ms`), TxHashSet validated, KernelIndex generation 2 (`3 357 330` kernels), catch-up, graceful restart with `kernel_index_check result=current` and no rebuild, `FULLY_SYNCED` at `3 324 513`, graceful stop, `bans=0`. Evidence: [docs/evidence/files/candidate-live-floonet-2026-11-05.md](evidence/files/candidate-live-floonet-2026-11-05.md) and [pibd-segment-counts](evidence/files/pibd-segment-counts-2026-11-05.md). Scoped to this binary/scenario. |
| Clean-copy rebuild | **PASS** | A clean clone built Release x64 and passed the same full suite (373 cases / 4982 assertions). Clean binaries `GrinNode 73F35D64…`, `Tests 7DE498F5…`; not bit-identical to the candidate (PE timestamps/source path) with identical source, toolchain, imports and tests. Hosted CI was not run. |

## Implemented, partially verified

| Area | Status | What remains |
|---|---|---|
| Multi-peer PIBD download | **PARTIAL** | Normal 9-peer download observed in the combined live run; wrong-segment / adversarial-peer ban behaviour was not tested live. |
| KernelIndex lifecycle | **PARTIAL** | Apply/rewind/orphan/restart and proxy fault injection pass. Real RocksDB read/write/commit failure, post-commit failure and power loss are NOT VERIFIED. |
| Header checkpoint (FastInit) | **EXPERIMENTAL** | Opt-in only, requires an explicit trust anchor (`GRINPP_HEADER_CKPT_BOOTSTRAP=1` plus a published anchor). Reorg, profile-lock and post-kill scenarios are unverified; the measured timings are not a final series. Not part of normal startup. |
| Parallel range-proof verification (G-49) | **PARTIAL** | Works; default is one worker. `GRINPP_RANGEPROOF_WORKERS=auto` is opt-in. A comparable full-state benchmark across 1/2/3/4/auto is OPEN. |
| HTTP/RPC API | **PARTIAL** | All 13 foreign method names and five owner methods are registered; four Rust owner chain-control methods are not implemented; PIBD/validation status fields differ from Rust. See [API.md](API.md). |
| Chain recovery and checkpoints | **PARTIAL** | Atomic `chain.checkpoint`, repair counts and fail-closed paths exist; power-loss and real disk-failure scenarios are NOT VERIFIED. |
| Peer diagnostics and block-sync counters | **PARTIAL** | Counters split deadline expiry from cooldown reassignment; long-run single-peer recovery is NOT VERIFIED. |

## Completed after the initial snapshot

The live candidate run and the clean-copy rebuild finished after the first
documentation snapshot. Their results are recorded in the verified table above;
no claim from them is transferred to another binary.

## Not verified

- **Mainnet:** no run on the candidate. Do not infer Mainnet readiness from Floonet.
- **Power loss:** the kill harness is not a power-loss test.
- **Real storage failure:** real RocksDB write/commit failure, post-commit failure, disk-full.
- **Public reorg:** only synthetic unit/integration reorg scenarios.
- **Live adversarial PIBD peers:** wrong segments, false-ban behaviour at the P2P layer.
- **Automatic range-proof workers:** the candidate live run used the default single worker; `auto` was not exercised on a full live state.
- **FastInit:** not exercised in the candidate live run (no `header_checkpoint_*` events); still experimental.
- **Wallet and UI:** see below.
- **Linux/macOS runtime:** source-compatible, not verified in this fork.
- **Long-duration stability:** multi-day operation and stage-by-stage restart matrix.

## Planned / experimental

| Item | Status | Note |
|---|---|---|
| PIHD header-segment client/server (G-27) | **PLANNED / EXPERIMENTAL** | Client skeleton exists and is off by default; server-side serving and honest capability advertisement are not implemented. Live interoperability is NOT VERIFIED. |
| Chunked KernelIndex rebuild | **PLANNED** | Peak rebuild memory measured at `5 715 MB` for ~3.36M kernels; a chunked design is estimated at ~2.0–2.5 GB but is not implemented. |
| Header validation speedup | **PLANNED** | Measured `57.8–88.0 s` per start on the profiled state; a bounded parallel-chunk proposal exists without weakening integrity checks. |
| Auto worker default | **OPEN** | Default stays 1 until the full-state benchmark exists. |
| Wallet/UI alignment | **PLANNED** | Wallet API and UI are separate acceptance tracks. |

## Wallet and UI

Wallet code is present in the source tree because this is a full Grin++ fork,
but **the wallet is not part of Node Beta acceptance**. Wallet-related changes
in the tree are covered only by unit tests in the automated suite; there is no
live wallet validation, no send/receive acceptance and no recovery acceptance.
The Electron UI is not part of this repository and is **NOT READY**; it must not
be represented as shipped or supported.

## Platform and packaging

- **Verified:** Windows 11 x64, Visual Studio 2022 MSVC 14.44, Release
  `/W4 /WX`, static CRT. **Windows 10 x64 is source-supported but NOT VERIFIED
  here.** The executable links the static CRT (`/MT`) and imports only OS DLLs
  on the tested machine; network-share execution is NOT VERIFIED.
- Prebuilt dependency binaries are not committed to this repository. The build
  guide documents the pinned dependency revisions and the local sysroot route.
- The GitHub Actions workflow is **prepared, not executed** in this fork. Local
  scripts are not hosted-CI evidence.
- **Licensing:** the fork is MIT. Thirty files derived from the Rust `grin`
  5.5.2 node carry comment-only Apache-2.0 attribution headers; the §4(c)
  file-level requirement is **met** (see
  [LICENSES/GRIN-DERIVED-FILES.md](../LICENSES/GRIN-DERIVED-FILES.md) and
  [NOTICE](../NOTICE)). `Logo.png` is **PARTIAL**: upstream asset from initial
  commit `7a4a6619` (2018-12-17, David Burkett), SHA-256 `52658BE6…`, no separate
  upstream licence, retained as an upstream MIT project asset.
- **Packaging:** the user-facing release package contains `GrinNode.exe` and
  documentation, not `Tests.exe`. `Tests.exe` is distributed only under
  `verification/` for reproducibility; it must run with isolated
  `HOMEDRIVE`/`HOMEPATH`/`TEMP` because a run against a real user profile caused
  a `0xC0000409` abort on the preparation machine.

## Evidence

Every claim maps to a recorded artifact in
[`docs/evidence/INDEX.md`](evidence/INDEX.md). Where the original artifact is
local-only, the index records its path and SHA-256; sanitized public copies live
under `docs/evidence/files/`.

---

[Русская версия](STATUS.ru.md) · [Deutsche Fassung](STATUS.de.md)
