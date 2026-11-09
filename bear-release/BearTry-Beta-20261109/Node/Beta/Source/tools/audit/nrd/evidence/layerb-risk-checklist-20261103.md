# Layer B (Mainnet readiness) — read-only gap checklist

Status: READ-ONLY GAP REPORT (2026-11-03). Snapshot: build-15
Tests `920F000F…`, GrinNode `A7D28BCC…`; no source/build changes; no access to
line1 profile. Scope: Gate B in `docs/modernization/REMAINING.md`. This lists
only **open gaps / not-verified items**, not closed evidence.

## B1 — G-09 / PIBD: negative wire, resources, concurrency

| # | Gap | Risk | Minimum evidence needed |
|---|---|---|---|
| B1.1 | Negative wire proof/root/segment-ID not tested against a live/public PIBD server | **P1**: accepting malformed proof/root/ID could desync/corrupt state | independent live negative proof/root/ID cases; rejection/no state mutation; logs/SHA |
| B1.2 | Resource limits (RSS/disk/per-connection/global) under concurrent serving not measured | **P1**: memory/disk exhaustion by client(s) | N-client stress with stated limits; peak RSS/disk; fail-closed when limit reached |
| B1.3 | Concurrent serving + single-flight/horizon switch not verified under load | **P1**: inconsistent archive/state or lease violation | multi-client, horizon-switch test; immutable lease; no partial/wrong archive |
| B1.4 | Controlled reorg/horizon switch is BLOCKED (no controlled +720-block source) | **P1** | controlled source or explicitly accepted BLOCKED with deployment impact |
| B1.5 | Public Floonet serving/fallback/restart not verified (ordinary public live is separate) | **P1** | public Floonet serving run; fallback/restart; exact tip/hash/roots; no silent failure |
| B1.6 | Archive readiness now extracts+validates 3 GB on each check (R3) | **P2**: disk/CPU amplification, TOCTOU, startup latency | disk-full/latency/concurrent-recovery test; bounded behavior; no partial publish |

## B2 — Fail-closed and publication

| # | Gap | Risk | Minimum evidence needed |
|---|---|---|---|
| B2.1 | R2 confirmed-vs-flushed invariant not applied/tested (F12 pending) | **P1**: preflight validates a different range than Rewind | F12 applied; mismatch/lag test; no snapshot on mismatch |
| B2.2 | R4 marker-write/disk-full bound not applied/tested (F14 pending) | **P1/P2**: no persistent retry bound if marker unwritable | F14; unwritable marker; skip/no publish; bounded restart |
| B2.3 | Crash matrix beyond one staging kill not verified | **P1**: crash after fsync/before rename, partial final, wrong-root archive, restart loop | crash at each publication phase; old archive preserved; no partial/wrong-root served; marker behavior |
| B2.4 | Existing-archive root validation failure/disk-full path not tested | **P1**: wrong-root archive could suppress/affect regeneration | F13 failure paths; no partial final; regeneration proceeds |
| B2.5 | Rewind fail-closed on other DB corruptions (not just previous header) not covered | **P1/P2**: null/partial maps, missing records, batch rollback | F1–F18 matrix; F11 done; add missing record/header/body rollback |
| B2.6 | `AreSpentPositionsComplete` empty-input/absent-record behavior and exception mapping | **P2**: generic failure/no actionable bound | test absent record for empty/non-empty block; no publish; marker |

## B3 — NRD / crypto parity

| # | Gap | Risk | Minimum evidence needed |
|---|---|---|---|
| B3.1 | Bodyless-NRD test still fails (short-chain v4 contradiction); no equivalent bodyless proof | **P1**: bodyless index/reorg behavior not verified | synthetic bodyless TxHashSet proof (design A) or explicitly accepted gap; no PASS by exception |
| B3.2 | Reorg/restart over genuinely bodyless PIBD state NOT VERIFIED | **P1** | real/synthetic bodyless state reorg; index completeness; fail-closed missing bodies |
| B3.3 | D2 txpool policy has unit tests only; no live pool/network path; index-policy early-return unwired | **P1/P2** | pool integration on testnet; mainnet/off rejection; no pool mutation; Rust-deserializer divergence documented |
| B3.4 | NRD rebuild interruption/kill NOT VERIFIED; RocksDB/DB paths outside ASan | **P1**: crash mid-rebuild could leave partial index | phase-marked kill harness + recovery; complete index; no partial publish |
| B3.5 | Rust differential has not_computed cases; proof/parser coverage subset | **P2** | computed/unpaired categories; address only risk-proportionate cases; no parity overclaim |
| B3.6 | Mainnet NRD block gate tested synthetically only; no mainnet data; legacy schema cleanup/migration open | **P1/P2** | no mainnet data; document synthetic-only; schema cleanup policy |

## B4 — DB resilience

| # | Gap | Risk | Minimum evidence needed |
|---|---|---|---|
| B4.1 | G-23 startup on corrupted HeaderMMR/partial PMMR not fault-injected | **P1**: wrong root/repair or unbootable state | corrupted-header/MMR startup; repair/refuse; no silent accept |
| B4.2 | Crash/kill during NRD rebuild not verified (same as B3.4; DB angle) | **P1** | kill+reopen; no partial index; tip/root stable |
| B4.3 | `Output Commitment not found` not reproduced in 7B; diagnosis pending if observed | **P2** | reproduce/diagnose on isolated copy; order-dependence check; do not claim fixed |
| B4.4 | Schema-v2 legacy keys ignored but not cleaned; downgrade policy absent | **P2** | read-only count; explicit cleanup/downgrade policy; no auto-delete |
| B4.5 | Disk-full/power-loss during checkpoint/MMR repair not in current evidence | **P1/P2** | fault injection on copies; checkpoint/repair consistency |
| B4.6 | Historical Tests.exe `0xC0000409` cause NOT VERIFIED | **P2** | no repeat/new data => accepted known limitation; do not mix with node crash |

## B5 — Release/evidence gaps (not implementation)

- Build-15 is source-bound, but **full suite is 260/259/1 FAIL** (known bodyless
  test) — must remain an explicit exception, not green.
- `full local PIBD 7B` PASS is build-13, not build-15; do not transfer.
- Clean checkout/toolchain/pins and hosted CI or accepted equivalent are not
  evidenced in this line.
- No Mainnet dry-run; no public wallet/UI acceptance.
- ASan coverage excludes RocksDB/DB and F11/F13/D2 paths; do not claim ASan for them.

## Priority (Layer B)

1. **P1**: B1.1, B1.2, B2.1, B2.3, B3.1, B3.4, B4.1.
2. **P1/P2**: B1.3/B1.5, B2.2/B2.4, B3.3, B4.2/B4.5.
3. **P2**: B1.6, B2.6, B3.5/B3.6, B4.3/B4.4/B4.6, B5.

No production code was changed; no live/public or line1 profile was touched.
