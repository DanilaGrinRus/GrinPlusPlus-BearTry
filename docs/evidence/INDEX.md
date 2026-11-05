# Evidence index

This index maps every significant public claim about the
`node-beta.1-candidate` tree to its evidence. Evidence is split into:

- **Public evidence** — sanitized copies published under `docs/evidence/files/`.
  These are the files a reader can open in this repository. Original local
  paths, sizes and SHA-256 are recorded in [ORIGINALS.md](ORIGINALS.md); paths
  and peer addresses were redacted where needed.
- **Local-origin evidence** — the artifact exists in the preparation workspace
  but is not published (large logs, profiles, reports containing local data).
  The claim is still recorded here with the artifact identity and hash so it can
  be re-checked by maintainers.

A skipped test, a missing fixture, a timeout or a mock is **not** a PASS. Never
transfer a result to another binary or commit.

## Verification environment

| Item | Value |
|---|---|
| Platform | Windows 11 x64 |
| Toolchain | Visual Studio 2022, MSVC 14.44 (v143), Release, `/W4 /WX`, static CRT |
| Candidate commit | `node-beta.1-candidate` — annotated tag object `fc5174ec…`, tagged commit `ccc4f356c5f46bd693f1965581c7307d4563d8c9` |
| Candidate `GrinNode.exe` | `4039086CE339106DE37D7C6A98D54873171458699507DB18E938AFD938C80EFC` |
| Candidate `Tests.exe` | `A9594508FBDF5B48AA123A862CBB68B20882AD98BB8AA437A9E9A9C60D4FAC8C` |
| Candidate source manifest | 1528 files, SHA-256 `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627` |
| Evidence snapshot date | 2026-11-05 / 2026-11-06 (Asia/Bangkok) |

## Public evidence files

| File | What it is | Original SHA-256 |
|---|---|---|
| [`files/tests-full-final.log`](files/tests-full-final.log) | Full default test run on the candidate | `EFDD1B9EA8AA6C5C1A5FFFD48B6D06C80A973ADE3B5AA86671028A3E7BF02212` |
| [`files/target-final.json`](files/target-final.json) | Targeted runs: orphan, write faults, TxHashSet install, PibdScheduler, BlockPipe | `C261C79121240951E0C4C29E40B96BA43E7040DB3FBAFB1E110E3F4F5A455C2D` |
| [`files/tests-fullsweep.log`](files/tests-fullsweep.log) | Hidden `FullSweepPpCkpt` run — passes with an empty-fixture warning | `212F3204C8E6A34001884886ABCAC59EEE737D0D8BE4F1B4D29883EB1E8F04BB` |
| [`files/nrd-kill-manifest.json`](files/nrd-kill-manifest.json) | Forced-kill NRD recovery manifest | `6E3F5FEAC79DAF78D627A79C222D11ACB0469D8D19D2A5AD97B508BE1B5AD6CF` |
| [`files/nrd-kill-child.stdout.log`](files/nrd-kill-child.stdout.log) | Killed child test output (tail shows mined blocks) | `FD06037B327CF50C6383BFD837A919F6430C691A660F9233126CD09A62E23725` |
| [`files/nrd-kill-recovery.stdout.log`](files/nrd-kill-recovery.stdout.log) | Recovery run: 241 assertions, exit 0 | `EAF152D52CFD61A90198F7EAF35ABBCF6AEC53CF49537A8FC65A3713A21C1FC0` |
| [`files/build-final.log`](files/build-final.log) | Candidate Release build result (`GrinNode`/`Tests`) | `64644CDD401DABEB3C39EF986684A5DAB321C98E83719D027B473A16D8320178` |
| [`files/configure.log`](files/configure.log) | CMake configure parameters and toolchain | `243CAB979AF339309260C5BA599B761BEE28DC1D1151FBF96A8476D81D7A3038` |
| [`files/combined-live-floonet-2026-11-05.md`](files/combined-live-floonet-2026-11-05.md) | Sanitized live Floonet run on the earlier combined build | extracted from `3734E46E73E204F96996FBAA456DDA4D9FB69BE291D056C62CDCE3EC494AE08B` |
| [`files/candidate-source-manifest-final.sha256`](files/candidate-source-manifest-final.sha256) | Final source file list + hashes (1528 lines) | `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627` |
| [`ORIGINALS.md`](ORIGINALS.md) | Original local paths, sizes, hashes and redaction notes | — |

## Claims

### Build and test suite

