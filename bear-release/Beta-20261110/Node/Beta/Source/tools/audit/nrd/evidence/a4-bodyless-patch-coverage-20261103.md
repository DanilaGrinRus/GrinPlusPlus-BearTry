# A4 bodyless patch coverage review (test-only, read-only decision)

Status: **STOPPED BEFORE APPLYING** — patch applies but does not cover the
original failing scenario. No source/build edits, no commit. Task-17.

## Patch

`tools/audit/nrd/evidence/build14-patches/bodyless-index-rebuild.patch`
SHA-256 `A4CDB478EEED31DB24418EE3899B62B582F88783ACB7CCCBBB6CAA88EA378798`
`git apply --check` against current HEAD: **PASS**.

## Original failing test (current source)

`tests/src/BlockChain/Test_NrdBodylessRestart.cpp`:
- real `TestServer` + real `TestMiner` chain;
- removes all block bodies from the real BlockDB;
- reopens the same paths with `TestServer::OpenExisting()` (startup NRD rebuild);
- addresses the intended NRD block, reorgs to an alternative branch, restarts again;
- asserts complete NRD index and persisted state across reopen/reorg/restart.
- Known failure at `pChain->AddBlock(block30)` because header-v4 NRD at height 30
  is rejected by `Consensus::GetHeaderVersion(AUTOMATED_TESTING, 30)==1`
  (v4 starts at 2,102,400). This is a real test-fixture contradiction, not a
  production defect.

## Patch's replacement test

`Bodyless TxHashSet NRD index rebuilds after bodies are removed`:
- synthetic `ChainBuilder`/`FakeBlockDB` only;
- applies NRD blocks at synthetic low height (bypasses BlockValidator);
- `builder.BlockDB()->ClearBlocks()` in memory;
- `builder.Rebuild()` in the same process;
- applies one more block;
- asserts `IsRecentNrdIndexComplete()` and entries.

It does **not** cover:
- real RocksDB/BlockDB persistence;
- `TestServer::OpenExisting()` startup/reopen;
- chain reorg or reorg rollback;
- restart after reorg;
- the original bodyless-real-state semantics.
It replaces/removes the original test and would make the full suite green while
proving a different, narrower property. That would weaken A4 evidence.

## Decision

Per task instruction, **do not apply** this patch as A4 closure. Full suite
remains with the known bodyless exception (build-19 not built).

## Valid closure options (need Lead choice)

1. **Real bodyless restart/reorg test with valid NRD height:** requires a
   controlled real/snapshot chain at height >= 2,102,400 or a test-only consensus
   seam; not a simple test-only synthetic patch.
2. **Real isolated profile/state:** use a copied real node state where NRD blocks
   are valid, remove bodies, reopen/reorg; no line1 public profile.
3. **Split evidence:** keep original failing test as explicit NOT VERIFIED and add
   the synthetic test only as supplementary evidence; do not claim full green.
4. **Accepted exception:** document bodyless restart/reorg as NOT VERIFIED for the
   candidate; do not use synthetic PASS to close A4.

No production semantics, line1 profile, or B3 kill harness touched.
