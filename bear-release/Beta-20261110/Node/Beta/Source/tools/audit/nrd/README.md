# NRD Rust differential harness (Line 2 — Crypto/NRD)

This directory holds the machine-readable NRD (NoRecentDuplicate) differential
cases, the Rust 5.5.2 oracle evidence and the C++ test entry points.

## What is compared

Grin's NRD rules are enforced by:

| Rule | Rust grin 5.5.2 | C++ Grin++ |
| --- | --- | --- |
| relative height (`current - previous >= relative_height`, saturating) | `chain/src/txhashset/txhashset.rs` `apply_kernel_rules`, `MultiIndex::peek_pos` | `include/PMMR/NrdRules.h`, `TxHashSet::ApplyBlock` (`src/PMMR/TxHashSetImpl.cpp`) |
| no duplicate NRD excess in one block/transaction | `core/src/core/transaction.rs` `verify_no_nrd_duplicates` | `NrdRules::RecordBlockKernel` in `ApplyBlock` |
| recent index rebuild from kernel MMR + headers | `TxHashSet::verify_kernel_pos_index` / `init_recent_kernel_pos_index` | `TxHashSet::RebuildRecentKernelIndex` |
| rewind on reorg | `TxHashSet::rewind_single_block` (+ `MultiIndex::rewind`) | `TxHashSet::Rewind` + `RemoveNrdAtOrAbove` |
| header version gate | `core/src/core/block.rs` `verify_nrd_kernels_for_header_version` (`version < 4` = pre-HF3) | `BlockValidator::VerifyKernelFeatures` |
| relative-height bounds | `NRDRelativeHeight::try_from` (1..=WEEK_HEIGHT) | `TransactionKernel::Deserialize` (V2) |

The comparison is not only "accept/reject": every case stores the decision
category, so two `false` outcomes for different reasons are not counted as a
match.

## Categories

| category | meaning |
| --- | --- |
| `accepted` | NRD kernel accepted by the relative-height rule |
| `accepted_non_nrd` | target block has no NRD kernels; NRD rule not applicable |
| `invalid_nrd_relative_height` | duplicate NRD excess inside the recent window violates `relative_height` (Rust `NRDRelativeHeight`) |
| `invalid_nrd_duplicate_in_block` | two NRD kernels with the same excess in one target block (Rust `verify_no_nrd_duplicates`) |
| `missing_local_history` | no complete recent-kernel index: fail-closed local-data category, **not** proof that the remote block is invalid |
| `malformed_input` | bytes fail deserialization (e.g. `relative_height` outside `1..=WEEK_HEIGHT`) |

## Case format (`cases/nrd_cases.json`)

```json
{
  "format_version": 1,
  "network": "AutomatedTesting",
  "header_version_default": 4,
  "cases": [
    {
      "id": "exact-boundary-accepted",
      "mode": "rebuild",
      "blocks": [
        { "kernels": [] },
        { "kernels": [{ "features": "nrd", "excess": 101, "relative_height": 2 }] },
        { "kernels": [] }
      ],
      "target": [{ "features": "nrd", "excess": 101, "relative_height": 2 }],
      "expect": {
        "cpp_category": "accepted",
        "expected_rust_category": "accepted",
        "rust_oracle": "chain/tests/nrd_validation_rules.rs process_block_nrd_validation"
      }
    }
  ]
}
```

* `mode`:
  * `incremental` — blocks are applied through the real `TxHashSet::ApplyBlock`
    on a complete index (normal node path),
  * `rebuild` — block kernels + headers are appended to the kernel MMR and
    header DB **without bodies** (PIBD state), then
    `TxHashSet::RebuildRecentKernelIndex` runs; `rebuild_complete: false`
    asserts the rebuild refused to publish,
  * `no_index` — kernels + headers exist but the index was never rebuilt
    (local incompleteness must fail closed).
