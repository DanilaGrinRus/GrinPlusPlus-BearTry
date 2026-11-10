# Read-only design: bodyless NRD, external-kill recovery, D2 txpool NRD parity

Status: DESIGN ONLY (2026-11-03). No source edits, build, or commit.
Source tree is frozen for build-13 live acceptance. These are proposals for
build-14+ and require Lead approval.

## 1. Bodyless NRD state: why the current test is structurally impossible

Current test `tests/src/BlockChain/Test_NrdBodylessRestart.cpp` mines 30 real
blocks via `TestMiner`, then adds an NRD block with header version 4 at height
30. `BlockChain::AddBlock` rejects it because
`Consensus::GetHeaderVersion(AUTOMATED_TESTING, 30)` is 1 until
`HARD_FORK_INTERVAL` (= YEAR_HEIGHT/2 = 525,600); version 4 starts at
4*HARD_FORK_INTERVAL = 2,102,400 for non-Floonet environments. A real
`BlockChain` short test chain therefore cannot contain an NRD-bearing block.
This is a test-fixture contradiction, not an NRD or bodyless production defect.
The single full-suite bodyless failure is this test at `AddBlock` (line ~103).

### Design A (recommended): bodyless TxHashSet state via ChainBuilder

Use the existing `tests/src/PMMR/Test_NrdSupport.h` `ChainBuilder`, which writes
headers/blocks into `FakeBlockDB` and applies bodies through the real
`TxHashSet::ApplyBlock` without `BlockValidator`/header-version consensus. It
already supports NRD kernels at low synthetic heights (`Test_NrdKernelRange`,
`Test_NrdIndexRebuild`).

Steps:
1. Build a synthetic header chain with NRD block(s) and a complete
   `recent Nrd` index; verify the index entries.
2. Remove all block bodies from `FakeBlockDB` while keeping headers, kernel MMR,
   output/range-proof MMR and header files (simulated PIBD-like bodyless state).
3. Reopen `ChainBuilder` on the same root; assert startup rebuild produces a
   complete index from headers+kernel MMR only.
4. Apply a new NRD block on the bodyless state; verify new entry and old entry.
5. Reorg to an alternative synthetic branch with more total difficulty; verify
   rollback/rewind and index completeness/correctness.
6. Restart again; verify the complete index and canonical position set.
7. Assert typed failure for a bodyless rewind that would need a missing body
   (block not found / fail-closed), so the test does not claim unsupported
   PIBD-without-bodies reorg parity.

Risks:
- ChainBuilder bypasses `BlockValidator` and real BlockChain header version, so
  this proves TxHashSet/NRD-index behavior, **not** block-consensus acceptance.
  Name/report must state that explicitly.
- FakeBlockDB is not RocksDB; restart/reopen uses files on disk but not real DB
  crash persistence. Keep separate from PIBD/live claims.
- Existing `Test_NrdIndexRebuild` already covers most of this; add only the
  bodyless-removal + reorg/restart delta to avoid duplicate tests.

### Design B (fallback): plain-block bodyless chain test only

Use 30 real blocks without NRD, remove bodies, reopen, reorg/restart. This is
valid on a short chain and verifies bodyless replay/restart invariants, but does
not exercise the NRD index. Acceptable as a bounded bodyless PASS with an
explicit NOT VERIFIED for NRD-in-bodyless.

Rejected design: raising `Consensus::GetHeaderVersion` or TestMiner height to
2.1M; it would change consensus behavior or require an infeasible chain. Do not
add a production version override for tests.

## 2. External kill/recovery harness

Current hidden tests: `RebuildKillChild`, `RebuildKillRecovery`; external
`tools/audit/nrd/rebuild-kill-harness.ps1` kills the child after the state
marker and copies `HOME/.GrinPP/AUTOMATED_TESTING` to `backup`; recovery copies
it back file-by-file. Build-09 failure: `Test_NrdKillHarness.cpp:137
REQUIRE(!error)` during the file-by-file restore. The exact failing path/errno
was not logged, so the harness cannot distinguish a transient Windows copy
error from a semantic state problem. Status remains NOT VERIFIED; no production
evidence of corruption.

### Design: harness-level restore, not in-test file-by-file copy

1. Child writes phase markers: `mining`, `before-reopen`, `after-reopen`,
   `index-complete`, plus expected height. Extend only the hidden test/harness.
2. Harness waits for `before-reopen`/`after-reopen` and kills while the NRD index
   rebuild is in flight, not just arbitrary time after the mining marker.
3. After kill, snapshot the **entire data dir** with `robocopy /E /COPY:DAT` to
   `backup`; preserve `robocopy` exit codes and file counts in the manifest.
4. Recovery should restore at directory level, not per file:
   - remove the fresh test-runtime data dir;
   - `fs::rename(backupDir, dataDir)` when on the same volume (atomic directory
     move, no per-file copy errors);
   - fallback to `robocopy`/`fs::copy` with explicit path+error capture only if
     rename is unavailable.
5. On any copy failure, log source/destination/`error.message()` and fail with
   a diagnostic that says whether state was partial; do not `REQUIRE(!error)`
   without context.
6. After restore, reopen real `TestServer::OpenExisting`; assert:
   - same confirmed tip height/hash;
   - complete NRD index (no partial publication trusted);
   - block bodies required by the test are present;
   - no `.tmp`/staging artifact becomes canonical;
   - second restart remains stable and idempotent.
