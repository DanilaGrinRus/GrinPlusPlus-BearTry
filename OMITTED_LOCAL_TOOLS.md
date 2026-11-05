# Optional local development artifacts omitted from the public tree

The node and its build sources are included. This file records what is
deliberately **not** part of the public source tree and why. Originals were
not deleted: they remain in the local preparation workspace
(`<private-workspace>` and the `merged-final-20261105` source snapshot).

## 1. Machine-specific audit evidence (excluded by publication decision)

The following raw outputs are tied to one host/workspace and contain absolute
local paths, timestamps and binary hashes from private test runs. They are kept
out of the public repository and are listed in `.gitignore`:

- `tools/audit/nrd/evidence/**` — raw differential/isolated-run dumps,
  sha256 inventories, patch-manifest JSON, local oracle reports.
- `tools/audit/nrd/rust-oracle/**` — the local Rust oracle harness
  (`Cargo.lock`; its `Cargo.toml`/`src/main.rs` were never copied into the
  candidate). The omission does not affect the node build or the standard
  test suite.

The audit **method** stays tracked and reproducible:

- `tools/audit/nrd/README.md`
- `tools/audit/nrd/cases/nrd_cases.json` — loaded at runtime by
  `tests/src/PMMR/Test_NrdDifferential.cpp`
- `tools/audit/check_audit_delta.ps1`, `tools/audit/kernel-index-crash.ps1`

## 2. Local scripts never copied into the accepted candidate

These auxiliary scripts were not part of the accepted source snapshot because
they refer to the private workspace layout. They are not needed to compile the
node or run the standard tests:

```
tools/fuzz/run_fuzz_smoke.ps1
tools/fuzz/run_asan_teardown_regression.ps1
tools/fuzz/gen_corpus.ps1
tools/fuzz/build_linked_targets.ps1
tools/fuzz/build_full_asan.ps1
tools/fuzz/build_asan.ps1
tools/build/run-tests-isolated.ps1
tools/build/ci.ps1
tools/build/ci-regressions.ps1
tools/build/ci-pipeline.ps1
tools/build/build.ps1
tools/audit/nrd/rebuild-kill-harness.ps1
tools/audit/nrd/paired-differential.ps1
tools/audit/nrd/evidence/binaries-sha256.txt
tools/audit/nrd/evidence/rust-oracle-*.json
tools/audit/nrd/evidence/kernelindex-duplicate-excess-g09-20261103.json
tools/deps/build_sysroot.py            (local machine-specific variant)
```

`tools/deps/build_sysroot.py` is replaced by a portable helper in a follow-up
tooling commit after the `node-beta.1-candidate` tag; the local variant is not
published.

## 3. Build outputs and runtime data

Build outputs (`bin/`, `build*/`, `out/`, `output/`), logs, local node/wallet
profiles, `*.api_secret`, seed files, and `dist/data/` are ignored by
`.gitignore` and are not part of the source tree. Upstream `dist/**` binaries
(Tor/pluggable transports) that were already tracked at upstream v1.2.9 remain
untouched under the upstream license audit.
