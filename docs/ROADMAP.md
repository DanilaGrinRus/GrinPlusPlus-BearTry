# Roadmap

The roadmap is ordered by what blocks an honest **Node Beta 1** acceptance and
then by product work. Statuses use the same vocabulary as [STATUS.md](STATUS.md).
Dates are not promised; each exit criterion is evidence-based.

## Phase 0 — freeze and evidence (current)

Goal: make the candidate's identity and evidence unambiguous.

| Item | Status | Exit criterion |
|---|---|---|
| Freeze source and binaries | **PASS** | Source manifest `964B2DD8…` (1528 files); `GrinNode` `4039086C…`; `Tests` `A9594508…` |
| Full test suite on the candidate | **PASS** | 373 cases / 4982 assertions, exit 0 |
| Targeted index/scheduler/block-pipe tests | **PASS** | `target-final.json` all PASS |
| Forced-kill NRD recovery | **PASS** | 241 assertions, recovery exit 0 |
| Stale-manifest cleanup | **PASS** | Only the final manifest is cited in docs |
| Commit history reconstructed | **PASS** | `node-beta.1-candidate` (annotated tag object `fc5174ec…`) points at commit `ccc4f356…`; tree verification PASS |
| Clean-copy rebuild | **PASS** | Clean clone built Release x64 and passed the same suite (373/4982); clean binaries `73F35D64…`/`7DE498F5…`, not bit-identical to the candidate by design of the build metadata |

## Phase 1 — live candidate verification

Goal: a completed live Floonet run bound to the candidate binary.

| Item | Status | Exit criterion |
|---|---|---|
| Candidate live Floonet sync | **PASS (scoped)** | Fresh isolated profile: headers → PIBD → validation → catch-up → `fully_synced`; evidence bound to `4039086C…` |
| Restart after full sync | **PASS (scoped)** | Restart 00:53:13→00:53:21 kept generation 2, `kernel_index_check result=current`, no rebuild; continued to `FULLY_SYNCED` at 3,324,513 |
| Restart during PIBD download | **NOT VERIFIED** | Documented stage; state preserved or safely restarted |
| Restart during validation/activation | **NOT VERIFIED** | Marker recovery path exercised; no partial live state |
| Restart during index build / first catch-up | **NOT VERIFIED** | Continuation or safe rebuild; no data loss |

## Phase 2 — node API acceptance

Goal: a documented, tested contract for the slice that is published.

| Item | Status | Exit criterion |
|---|---|---|
| Auth/route matrix on one SHA | **PARTIAL** | The 27-check local matrix re-run on the candidate; results bound to the binary |
| Per-method schema comparison with Rust 5.5.2 | **NOT VERIFIED** | Parameter/result/error table for every registered method |
| Owner chain-control methods | **NOT IMPLEMENTED** | Implement on a real backend with security review and tests, or keep them explicitly unsupported |
| PIBD/validation status fields | **PARTIAL** | Real phase counters instrumented, or the difference documented permanently |
| Peer identity parity (IP vs SocketAddr) | **PARTIAL** | Contract decision documented and tested |

## Phase 3 — resilience

Goal: prove behaviour under failure, not just normal operation.

| Item | Status | Exit criterion |
|---|---|---|
| Real RocksDB read/write/commit failure | **NOT VERIFIED** | Fault injected at the real DB boundary, not a proxy |
| Post-commit filesystem failure | **NOT VERIFIED** | No partial publication; restart consistent |
| Power loss | **NOT VERIFIED** | Repeated sudden-power-off runs on a test profile |
| Disk full / read-only profile | **NOT VERIFIED** | Fail closed with a clear error |
| Public reorg (Floonet) | **NOT VERIFIED** | Controlled alternative branch on a real network profile |
| Adversarial PIBD peers | **NOT VERIFIED** | Wrong segments, wrong peer, late/duplicate, no false ban live |

## Phase 4 — performance

Goal: decide defaults with measurements, not guesses.

| Item | Status | Exit criterion |
|---|---|---|
| Full-state 1/2/3/4/auto range-proof benchmark | **OPEN** | Same state, repeated runs, CPU/RSS/time recorded; choose a default |
| Header validation speedup | **PLANNED** | Integrity-preserving chunked verification; corruption cases still fail |
| KernelIndex chunked rebuild | **PLANNED** | Measured peak reduction; crash between chunks; digest mismatch |
| Startup timing on a fresh final-SHA profile | **NOT VERIFIED** | Repeatable series with host load recorded |

## Phase 5 — FastInit decision

| Item | Status | Exit criterion |
|---|---|---|
| Reorg on a valid alternative branch | **NOT VERIFIED** | Checkpoint either stays valid or falls back safely |
| Profile lock | **NOT VERIFIED** | Second instance fails with a clear error |
| Post-kill continuation | **NOT VERIFIED** | Next start recovers consistently |
| Final timing series | **NOT VERIFIED** | Controlled runs, no background load |
| Decision | **EXPERIMENTAL** | Promote, keep opt-in, or remove — with evidence |

## Phase 6 — PIHD (planned extension)

| Item | Status | Exit criterion |
|---|---|---|
| Client hardening (off by default) | **PARTIAL** | Timeout, late/wrong response, fallback, no false ban |
| Server header segments + honest capability | **PLANNED** | Rust staging live loopback in both directions |
| Runtime enable/disable via owner API | **PLANNED** | No restart required; documented contract |

## Phase 7 — Mainnet gate

| Item | Status | Exit criterion |
|---|---|---|
| Mainnet fresh sync | **NOT VERIFIED** | From zero to current height on one final SHA |
| Mainnet catch-up and restart | **NOT VERIFIED** | Continues accepting blocks; restart keeps state |
| No artificial short peer list | **NOT VERIFIED** | Normal discovery; safe configurable connection limit |
| Release evidence pack | **PARTIAL** | Every claim bound to commit + SHA + scenario |

## Separate tracks (not Node Beta)

- **Wallet:** live send/receive/recovery acceptance, key handling, migration and
  security review. Currently **NOT READY**.
- **UI:** alignment with the actual backend contract, process/profile safety and
  end-to-end acceptance in a separate repository. Currently **NOT READY**.
- **Flint/messenger/own-network research:** design only; not part of this
  repository's release.

## Stop rule

Do not chase ever-rarer defects indefinitely. When the checks for a specific
mode are sufficient, record PASS/FAIL/NOT VERIFIED, the residual risk and the
candidate. Never mix wallet data, ports, live copies or build directories
between parallel runs. A hash or a mock never replaces a live check, and no
secret, seed or private key may appear in reports or logs.