| ID | Claim | Status | Evidence | Scenario / command | Limitations |
|---|---|---|---|---|---|
| E-01 | Candidate passes the full default suite: 373 cases / 4982 assertions, exit 0 | **PASS** | [`files/tests-full-final.log`](files/tests-full-final.log) | `Tests.exe "~[.]"` on `A9594508…`, 2026-11-05 23:50–23:51, Windows 11 x64 | Hidden tags and absent external fixtures are skipped; skips are not passes |
| E-02 | Targeted recovery/index/scheduler/block-pipe scenarios pass | **PASS** | [`files/target-final.json`](files/target-final.json) | Explicit filters `KernelIndex …`, `PibdScheduler*`, `BlockPipe*` | Bind to this Tests binary only |
| E-03 | Hidden full-sweep check runs | **PASS with limitation** | [`files/tests-fullsweep.log`](files/tests-fullsweep.log) | `Tests.exe "PibdSegmentService::FullSweepPpCkpt"` | Warning shows `bitmap 0/7, output 0/1678, rangeproof 0/1678, kernel 0/1641` — the fixture is empty; this is **not** evidence of a populated sweep |
| E-04 | Forced-kill recovery of the NRD index | **PASS** | [`files/nrd-kill-manifest.json`](files/nrd-kill-manifest.json), [`files/nrd-kill-recovery.stdout.log`](files/nrd-kill-recovery.stdout.log) | Child killed ~300 ms after a state marker; recovery `RebuildKillRecovery` = 241 assertions, exit 0 | Fault-injected kill, not power loss; NRD index is in-memory and rebuilt from headers+kernel MMR |
| E-05 | Candidate source identity | **PASS** | [`files/candidate-source-manifest-final.sha256`](files/candidate-source-manifest-final.sha256) | 1528 files, SHA-256 `964B2DD8…` | Supersedes the stale `source-manifest.sha256`; see [ORIGINALS.md](ORIGINALS.md) |
| E-06 | Configure parameters match the documented build | **PASS** | [`files/configure.log`](files/configure.log) | `cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DGRINPP_TESTS=ON -DGRINPP_TOOLS=OFF` | Sysroot supplied prebuilt; Windows/MSVC only |
| E-06b | Candidate Release build used for the tests | **PASS (local)** | [`files/build-final.log`](files/build-final.log) | MSBuild Release x64 `/W4 /WX`, `GrinNode` + `Tests` | Clean-clone rebuild was still in progress; hosted CI was not run |

### Sync and live network

| ID | Claim | Status | Evidence | Scenario | Limitations |
|---|---|---|---|---|---|
| E-07 | Live Floonet sync from zero to `fully_synced` on combined build `16C24DB4…` | **PASS (scoped to that binary)** | [`files/combined-live-floonet-2026-11-05.md`](files/combined-live-floonet-2026-11-05.md) | Fresh isolated profile, 10 connections (9 PIBD-capable peers), headers `0 → 3 324 027`, PIBD `4998/4998`, validation ~`1987 s`, index generation 2 `3 356 610` kernels, catch-up `3 324 064`, restarts current, final `3 324 082` | This is **not** the candidate binary. Peer addresses redacted. No Mainnet. |
| E-08 | Live Floonet run on candidate `4039086C…` | **IN PROGRESS / NOT VERIFIED** | No completed public artifact yet | Started 2026-11-05 23:33, restarted 23:52; PIBD `4998/4998`, validation and KernelIndex rebuild done; caught up to network height `3 324 505` at 00:48–00:50; transient `Output Commitment`/deadline events were retried, `bans=0` | Must become PASS only after the Lead confirms `fully_synced` + graceful stop + index check; bind evidence to this binary |
| E-09 | Mainnet synchronisation | **NOT VERIFIED** | — | — | No Mainnet run exists for the candidate; Floonet ≠ Mainnet |
| E-10 | Restart behaviour across sync stages | **PARTIAL** | E-07 (restart after sync) and local reports | Restart after complete sync on the combined build | Restart during PIBD download/validation and a stage matrix are NOT VERIFIED |

### KernelIndex and recovery

| ID | Claim | Status | Evidence | Scenario | Limitations |
|---|---|---|---|---|---|
| E-11 | KernelIndex orphan regression and write-fault behaviour | **PASS** | [`files/target-final.json`](files/target-final.json); local KernelIndex report | Orphan 17 assertions; fault injection 81 assertions | Proxy boundary around real index transaction, not real RocksDB failure |
| E-12 | KernelIndex after TxHashSet install + clean restart | **PASS** | [`files/target-final.json`](files/target-final.json); local report | Synthetic archive install through `ProcessTransactionHashSet`, 25 assertions | Archive install, not live network PIBD |
| E-13 | Index current at the tip after a live run | **PASS (scoped)** | E-07 | Generation 2, `3 356 610` kernels, no rebuild on restart | Combined build, not candidate |
| E-14 | Real RocksDB failure / power loss | **NOT VERIFIED** | — | — | Kill harness ≠ power loss |
| E-15 | Chunked rebuild memory reduction | **PLANNED** | local KernelIndex report | Estimate ~2.0–2.5 GB vs measured `5 715 MB` peak | Not implemented |

### Validation and workers

