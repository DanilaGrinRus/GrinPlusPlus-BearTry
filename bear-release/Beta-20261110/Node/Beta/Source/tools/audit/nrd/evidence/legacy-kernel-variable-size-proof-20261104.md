# Legacy kernel MMR format mismatch — raw Rust DB proof

Date: 2026-11-04. Status: **mechanism CONFIRMED from Rust full DB; formal same-ZIP
test still pending**. No release/PASS claim.

## Raw Rust full Floonet DB

Source: `<workspace>\.tmp\legacy-local-01\rust\chain_data\txhashset\kernel`
(full Rust node, chain_type Testnet/Floonet).

| File | Bytes |
|---|---:|
| `kernel/pmmr_data.bin` | 329,241,204 |
| `kernel/pmmr_hash.bin` | 214,694,752 |
| `kernel/pmmr_size.bin` | 33,546,130 |

- `pmmr_size.bin` entries (10 B each) = **3,354,613 kernel leaves**.
- C++ `KernelMMR` fixed record `#define KERNEL_SIZE 114` would require
  **114 × 3,354,613 = 382,425,882 B**, i.e. **53,184,678 B more** than the
  actual Rust data file.
- C++ `DataFile<114>::GetSize()` on the Rust file = floor(329,241,204 / 114) =
  **2,888,080**, not 3,354,613.
- The failed first archive target was `3,354,438`; C++ `DataFile<114>::Rewind`
  attempted `3,354,438 × 114 = 382,405,932 B`, which exceeds the Rust-style
  data file **329,241,204 B**. This is exactly the shape of
  `Failed to rewind to size 3354438` (from `include/Core/File/DataFile.h`).

## Rust source

`<workspace>\reference\grin-5.5.2`:

- `core/src/core/transaction.rs`: `impl PMMRable for TxKernel { fn elmt_size()
  -> Option<u16> { None } }` — variable-size kernel elements.
- `store/src/pmmr.rs`: variable-size backend uses `SizeInfo::VariableSize` with
  `pmmr_size.bin` offset/size entries (`SizeEntry::LEN = 10`).
- `chain/src/txhashset/txhashset.rs` `file_list(header)` lists
  `kernel/pmmr_data.bin` and `kernel/pmmr_hash.bin` but **not**
  `kernel/pmmr_size.bin`; therefore a C++ fixed-record reader has neither the
  record size nor the size index.

## C++ source

- `src/PMMR/KernelMMR.h`: `#define KERNEL_SIZE 114`; `DataFile<KERNEL_SIZE>`.
- `src/PMMR/KernelMMR.cpp`: `Rewind(num_kernels)` rewinds `DataFile<114>` by
  `num_kernels` records.
- `include/Core/File/DataFile.h`: failure message
  `Failed to rewind to size {}` when `AppendOnlyFile::Rewind(size*114)` fails.
- `TransactionKernel::Serialize` uses variable V2 layouts (coinbase 98,
  plain 106, NRD 108, height-locked 114), while the MMR data file is read as
  fixed 114.

## Conclusion / limits

This is sufficient to confirm the **mechanism** of a Rust variable-size kernel
MMR being indexed with C++ 114-byte records, and to explain why a C++ rewind to
3,354,438 can run past the Rust data file. It is **not yet** a same-archive
proof: a full zip with known SHA and header still must be preserved and run
through the loaders. Do not claim legacy PASS or proven sender corruption.

## Same-archive capture pending

- Local Rust full node PID 14956 (`<workspace>\.tmp\legacy-local-01\rust\grin.toml`,
  full chain_data) is serving a C++ build-56 node PID 15424.
- C++ build-56 `<workspace>\.tmp\legacy-local-01\cpp` is doing header sync; no state
  archive yet at the time of this note.
- Diagnostic build `291A75CC…` was restarted but local Rust `peers_allow` only
  admits `127.0.0.1:25836`, so the diagnostic's own 13620 listener was rejected
  at handshake; its own public run was stopped and is not used for this proof.
