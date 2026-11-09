# Build-14 fixture drafts: F12, F13, F14 (EVIDENCE ONLY)

Status: fixture/test specifications only. No project source edits, no build.
These tests become applicable **after** the corresponding R2/R3/R4 patches are
applied, because the frozen tree has no test seam for the internal helpers.

## F12 — flushed TxHashSet must equal confirmed tip (R2)

### Setup
1. Create a real `TestServer` and mine a short valid chain (e.g. 5 blocks).
2. Obtain the managed `TxHashSet` and the confirmed tip header.
3. Construct a second valid `TxHashSet` state at a lower header (or replace the
   live manager's state with one whose `GetFlushedBlockHeader()` is the lower
   header) **without** mutating the confirmed chain.
4. Call the preflight/archive entry point that invokes R2's invariant check.

### Required seam
R2 currently checks the invariant directly inside
`StateSyncer::PreflightArchiveRewind`; a test cannot inject a mismatched
`IBlockChain`/`ITxHashSet` without constructing the full syncer. Recommended
small refactor (add to R2 patch before merge):

```cpp
// StateSyncer.cpp / public static for tests
static bool FlushedMatchesConfirmed(
    const BlockHeaderPtr& pFlushed,
    const BlockHeaderPtr& pConfirmedTip,
    std::string* pReason = nullptr);
```

`PreflightArchiveRewind` calls it; the test calls it directly with real and
synthetic headers. This proves the invariant without a live chain.

### Assertions
- same hash+height → true;
- lower flushed height → false, reason mentions mismatch;
- same height/different hash → false;
- null either header → false;
- preflight returns false before any snapshot/archive action (integration
  assertion if the full entry point is used).

## F13 — container-valid wrong-root archive must not suppress regeneration (R3)

### Setup
Reuse `tests/src/PMMR/Test_PibdServing.cpp` helpers `WriteTestState` and
`BuildArchiveZip` (already used by `P2P::ArchivePublicationValidatedAtomicPublish`):
1. Build a valid archive zip and a header matching its roots.
2. Build a second header with the same height/hash fields but a deliberately
   wrong output/range-proof/kernel root (or wrong MMR size).
3. Put the valid zip at the final archive path named for the wrong header
   (container-valid, root-invalid).
4. Call readiness logic used by `TryRecoverArchive`/`ArchivePibdState`.

### Required seam
R3's `IsReadyArchiveForHeader` is static in `StateSyncer.cpp`; expose it as a
testable helper. Recommended:

```cpp
// ArchivePublication.h (or PibdArchiveServer static)
bool ValidateArchiveForHeader(
    const fs::path& archivePath,
    const BlockHeaderPtr& pHeader,
    std::string* pError = nullptr);
```

It performs `IsReadyArchive` + extraction + `PibdArchiveServer::LoadAndValidate`.
`StateSyncer` calls it; tests call it directly.

### Assertions
- valid zip + matching header → true;
- valid container + wrong roots → false with non-empty error;
- valid container + header height/hash mismatch → false;
- after false, recovery path treats archive as unready and proceeds to
  preflight/regeneration (not an early ready return);
- no final archive replacement/partial publish on failure;
- old final archive is not deleted until atomic publish succeeds.

## F14 — unwritable failure marker must bound retries (R4)

### Setup
1. Isolated temp data path with a filesystem object at
   `NODE/TXHASHSET/pibd_state.recovery_failed` that cannot be used as a file
   (e.g. a directory) or a read-only directory.
2. Prepare a marker `pibd_state.complete` and a missing target archive so
   `TryRecoverArchive` would otherwise attempt recovery.
3. Invoke recovery entry point/restart.

### Required seam
R4's `IsRecoveryMarkerWritable` is static. Expose a pure helper:

```cpp
bool ArchivePublication::IsMarkerPathWritable(const fs::path& markerPath);
```

It probes parent creation + tiny write/remove. `RecordRecoveryFailure` remains
best-effort; the preflight probe decides whether recovery may start.

### Assertions
- writable marker path → probe true; existing recovery behavior unchanged;
- directory/unwritable marker path → probe false;
- recovery skipped with explicit warning; no snapshot/Rewind, no final archive,
  no partial file;
- restart repeats the bounded skip (no expensive retry loop);
- after marker path becomes writable, next recovery can attempt once;
- no crash/exception escape.

## Fixture build order

1. Apply R2; add F12 helper + pure test.
2. Apply R3; expose `ValidateArchiveForHeader`; add F13 using existing zip helpers.
3. Apply R4; expose marker probe; add F14 with isolated temp path.
4. Run focused tests normal-only; ASan if line3 schedules; no false ASan claim
   for RocksDB/DB-dependent paths.

Residual: F13 root validation extracts a 3 GB archive; tests should use tiny
synthetic states (existing zip helper) not production archives. F14 cannot
simulate every disk-full race; probe is best-effort.