| ID | Claim | Status | Evidence | Scenario | Limitations |
|---|---|---|---|---|---|
| E-16 | Parallel range-proof validation exists; default is 1 worker | **PARTIAL** | local G-49 / auto-worker reports | Unit + synthetic 10 001-proof benchmark; historical full-state 2/4-worker runs | Full-state 1/2/3/4/auto benchmark OPEN; `auto` is opt-in |
| E-17 | Windows Job Object memory accounting and worker policy | **PASS** | local auto-worker report | Focused tests 41 assertions | Host-specific; thresholds not proven for all machines |
| E-18 | Multi-peer PIBD scheduler policy | **PASS** | [`files/target-final.json`](files/target-final.json) | `PibdScheduler*`: 68 assertions / 9 cases | Deterministic policy only; live adversarial peers NOT VERIFIED |

### API

| ID | Claim | Status | Evidence | Scenario | Limitations |
|---|---|---|---|---|---|
| E-19 | Owner auth, route protection and local HTTP/RPC matrix | **PASS (intermediate binaries)** | local HTTP-check artifacts (27 checks) | `C996B748…`, `36AD1C92…`, `D748B90E…` | Not re-run on `4039086C…`; not full parity |
| E-20 | Four owner chain-control methods implemented | **FAIL / NOT IMPLEMENTED** | code (`NodeServer.cpp`) | `validate_chain`, `compact_chain`, `reset_chain_head`, `invalidate_header` are not registered | Not stubbed with fake success |
| E-21 | Full Rust 5.5.2 API schema parity | **NOT VERIFIED** | [../API.md](../API.md) | PIBD/validation status fields and peer identity differ | Documented as PARTIAL/OPEN |

### FastInit, PIHD, wallet

| ID | Claim | Status | Evidence | Scenario | Limitations |
|---|---|---|---|---|---|
| E-22 | FastInit header checkpoint | **EXPERIMENTAL** | local FastInit status report | Stand field-corruption, MMR corruption, stale fallback, refresh, restart | Merge live scenario, reorg, profile lock, post-kill not verified; timings not final |
| E-23 | PIHD client/server | **PLANNED / EXPERIMENTAL** | local plan | Client skeleton off by default; server not implemented | No live interop |
| E-24 | Wallet and UI readiness | **NOT READY / NOT VERIFIED** | — | Unit tests only for wallet code; UI not in repository | Not part of Node Beta acceptance |

### Performance

| ID | Claim | Status | Evidence | Scenario | Limitations |
|---|---|---|---|---|---|
| E-25 | Startup timing on a profiled state | **PARTIAL** | local KernelIndex report | Health first 200 `0.56–0.62 s`; REST readiness ~65 s; `ready` before `fully_synced` by 19–25 s | Profile-specific; fresh-profile timing on the final SHA NOT VERIFIED |
| E-26 | Header validation time | **PARTIAL** | local report | `57.8–88.0 s` per start on ~3.32M headers | External load affected one measurement; not a controlled benchmark |
| E-27 | Peak rebuild memory | **PASS (measured, scoped)** | local report | `5 715 MB` peak for ~3.36M kernels; `122 194 ms` total rebuild | Single state; reduction is PLANNED |

## Local-origin evidence not published

These artifacts are referenced by the claims above but are not shipped in the
public tree. Their identity and original SHA-256 are in
[ORIGINALS.md](ORIGINALS.md).

| Artifact | Purpose |
|---|---|
| `work/kernel-index-fast-20261105/REPORT.md` | KernelIndex orphan, fault injection, install, measurements, kill harness |
| `work/auto-workers-pibd-20261105/REPORT.md` | Worker policy, Job accounting, benchmark, scheduler |
| `work/combined-kernel-auto-20261105/FINAL_REPORT.md` | Combined live Floonet run, full suite, targeted results |
| `work/header-checkpoint-20261105/FINAL_STATUS_FASTINIT.md` | FastInit status, trust model, unverified scenarios |
| `work/merged-final-20261105/logs/tests-full-final.log` | Full suite (public sanitized copy exists) |
| `work/merged-final-20261105/logs/nrd-kill-evidence/…` | NRD kill/recovery (public sanitized copies exist) |
| `work/node-wallet-api-unified-20261104/…` | API HTTP check artifacts (27 checks) on intermediate candidates |
| `work/merged-final-20261105/build-final.log`, `configure.log` | Candidate build configuration and result |
| `work/merged-final-20261105/runs/live-merged/` | Candidate live run profile and logs (IN PROGRESS; not published) |

## Provenance notes

- The file `work/merged-final-20261105/source-manifest.sha256` is **stale**:
  five files were edited and the binaries rebuilt after it was generated. The
  final identity is `candidate-source-manifest-final.sha256`
  (`964B2DD8…`), used everywhere in the docs.
- The original 2026 development history was a squashed import; commits in this
  fork are reconstructed from the verified final diff. See
  [../HISTORY-RECONSTRUCTION.md](../HISTORY-RECONSTRUCTION.md).- If a public evidence file is replaced, update its hash here and in
  ORIGINALS.md in the same commit.

---

[Русская версия](INDEX.ru.md) · [Deutsche Fassung](INDEX.de.md)
