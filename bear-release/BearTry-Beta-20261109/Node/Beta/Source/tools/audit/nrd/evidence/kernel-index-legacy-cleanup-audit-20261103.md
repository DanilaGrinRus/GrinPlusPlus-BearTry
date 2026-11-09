# Read-only audit: kernel-index legacy-key cleanup / migration

Status: AUDIT/DESIGN ONLY (2026-11-03). No source edits, build, or commit.
Related: task-9 kernel-index schema v2; current v2 keys are
`KERNELPOS2/<generation>/P|E/...` with state `KERNELIDX2/STATE` in the default
column family. Legacy schema-v1 keys `KERNELPOS/<generation>/<excess>` and
`KERNELIDX/STATE` are ignored by v2 and currently orphaned.

## Evidence already established

- Isolated g09 fixture: 3,354,613 kernel leaves, 3,354,326 unique excesses,
  shortfall 287 (281 duplicates ×2, 3 ×3). Old key model has one
  `KERNELPOS/<gen>/<excess>` key per unique excess, so ~3,354,326 old position
  keys plus one old state key remain orphaned after v2 rebuild.
- v2 count/checksum is over every leaf (`P/<pos><excess>` primary), and the
  legacy keys are not read by `BlockDBImpl` schema v2.
- Code has `ClearKernelPositions(generation)` / `ClearAllKernelPositions()` that
  iterate the **v2** generation prefix only; there is no legacy cleanup API.

## Why not simply delete legacy keys now

1. **Downgrade/rollback compatibility:** an old binary (schema v1) expects
   `KERNELPOS/<gen>/<excess>` and `KERNELIDX/STATE`; deleting while allowing a
   downgrade leaves it with no index and possibly a stale/empty state.
2. **No atomic cross-key deletion:** RocksDB WriteBatch can delete a batch, but
   a crash mid-cleanup leaves a partial old set. This is acceptable only if no
   reader trusts old keys (v2 ignores them), and cleanup is idempotent.
3. **Disk/space:** old keys consume space until compaction; deletion alone may
   not immediately reduce disk. A `CompactRange`/manual compaction step is
   needed and is operationally expensive on mainnet-sized DBs.
4. **State key ordering:** deleting `KERNELIDX/STATE` before positions is safe
   for v2 (ignored), but for downgrade semantics it is worse; delete state last
   if downgrade support is retained.
5. **Generation ambiguity:** old keys are generation-scoped; a cleanup must
   only delete old prefixes, never `KERNELPOS2/` or `KERNELIDX2/STATE`.

## Proposed staged design (build-14+ separate approval)

### Phase 0 — measurement (read-only, safe now)
- Iterate default CF with prefix `KERNELPOS/` (exact prefix, not `KERNELPOS2/`)
  and `KERNELIDX/STATE`; count keys and sum key/value bytes.
- Iterate `KERNELPOS2/` count to prove v2 index is present/complete.
- Record CURRENT/MANIFEST/SST hashes and free-space; do not write.
- Gate: no production change unless v2 state is present and verified.

### Phase 1 — explicit opt-in cleanup tool/path
- Add a **testable maintenance API** (e.g.
  `BlockDB::CountLegacyKernelPositions()` / `ClearLegacyKernelPositions()`),
  not automatic on startup.
- Iterate old prefix in bounded batches; delete keys via WriteBatch; keep
  old `KERNELIDX/STATE` until all position keys are gone, then delete state.
- Idempotent: rerun after crash continues; never touch v2 prefixes.
- Refuse if v2 state is missing/checksum mismatch (avoid deleting legacy data
  when no replacement exists).
- Never run automatically on normal startup; explicit operator/tool action
  only, with dry-run count first.

### Phase 2 — compaction/verification
- Verify v2 count/checksum unchanged after cleanup.
- Optional `CompactRange` for the default CF prefix (expensive; separate
  operational decision); report space reclaimed only after compaction.
- Restart and re-verify index publication; `get_kernel` direct/index path.

### Phase 3 — downgrade policy
- Document that after cleanup, downgrade to schema-v1 binaries is unsupported
  unless they rebuild from chain. If downgrade must remain supported, keep old
  keys or provide an export/rebuild path; do not silently delete.

## Risks

- **P1:** deleting legacy keys while a v1 binary can still open the DB breaks its
  index/serving path; require explicit downgrade policy/approval.
- **P1:** a buggy prefix match could delete `KERNELPOS2/` keys. Use exact
  `GenerationPrefix`/prefix comparison of `KERNELPOS/` and test with v2 keys.
- **P2:** partial cleanup + crash: v2 unaffected, but disk/space remains; make
  idempotent and report incomplete state.
- **P2:** compaction on mainnet DB can take hours and high IO; do not bundle
  with correctness fix.
- **P2:** cleanup tool needs raw DB access; keep it out of normal startup and
  out of consensus/API paths.

## Test matrix

| ID | Scenario | Expected |
|---|---|---|
| K1 | fixture with v1 keys + valid v2 state; count legacy | exact counts/bytes; v2 unchanged |
| K2 | dry-run cleanup | reports keys; writes nothing |
| K3 | cleanup with v2 state missing | refuses; no deletion |
| K4 | delete in batches, verify no `KERNELPOS2/` or `KERNELIDX2/STATE` touched | v2 count/checksum unchanged |
| K5 | crash between batches / restart cleanup | idempotent; v2 usable; no partial publish |
| K6 | state key deleted last | old state absent after positions; v2 state intact |
| K7 | downgrade attempt after cleanup | documented failure/unsupported, not silent corruption |
| K8 | restart + get_kernel duplicate-excess | latest-in-range still correct via v2 index/fallback |

## Recommendation

Keep old keys for now (they are ignored, correctness unaffected). Do a read-only
count/space measurement first; implement cleanup only as an explicit maintenance
tool after build-13/14 live acceptance and an explicit downgrade policy. Do not
auto-delete on startup. The 287-shortfall correctness defect is already fixed by
schema v2; this backlog is storage/operations, not consensus.
