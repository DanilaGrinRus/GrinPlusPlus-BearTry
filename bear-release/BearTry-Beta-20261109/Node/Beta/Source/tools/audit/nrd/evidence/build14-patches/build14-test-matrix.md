# Build-14 apply-ready patch drafts and test matrix (EVIDENCE ONLY)

Status: DRAFTS ONLY. No project source was edited; these patches were created in
a temporary git tree and verified to apply sequentially to the current frozen
tree. They are **compile-unverified** (no build was run). R1 is ready to apply;
R2/R3/R4 are code-complete drafts requiring review/tests before build-14.

## Patch files

| Risk | Patch | SHA-256 | Notes |
|---|---|---|---|
| R1 | `r1-rewind-null-header.patch` | `01807920AC15EB3C5A7E20B9931876FA3A022196AD67A8E32353E33F0F8E9DDB` | Fail-closed exception on missing previous header in `TxHashSet::Rewind`; adds F11 test + PMMR CMake entry |
| R2 | `r2-flushed-vs-confirmed-invariant.patch` | `297D7034648AFC3E528C2D509A226685439EC09240D0B4FEFFDC12945CDC1899` | Preflight uses actual `TxHashSet::GetFlushedBlockHeader()` and requires hash/height == confirmed tip; changes preflight method to non-const |
| R3 | `r3-wrong-root-readiness.patch` | `3619CD27AAA1A85EB891D49629C3E6E41E5D15495D2BFC6365637F13849CAD05` | Existing archive must validate exact header roots via extract + `PibdArchiveServer::LoadAndValidate`; container-only readiness no longer suppresses regeneration |
| R4 | `r4-marker-writable-bound.patch` | `B7000057888584544ECD745C411556F1F7B2000962BD1FE36FB66A20A1C05C9A` | Pre-attempt probe: if failure-marker path is not writable, skip recovery rather than retry on every restart without a bound |

All four apply sequentially (R1→R2→R3→R4) with `git apply --check` PASS on a
copy of the frozen tree. Combined diff touches:
`src/PMMR/TxHashSetImpl.cpp`, `src/P2P/Sync/StateSyncer.cpp`,
`src/P2P/Sync/StateSyncer.h`, `tests/src/PMMR/CMakeLists.txt`, plus new
`tests/src/PMMR/Test_RewindFailClosed.cpp`.

## Patch details / caveats

### R1 (P1, ready)
- Replaces the unchecked assignment at `TxHashSetImpl.cpp:719` with a typed
  `TxHashSetException` when the previous header is absent.
- F11 test uses `ChainBuilder`, removes the genesis header from `FakeBlockDB`,
  calls `Rewind` and expects `TxHashSetException`, not a null dereference.
- Touches only Rewind + test; no public API change.

### R2 (P1)
- `PreflightArchiveRewind` becomes non-const (only caller is non-const
  `ArchivePibdState`); it obtains `m_pBlockChain->GetTxHashSet()` and requires
  `GetFlushedBlockHeader()` hash/height == confirmed tip.
- If the live TxHashSet is null or lags/leads confirmed tip, preflight fails
  closed before snapshot/rewind.
- No unit test draft yet: F12 needs a chain-state seam; add integration test or
  a small `PreflightFlushedInvariant` helper test in build-14.

### R3 (P1)
- Adds `IsReadyArchiveForHeader` in `StateSyncer.cpp`: container readiness first,
  then extracts the final archive to a temp dir and validates exact header roots
  via `PibdArchiveServer::LoadAndValidate`. Both `TryRecoverArchive` and
  `ArchivePibdState` use it instead of container-only `IsReadyArchive`.
- Cost: every existing-archive readiness check extracts a ~3 GB zip; this is
  correct but expensive. Follow-up may add a sidecar readiness manifest
  (exact hash + roots + archive SHA) to avoid repeated extraction.
- If validation fails, old archive remains on disk but no longer suppresses
  regeneration; preflight/snapshot proceeds and publish replaces atomically.
- Test F13: container-valid wrong-root archive must not be treated ready and
  must trigger regeneration; add via a temporary archive fixture.

### R4 (P2)
- Adds `IsRecoveryMarkerWritable()` and skips recovery with a warning when the
  marker path cannot be written. This prevents an unbounded restart-retry loop
  when the bound cannot be persisted.
- Probe is best-effort (tiny write+remove); disk-full/permission races after the
  probe remain possible. If the marker still cannot be written after a failure,
  document the residual limitation.
- Test F14: make `pibd_state.recovery_failed` path unwritable/directory, assert
  no snapshot/rewind attempt and no final publication.

## Test matrix

| ID | Scenario | Expected | Draft/owner |
|---|---|---|---|
| F1 | target == flushed, target body absent | PASS no-op | existing line1 test |
| F2 | target == flushed-1, only flushed body/spent | PASS | new preflight test |
| F3 | target > flushed | fail, no snapshot | new preflight test |
| F4 | target header missing | fail, no snapshot | new |
| F5 | target hash mismatch | fail, no snapshot | existing line1 test |
| F6 | target+1 header missing | fail | new |
| F7 | target+1 body missing | fail | existing line1 test |
| F8 | target+1 SPENT_OUTPUTS absent | fail, marker, no snapshot | new integration |
| F9 | flushed header missing | fail | new R2/line1 test |
| F10 | target=0/genesis | preflight/Rewind safe | new |
| F11 | Rewind missing previous header | typed exception, no crash | **R1 test included** |
| F12 | flushed header != confirmed tip | fail before snapshot | R2 integration |
| F13 | container-valid wrong-root archive | not ready; regenerate | R3 integration |
| F14 | marker path unwritable | skip, bounded, no publish | R4 integration |
| F15 | empty-input block missing SPENT record | fail closed/actionable | new |
| F16 | restart same failure marker | skip same target; new target retried once | existing/manual |
| F17 | publish/fsync failure | old archive preserved, staging removed | existing ArchivePublication |
| F18 | H=3313440/H=3315600 fresh rewind | PASS | build-13 evidence |

## Build-14 apply/acceptance order

1. Apply R1 + F11; build normal Tests; run `Rewind fails closed*`.
2. Apply R2; add F12; run archive preflight group + fresh-copy H3313440/3315600.
3. Apply R3; F13 wrong-root fixture; verify no regeneration suppression + no partial publish.
4. Apply R4; F14 unwritable-marker fixture; verify bounded/no publish.
5. Full normal suite; ASan coverage by line3 where applicable; no ASan claim
   for RocksDB/DB paths outside stated coverage.

No source or build changes were made. Build-13 live evidence remains authoritative.
