# Layer B / B4 — DB resilience fault-injection matrix (read-only)

Status: MATRIX ONLY (2026-11-03). No source/build/deletion; no live or line1
profile. Snapshot: build-15 Tests `920F000F…`, GrinNode `A7D28BCC…`. This maps
REMAINING.md Gate B4 gaps to injection points, expected fail-closed behavior and
minimum evidence. It does not claim any scenario PASS.

## Test seams / constraints

- Prefer copied profiles and test-only fake DB/FS wrappers; no production hook.
- Crash tests use external child kill or a wrapper that throws at a named call;
  never mutate the public/live profile.
- Disk-full: use a constrained temp volume/junction or inject `FILE_EXCEPTION`
  at the documented write call; record exact syscall/error.
- Preserve WAL/LOCK/SST bytes on copy; do not compact/repair source.
- Distinguish: fail-closed refusal vs repair vs silent fallback; a successful
  retry is not a PASS for the original fault.

## Matrix

| ID | Fault | Injection point | Expected fail-closed behavior | Current evidence / status | Minimum missing evidence | Priority |
|---|---|---|---|---|---|---|
| D1 | Corrupted/truncated `header_mmr.bin` | startup, HeaderMMR load/repair | detect invalid index/root; repair from block headers or refuse; no wrong chain | G-23 structural rebuild tested historically; startup corruption NOT verified | corrupt copy at head/middle; restart; exact error/repair; tip/root; no silent truncation | P1 |
| D2 | Partial kernel/output/rangeproof PMMR (`pmmr_data/hash/prun/leafset`) | startup `KernelMMR/OutputPMMR/RangeProofPMMR::Load` | reject incomplete state; repair from block DB / fail closed; never publish partial index | existing PMMR/rebuild unit coverage; file-level corruption restart NOT verified | truncate/remove each file on copy; startup; expected refuse/repair; no serving of partial state | P1 |
| D3 | Kill/crash during startup NRD rebuild | child process kill between index rebuild phases | no partial index published/trusted; reopen rebuilds complete; no unbootable DB | kill harness FAIL/NOT VERIFIED (file restore); build-15 F11 only null header | phase-marked kill during rebuild; recovery complete index; second restart; no partial marker | P1 |
| D4 | Disk full during checkpoint/MMR commit | `ChainState::Commit` flush/checkpoint writes, MMR file append | previous checkpoint retained; no partial state claimed durable; bounded error; no corrupt accepted | fsync order implemented/tested; disk-full NOT verified | constrained copy; fail at each write; restart; old state usable; no checkpoint claiming failed data | P1 |
| D5 | Power-loss/crash between MMR/chain writes and checkpoint rename | `ChainState::Commit` ordering, `DbCheckpoint` rename | load truncates to last valid checkpoint; repair/no silent loss; no unbootable | checkpoint atomicity unit/historical; crash matrix NOT verified | kill at post-MMR/pre-checkpoint and post-checkpoint; restart; tip/root; `db_repaired` semantics | P1 |
| D6 | Missing block body during Rewind | `TxHashSet::Rewind` `GetBlock` null | typed exception, batch rollback, no partial mutation | F11 covers missing previous header; target/body boundary build-13; other missing-body paths NOT all covered | missing body on a rewound block; exception; WAL/DB/tip unchanged; retry safe | P1 |
| D7 | Missing SPENT_OUTPUTS record / empty map | `BlockDB::GetSpentPositions`, preflight | missing record throws; empty record actionable; no publish; marker bounded | build-11 root cause fixed; F13 readiness; F8/F14 pending | absent/empty record at target+1/target; no snapshot; marker/restart behavior | P1 |
| D8 | Corrupt/partial archive during readiness/publish | `IsReadyArchive`/extract/validate/publish | container+root validation; wrong-root not ready; old archive preserved; staging removed | build-13 staging kill one case; R3/F13 normal PASS; wrong-root/live not verified | wrong-root existing archive; crash after fsync/before rename; partial final; retry bound | P1 |
| D9 | `Output Commitment not found` / V3 deserialize order | V3 input deserialization vs creator block apply | diagnostic error; no state mutation; no silent accept | not reproduced in 7B; static order-dependence known | controlled A→B deserialize-before-apply on copy; diagnose; classify | P2 |
| D10 | Historical Tests.exe `0xC0000409` | test runtime/teardown | test-only crash, no production state claim | cause NOT VERIFIED; no repeat | reproduce only with new data or accept as known limitation; do not block node | P2 |
| D11 | Schema-v2 legacy keys/downgrade | maintenance cleanup (not startup) | no auto-delete; explicit tool; v2 state gate; idempotent | read-only count done: 3,354,326 legacy keys, 254,928,776 logical B; cleanup NOT implemented | opt-in cleanup tests; crash mid-batch; v2 count/checksum unchanged; downgrade policy | P2 |
| D12 | Compaction/repair failure | DB repair/compaction | no data loss; report; old state remains | historical repair unit; fault injection NOT verified | inject compaction/repair failure; restart; DB usable; no partial checkpoint | P2 |

## Gaps summary

- No crash matrix for D1/D2/D4/D5/D8; current evidence is one publish staging
  kill and normal FS assumptions.
- D3 kill harness remains NOT VERIFIED for the actual rebuild window.
- D6/D7 fail-closed paths are partially covered (F11/F13); missing-body/empty
  record combinations still need tests.
- D9/D10 are observation-only; do not claim fixed.
- D11 cleanup is read-only counted; no implementation/deletion.
- ASan does not cover RocksDB/DB paths; do not use ASan as evidence for D1–D5.

## Stop rule

Run only risk-proportionate copies; record PASS/FAIL/NOT VERIFIED per fault.
Do not repeat historical kills without a new hypothesis; one successful recovery
does not prove the crash matrix.