* `excess` is a small test tag that maps to a deterministic 33-byte commitment
  (`CBigInteger<33>::ValueOf(tag)`); it is not a valid secp point and is only
  used by the NRD/index code, never by signature verification.
* `relative_height` must be in `1..=WEEK_HEIGHT` (matches `NRDRelativeHeight`).
* `header_version_cases` records the header-version/network feature gate cases
  (HF3 `version < 4`, mainnet NRD flag) that are covered by the C++
  `NrdHeaderVersionGateMatchesRustHf3Rule` test and by the Rust oracle logs.

### Paired Rust/C++ differential (oracle PASS; paired C++ run pending)

`expected_rust_category` is **expectation metadata** derived from the cited Rust
test sources — it is NOT computed by executing Rust on the same input. The C++
runner records `"rust_result_source": "expectation_metadata_not_computed"` when
no oracle is provided and a missing JSON fixture FAILS the test.

`rust-oracle/` is a standalone Rust crate (own `[workspace]`, read-only path deps
into the reference grin 5.5.2 checkout; the reference tree is never modified)
that executes real `grin_chain` code on these cases and emits
`actual_rust_category` (or an explicit `not_computed` reason). Executed result
(2026-11-03): **11 computed / 3 not_computed**; all computed categories come
from real `grin_chain` 5.5.2 `process_block`. Evidence:
`evidence/rust-oracle-8F10BFD6.json` (sha `3FCB0124…DB0`), oracle binary
`evidence/bin/nrd-oracle-615A3B63CECB.exe`, build log
`evidence/rust-oracle-build.log`. A preliminary, explicitly NON-paired sanity
comparison of actual Rust vs C++ expected metadata (11/11 agree) is in
`evidence/rust-oracle-vs-cpp-expectation-8F10BFD6.json`.

Build/run with a separate cargo target — never the project build:

```powershell
$env:CARGO_TARGET_DIR = '<user-home>\AppData\Local\Temp\grin55-target'
cargo build --release --manifest-path tools/audit/nrd/rust-oracle/Cargo.toml
$env:GRINPP_NRD_CASES = '<repo>\tools\audit\nrd\cases\nrd_cases.json'
$env:GRINPP_NRD_RUST_ORACLE_OUT = '<repo>\tools\audit\nrd\evidence\rust-oracle-<sha>.json'
cargo run --release --manifest-path tools/audit/nrd/rust-oracle/Cargo.toml
```

Then run the C++ differential with both `GRINPP_NRD_REPORT` and
`GRINPP_NRD_RUST_ORACLE` set; the runner compares actual Rust vs actual C++,
requires zero mismatches, and only the `computed` subset may be reported as
paired. `no_index` and `rebuild_complete=false` fixtures are intentionally
`not_computed` (Rust has no local-missing-index state; invalid NRD history needs
a crafted-MMR harness). Paired actual-C++ vs actual-Rust mismatch count is
pending the next Tests build slot.

## Running

C++ (needs a Tests build slot, see the Line 2 report):

```powershell
pwsh tools/build/build.ps1 -Target Tests
pwsh tools/build/run-tests-isolated.ps1 -RunId nrd-diff -TestArgs 'Nrd*'
```

Paired Rust/C++ report (uses the existing Tests binary — never builds — and the
already-generated oracle JSON; writes a paired report):

```powershell
pwsh tools/audit/nrd/paired-differential.ps1 -ExpectedTestsSha <tests-sha256> `
    -OracleJson <repo>\tools\audit\nrd\evidence\rust-oracle-8F10BFD6.json