7. Repeat kill at several delays (e.g., 50/150/300/600 ms) and require at least
   one run whose phase marker proves the kill occurred during reopen/rebuild.

Risks:
- Windows directory rename/handles: child must be fully dead and handles closed;
  antivirus can transiently lock. Use retry with bounded backoff + detailed logs.
- Backup after kill may contain a LIVE RocksDB WAL/LOCK; this is intentional
  crash-state. Restore must preserve it byte-for-byte.
- Tests must remain hidden `[.]` and run only through the external harness;
  normal full suite must not create a false PASS.
- No production hook/flag; all instrumentation is test/harness-only.

## 3. D2 txpool NRD parity (OPEN, no wiring yet)

Rust grin 5.5.2 (`pool/src/transaction_pool.rs:123-136`,
`verify_kernel_variants`):
- if any kernel is NRD and `!global::is_nrd_enabled()` ->
  `PoolError::NRDKernelNotEnabled`;
- else if any NRD and `header.version < HeaderVersion(4)` ->
  `PoolError::NRDKernelPreHF3`.
Rust also rejects NRD kernel deserialization when the flag is off
(`core/src/core/transaction.rs`, `CorruptedData`), and skips NRD duplicate/index
rules when off (`core/src/core/transaction.rs`, `txhashset.rs`).

Grin++ currently has no txpool policy gate:
`src/TxPool/TransactionPoolImpl.cpp::AddTransaction` validates fee/lock/
signature/UTXO but not `Config::IsNrdKernelEnabled(Global::GetEnv())` or the
head header version. Block consensus is already gated in
`BlockValidator::VerifyNrdNetworkGate`; the txpool path remains OPEN/UNWIRED.
C++ `SpentOutputs`/NRD index still processes NRD kernels when policy is off,
which is fail-closed/stricter than Rust but not parity.

### Proposed minimal design (do not implement before approval)

1. Add a pure policy helper (test seam), e.g.
   `TransactionPool::CheckNrdPolicy(kernels, lastConfirmedHeaderVersion,
   environment) -> enum {OK, NRD_NOT_ENABLED, NRD_PRE_HF3}`.
2. In `AddTransaction`, before `TransactionValidator`, call it with
   `Global::GetEnv()` and `lastConfirmedBlock.GetVersion()`.
   - policy off -> `EAddTransactionStatus::TX_INVALID` (maps to
     `EBlockChainStatus::INVALID`; log `NRDKernelNotEnabled`);
   - policy on and header version <4 -> `TX_INVALID` (log `NRDKernelPreHF3`).
3. Do **not** change `TxHashSetImpl` NRD index early-returns in the same patch:
   block gate already rejects NRD blocks on mainnet; changing indexing could
   weaken fail-closed behavior. Record as a separate parity decision.
4. Tests:
   - pure helper: mainnet/off + NRD -> not-enabled; testnet/on + v1 -> pre-HF3;
     testnet/on + v4 -> OK; plain kernels always OK;
   - `TransactionPool::AddTransaction` with synthetic NRD tx on a real
     TestServer testnet chain: pre-HF3 rejected, no pool mutation; policy-off
     seam rejected, no pool mutation;
   - test does not mutate `Global::GetEnv()` if helper accepts environment
     explicitly; avoid cross-test global state;
   - external API mapping remains INVALID; no wallet/network/mainnet data.
5. Risks:
   - Rust rejects NRD tx at deserialization under mainnet, C++ would accept the
     wire object then reject in txpool. Full byte/deserializer parity is a
     larger decision; document divergence, do not silently claim parity.
   - Status enum has no dedicated NRD variants; adding them changes public
     mapping. Minimal patch reuses TX_INVALID; if exact Rust error text is
     required externally, add an enum/status mapping under a separate approved
     change.
   - Global environment mutation in tests can leak; use pure helper/parameter.
   - Mainnet readiness remains NOT VERIFIED; no mainnet data.

## 4. Build/acceptance order after Lead approval

1. Build-13 full 7B live acceptance completes; freeze its evidence.
2. Build-14 (if approved): bodyless ChainBuilder test + kill harness restore
   diagnostics (test/harness-only first); txpool helper/gate only if separately
   approved.
3. Run normal full suite; bodyless must pass; kill harness must record
   killed-phase and recovery PASS; txpool tests normal-only.
4. ASan coverage for new tests depends on line3; RocksDB/DB remains outside
   stated ASan coverage. No ASan claim without evidence.
5. Mainnet/public-PIBD/live claims remain NOT VERIFIED; no mainnet data.

## References

- `docs/modernization/NRD_DIFFERENTIAL.md` §3 D5, §7.2, §8.5, §9.
- `tools/audit/nrd/evidence/isolated/kill-b09-20261103-062035/recovery.stdout.log`
  (line 137 file-copy failure; exact path not logged).
- Rust reference `<workspace>\reference\grin-5.5.2`: pool/src/transaction_pool.rs,
  pool/tests/nrd_kernels_disabled.rs, core/src/core/transaction.rs,
  chain/src/txhashset/txhashset.rs.
- Tests: `Test_NrdBodylessRestart.cpp`, `Test_NrdKillHarness.cpp`,
  `Test_NrdSupport.h`, `Test_NrdIndexRebuild.cpp`, `Test_NrdKernelRange.cpp`.
