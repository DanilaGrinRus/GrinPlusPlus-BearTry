# Legacy TxHashSet Rust/C++ diagnostic — 2026-11-04

Status: **IN PROGRESS / NOT VERIFIED**. No release or legacy PASS claim.

## Build under test

`<workspace>\kegacy\GrinNode.exe`
SHA-256 `291A75CC70DC69C37D0E7A3F5037F65AC5201DCB4DCEE39BF6949098CB10C8F7`
size 7,861,248 B. Diagnostic build based on `fdb0606`; it intentionally does
not contain the current uncommitted false-ban fixes. Not a release binary.

## Prior live run (old build-33, closed by owner)

PID 3680, `<workspace>\.tmp\build-33\GrinNode.exe`, legacy path:
- Attempt 1, peer 80.228.193.72: start 01:07:10, complete 02:32:12,
  extract 02:32:17, then `Failed to rewind to size 3354438`, peer banned
  `BadTxHashSet`; zip/staging removed by old binary.
- Attempt 2, peer 66.154.127.84: start 02:32:29, complete 04:16:42,
  `Failed to rewind to size 3354438` at 04:16:47; zip/staging removed.
- Attempt 3, peer 195.201.2.51: started 04:17:03; owner closed the old run
  before completion. No captured archive from these attempts.
- Late answers from 103.7.54.154 / 96.44.159.174 / 185.177.219.185 were
  banned `Abusive` while 80 transfer was active (01:08–01:09): confirmed
  overlapping request/response race in old build.

The old error was emitted by `DataFile<114>::Rewind(size)`; `3354438` is in the
same range as the historical kernel leaf count (~3.35M), while output leaves are
an order of magnitude larger. This is evidence for a **kernel-side** rewind, not
proof of a Rust/C++ format defect.

## Kernel format hypothesis (not yet proven)

- C++ `KernelMMR` uses `#define KERNEL_SIZE 114` and
  `DataFile<114>::Rewind(numKernels)`, so a C++ archive must satisfy
  `kernel/pmmr_data.bin == 114 * num_kernels` for fixed-record reads.
- Rust grin v5.5.2 reference: `TxKernel::elmt_size() -> None` (variable-size
  element), PMMR data uses `SizeInfo::VariableSize` plus a `pmmr_size.bin`
  offset/size file; `TransactionKernel` serialization is feature-dependent.
- Rust `file_list(header)` in `chain/src/txhashset/txhashset.rs` lists only
  hash/data/prun/leaf files for the kernel tree, not `kernel/pmmr_size.bin`.
  Therefore actual archive inspection is required before claiming that the
  missing size file or variable kernels caused the rewind error.
- Decision: do **not** attribute failure 3354438 to format mixing until the
  preserved full ZIP is compared to its header and both loaders.

## Diagnostic run (separate from old profile)

- Root `<workspace>\.tmp\legacy-diag-04`; profile, DB, HOME/APPDATA, TMP/TEMP all
  separate; no wallet; PIBD disabled; PIHD off.
- P2P 13618 / peer port 13414; REST 25654; public preferred peers
  and DNS discovery only.
- Diagnostic build capture path:
  `<workspace>\.tmp\legacy-diag-04\temp\legacy-diagnostic-capture\received.zip`.
- At 04:41 header sync is active from peers 185.177.219.185, 66.154.127.84,
  195.201.2.51; no state request/capture yet.
- `received.zip` preservation and SHA are **NOT VERIFIED** until the file
  appears and its central directory is read.

## Source work prepared (not built, no commit)

- Request identity (peer + archive hash + height) recorded by `SyncStatus`;
  current/late/unsolicited classifications; late/unrequested archives ignored,
  no automatic ban.
- One active transfer guard (`IsTxHashSetDownloading`) prevents peer rotation
  from starting overlapping transfers; pipe watchdog uses monotonic 10-minute
  no-progress detection.
- Unique archive temp path per attempt (`txhashset_<short>_attempt_<n>.zip`).
- Failed full archive preserved once as `txhashset_failed.zip` on rewind/load
  failure (bounded one file), with header/height/size log; no live-state change.
- Rewind diagnostics log header hash/height, target kernels/outputs, actual MMR
  sizes and the specific MMR that failed.
- No archive file is copied while being written; capture must occur after the
  transfer closes (or by the diagnostic build itself).

## Required next evidence

1. Wait for diagnostic `received.zip`; verify ZIP central directory and SHA-256.
2. Extract/read `kernel/pmmr_data.bin`, `kernel/pmmr_hash.bin` and any
   `pmmr_size.bin`; compute `data_size`, `114*num_kernels`, serialize version.
3. Run the same ZIP/header through the original 1.2.9 loader and current loader
   in separate profiles; compare failure with exact MMR/file.
4. Controlled A→B→late-A scenario and restart/horizon scenarios on a new build;
   verify attempt isolation, no mixed partial data, no false bans.
5. Build slot for the source changes; attach every result to build SHA.