```

It produces `evidence/cpp-nrd-differential-report-paired-<RunId>.json` and
`evidence/paired-differential-report-<RunId>.json` with `computed` /
`not_computed` lists, `paired_cases` and `paired_mismatches`. Only rows with
`paired=true` are a paired Rust/C++ claim; `not_computed` rows carry reasons.

The case file is found by walking up from the working directory until
`tools/audit/nrd/cases/nrd_cases.json` is located; override with
`GRINPP_NRD_CASES`. Set `GRINPP_NRD_REPORT=<path>` to write the per-case JSON
report (both variables are read by the test process).

Rust oracle (read-only, already executed; see `evidence/`):

```powershell
$env:CARGO_TARGET_DIR = '<user-home>\AppData\Local\Temp\grin55-target'
cd <user-home>\AppData\Local\Temp\grin55-minpeers
cargo test --release -p grin_chain --test nrd_validation_rules
cargo test --release -p grin_chain --test store_kernel_pos_index
cargo test --release -p grin_core  --test block
cargo test --release -p grin_pool  --test nrd_kernel_relative_height
cargo test --release -p grin_pool  --test nrd_kernels_disabled
cargo test --release -p grin_pool  --test nrd_kernels_enabled
cargo test --release -p grin_core  --lib nrd
```

## Reference provenance

* Source tree used for the oracle run:
  `<user-home>\AppData\Local\Temp\grin55-minpeers`.
* Canonical reference tree: `<workspace>\reference\grin-5.5.2` (no `.git`).
* `Cargo.toml` version: `5.5.2`.
* Claimed commit: `dbaff34` — **NOT VERIFIED** from independent metadata; the
  reference tree carries no Grin git object database.
* `evidence/grin-5.5.2-reference-sha256.txt` is the sorted per-file SHA-256
  manifest of the canonical tree (391 files). `evidence/rust55-source-sha256-*.txt`
  prove the oracle copy's sources were unchanged before/after the runs and that
  `chain/`, `core/`, `pool/`, `store/`, `util/`, `keychain/` are byte-identical
  to the canonical tree. Only `servers/src/grin/sync/syncer.rs` (another line's
  min-peers patch) and `Cargo.lock` differ; neither is used by the NRD oracle
  tests.

## Files

| file | purpose |
| --- | --- |
| `cases/nrd_cases.json` | machine-readable differential cases (`expected_rust_category` is expectation metadata) |
| `rust-oracle/` | standalone Rust crate executing real grin 5.5.2 code on the cases → `actual_rust_category` / `not_computed` |
| `evidence/rust55-*.log` | Rust oracle test output (release binaries reused, see logs for versions) |
| `evidence/grin-5.5.2-reference-sha256.txt` | canonical reference tree manifest |
| `evidence/rust55-source-sha256-{pre,post}.txt` | oracle source integrity |
| `evidence/bin/` | preserved Tests.exe binaries + `.sha256.txt` manifests bound to the isolated runs |
| `evidence/isolated/<run-id>/` | isolated run stdout + manifest (HOME/TMP isolation, profile unchanged, exit codes) |
| `evidence/cpp-nrd-differential-report*.json` | per-case C++ (and optionally paired Rust) category reports |
| `../../../../tests/src/PMMR/Test_NrdDifferential.cpp` | C++ case runner + header-version + malformed-input tests |
| `../../../../tests/src/PMMR/Test_NrdIndexRebuild.cpp` | rebuild/partial-publication/restart/rewind/rollback tests |
| `../../../../tests/src/PMMR/Test_NrdRules.cpp` | pure rule boundary tests |
| `../../../../tests/src/PMMR/Test_NrdFalseBan.cpp` | block-apply false-ban regression (rebuild-once + non-ban status classification) |

## External audit

line3-fuzz independently audited this handoff: binary SHA-256, manifests, stdout
hashes, isolation and the 14/14 differential categories were confirmed; it
re-ran both suites with `-KeepBinary` on the same binary
(`evidence/line3/nrd-audit-20261103-01/02`) and ASan-instrumented the
header-only `include/PMMR/NrdRules.h` in its own target (26 seeds, smoke PASS).
Audit memo: `evidence/line3/nrd-handoff-audit-20261103.md`.
