# Read-only fail-closed audit: TxHashSet::Rewind + archive preflight

Status: READ-ONLY audit (2026-11-03). No source edits, build, or commit.
Scope: `src/PMMR/TxHashSetImpl.cpp` Rewind, `src/P2P/Sync/StateSyncer.cpp`
TryRecoverArchive/PreflightArchiveRewind, `src/P2P/ArchivePublication.h`
ValidateRewindPreflight/readiness/publish, `src/BlockChain/Processors/BlockProcessor.cpp`
reorg rollback. Source binding: TxHashSetImpl `617AD908…`, BlockDBImpl `F88A107D…`,
StateSyncer `FDC5B4BB…`, ArchivePublication.h `F1E43737…`.

## 1. Confirmed behavior (build-13 verified)

- H=3315600 and H=3313440 fresh-copy recovery PASS; target body not required;
  bodies/spent checked for target+1..flushed; no publication on preflight fail.
- Preflight target hash equality checked against confirmed header at target height.
- `GetSpentPositions` throws when a SPENT_OUTPUTS record is absent; this is caught
  by `ArchivePibdState`, returns false, records `pibd_state.recovery_failed`.
- `TryRecoverArchive` is bounded per process (`m_archiveRecoveryChecked`) and
  persisted failure marker is honored only for the same target hash.

## 2. Risks found (real, not yet patched)

### R1 (P1) Rewind can dereference a null previous header

`TxHashSetImpl.cpp:719`:
```cpp
m_pBlockHeader = pBlockDB->GetBlockHeader(m_pBlockHeader->GetPreviousHash());
```
There is no null check. The loop then evaluates `while (*m_pBlockHeader != header)`,
so a missing/corrupt previous header causes a null dereference instead of a typed
fail-closed exception. Preflight normally prevents this for archive rewind, but
`TxHashSet::Rewind` is also called from `BlockProcessor::HandleReorg` and any
direct caller; DB damage/partial state is exactly the scenario fail-closed must
handle. Expected: throw TXHASHSET_EXCEPTION, rely on batch rollback; no crash.

### R2 (P1) Preflight uses confirmed tip, Rewind uses TxHashSet flushed header

`PreflightArchiveRewind` sets `flushedHeight = GetTipBlockHeader(CONFIRMED)`.
`TxHashSetManager::SaveSnapshot` uses
`m_pTxHashSet->GetFlushedBlockHeader()` as the actual start of `Rewind`
(`m_pBlockHeaderBackup`). These are normally equal after commit, but there is no
checked invariant. If the txhashset flushed header lags confirmed tip, preflight
may check a different range than Rewind traverses; if target is above the actual
flushed header, Rewind walks backwards and may hit a missing previous header
(R1) or fail later. Expected: preflight must compare/use the actual flushed
header, or `SaveSnapshot` must reject target > flushed before copying.

### R3 (P1/P2) Existing archive readiness is container-only

`ArchivePublication::IsReadyArchive` checks that the zip opens and required
entries/legacy leaf prefixes exist, but not roots/MMR sizes/content hashes.
`TryRecoverArchive` treats a container-valid final archive as ready and returns
before preflight/regeneration. `PibdArchiveServer::LoadAndValidate` validates
roots before serving, but a complete yet wrong-root archive can suppress automatic
regeneration and leave serving without a usable horizon. Expected: either
validate the exact header roots before `IsReadyArchive` returns ready, or leave
the failure marker/no-ready state and let serving validation drive regeneration.

### R4 (P2) Failure-marker write failure allows retry on every restart

`RecordRecoveryFailure` swallows marker-write exceptions by design. If the marker
cannot be written (disk full/ACL), each process attempts the same unbounded
recovery once. This is not an infinite loop within one process, but it is not a
bounded-across-restart fail-closed guarantee. Expected: bound/log repeated
failures (e.g., attempt counter) or surface marker-write failure explicitly.

### R5 (P2) Missing SPENT_OUTPUTS record for an empty-input block

`GetSpentPositions` requires a record even for a block with zero inputs; a
compact/bodyless state that omits empty records makes preflight/rewind throw.
This is fail-closed and recorded as failure, not corruption, but the error text
is generic. Expected: preserve fail-closed behavior; test that it produces an
actionable error and no publication/retry loop.

### R6 (P3) Matrix gaps in existing tests

Covered: target==flushed no-op, target body absent, target+1 body missing,
target+1 spent missing, wrong target hash; H=3315600/H=3313440 live-copy.
Not covered: target=flushed-1, target>flushed, missing target header, missing
intermediate header, missing flushed header, target=0/genesis, missing previous
header (R1), actual flushed != confirmed tip (R2), malformed/partial existing
archive (R3), marker-write failure (R4), empty-input block with absent record
(R5).

## 3. Test matrix (proposed, build-14+)

| ID | Scenario | Expected |
|---|---|---|
| F1 | target == flushed, target body absent | preflight true; no-op succeeds |
| F2 | target == flushed-1, only flushed body/spent required | true; target body absent accepted |
| F3 | target > flushed | preflight false, no snapshot/publish; marker |
| F4 | target confirmed header missing | false, no snapshot/publish |
| F5 | target hash mismatch at same height | false, no snapshot |
| F6 | target+1 header missing | false, no snapshot |
| F7 | target+1 body missing | false, no snapshot |
| F8 | target+1 SPENT_OUTPUTS record absent | false, no snapshot, marker written |
| F9 | flushed header missing | false, no snapshot |
| F10 | target=0/genesis, full bodies/spent | preflight true; Rewind safe |
| F11 | Rewind with missing previous header (fake DB) | typed exception + rollback, no null deref (R1) |
| F12 | actual TxHashSet flushed header < confirmed tip | preflight/save rejects or uses actual flushed (R2) |
| F13 | container-valid zip with wrong roots | not treated as ready; no silent suppression (R3) |
| F14 | marker path unwritable | bounded/logged failure; no unbounded restart loop (R4) |
| F15 | empty-input block with absent SPENT_OUTPUTS | fail closed with actionable error (R5) |
| F16 | restart same failure marker | skip same target; different target retired once |
| F17 | publish/rename/fsync failure | final archive absent/old preserved; staging removed |
| F18 | successful H=3313440/H=3315600 | PASS (already build-13 evidence) |

## 4. Recommended order

1. R1 fix + F11 test (smallest, clear null-deref fail-closed defect).
2. R2 invariant check in preflight/SaveSnapshot + F12 test; coordinate line1.
3. R3 readiness/root validation or explicit "container only" policy + F13.
4. R4 bounded retry/marker-write visibility + F14; else document accepted limitation.
5. Fill F1–F10/F15–F17 matrix around build-14; keep normal-only where DB is needed.

No production code was changed. H=3313440/H=3315600 build-13 PASS remains valid.
