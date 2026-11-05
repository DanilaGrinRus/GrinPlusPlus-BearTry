# Known issues

This list is part of the release statement, not a bug tracker. Statuses are
**PARTIAL**, **NOT VERIFIED**, **NOT IMPLEMENTED** or **EXPERIMENTAL**; a bold
**PASS** appears only where the specific scenario is verified.

## Release blockers / acceptance gaps

### KI-01 — Mainnet is NOT TESTED

No Mainnet sync, catch-up or restart was run for the candidate. Floonet results
(including the live combined-build run) do not transfer. Treat Mainnet readiness
as unproven.

### KI-02 — Candidate live run: PASS (scoped), caveats remain

The run on `GrinNode.exe` `4039086C…` reached `FULLY_SYNCED` and stopped
gracefully. It used a fresh isolated Floonet profile with 10 peers; PIBD started
at `3 321 360` (`17d0428f…`), verified `2 948 812/2 948 812` range proofs in
`1 215 538 ms` with 1 worker, validated the TxHashSet at 00:44:15, built
KernelIndex generation 2 (`3 357 330` kernels), and completed PIBD state sync at
00:46:40. After a graceful restart (00:53:13→00:53:21) the index check was
`result=current` (generation 2, indexed height `3 324 510`, `3 360 496` kernels)
with no rebuild; `FULLY_SYNCED` was reached at `3 324 513`. Graceful shutdown at
00:55:53; `bans_fraud_height=0`, `bans_invalid_data=0`.

This is a **PASS scoped to the documented Floonet scenario and this binary**. It
does not cover Mainnet, adversarial peers, power loss, reorg, multi-day stability,
automatic worker mode or FastInit. Evidence:
[docs/evidence/files/candidate-live-floonet-2026-11-05.md](docs/evidence/files/candidate-live-floonet-2026-11-05.md).

### KI-03 — API parity is incomplete

- Four Rust 5.5.2 owner methods are **NOT IMPLEMENTED**:
  `validate_chain`, `compact_chain`, `reset_chain_head`, `invalidate_header`.
  They are not stubbed; calls return `METHOD_NOT_FOUND`.
- During PIBD/validation, status uses C++ fields instead of Rust PIBD leaf and
  phase counters; see [docs/API.md](docs/API.md).
- Peer identity is IP-only, not `SocketAddr`.
- `GET /v1/peers/<ip>` and `lastkernels` changed shape on newer candidates;
  external consumers are not fully re-verified.

### KI-04 — Restart-at-each-sync-stage matrix is NOT VERIFIED

Restart after a complete sync is verified: the candidate restarted at 00:53 with
`kernel_index_check result=current` generation 2 and no rebuild. Restart in the
middle of PIBD download, validation/activation, index build and first catch-up is
not verified. A stage-by-stage matrix is open.

### KI-05 — Storage failure and power loss are NOT VERIFIED

Kill-harness recovery is not power loss. Real RocksDB read/write/commit
failures, post-commit filesystem failure, disk-full and hardware corruption are
unverified. `chain.checkpoint`, DB repair and fail-closed paths are designed for
these cases but not proven under real failure.

## Performance and scale

### KI-06 — KernelIndex rebuild is expensive

A full rebuild of a ~3.36M-kernel state measured ~`122 s` and `5 715 MB` peak
working set. A chunked rebuild (~2.0–2.5 GB estimated) is **PLANNED**, not
implemented. Startup on a stale index can therefore take minutes and use several
GB of RAM.

### KI-07 — Header validation is slow

Header/MMR validation takes `57.8–88.0 s` on the profiled ~3.32M-header state.
An integrity-preserving parallel-chunk speedup is **PLANNED**; FastInit is an
experimental shortcut, not a solution.

### KI-08 — Worker benchmark is incomplete

`GRINPP_RANGEPROOF_WORKERS=auto` is opt-in. The candidate live run used the
default single worker (`1 215 538 ms` for `2 948 812` proofs); `auto` was not
exercised on a full live state. A comparable full-state 1/2/3/4/auto benchmark
has not been run, so the default stays 1. Do not promise a speedup from `auto`.

### KI-09 — Status latency under load is unexplained

`/v1/status` was observed to exceed 5 s once after state validation while
`/v1/health` stayed available. The cause was not established. Read `/v1/status`
with a timeout and treat health as process liveness only.

### KI-10 — Block-sync counters can mislead

`timeouts` can exceed `requests` because a request may be checked and reassigned
more than once. A high counter is not proof of a network fault. Single-peer
long-run recovery is not verified.

## Network and PIBD

### KI-11 — Adversarial PIBD peers are NOT VERIFIED live

Scheduler policy tests cover wrong-peer, duplicate, late, send-failure and
disconnect cases deterministically. Live malicious-segment behaviour and
false-ban behaviour at the P2P layer are not tested.

### KI-12 — Legacy ZIP fallback is not re-verified on the candidate

The fallback path exists and has historical evidence, but the candidate's
fallback under a PIBD-less peer set is not re-verified.

### KI-13 — `Output Commitment not found` during block processing

Some runs logged `GetOutputType: Output Commitment … not found` while processing
a peer block. On the candidate live run these events appeared during catch-up
after PIBD activation and an index rebuild, alongside block-request deadlines;
they were retried, no peer was banned, and the node still reached `FULLY_SYNCED`.
The exact cause of those transient rejections is not established and no
controlled reproduction exists. It is not proof of a consensus defect, but it is
also not claimed as fixed; capture the full log and profile if it blocks progress.

## Experimental / planned features

### KI-14 — FastInit is EXPERIMENTAL

The header checkpoint is opt-in and gated on an explicit trust anchor. The
candidate live run did not exercise it (no `header_checkpoint_*` events). Real
reorg, profile lock and post-kill continuation are NOT VERIFIED. Timings are not
a final controlled series. Do not enable it on a node whose profile you cannot
afford to re-verify.

### KI-15 — PIHD is not implemented as a service

The PIHD client skeleton is off by default; server-side header-segment serving
and honest capability advertisement are **PLANNED**. Live Rust interoperability
is NOT VERIFIED.

## Product scope

### KI-16 — Wallet and UI are NOT READY

Wallet code is in the tree but is not part of Node Beta acceptance; only unit
tests cover it. There is no live send/receive/recovery acceptance. The UI is a
separate repository and is not shipped or supported here.

### KI-17 — Platform coverage is narrow

Windows 11 x64 + MSVC 14.44 is the only verified platform. Windows 10, Linux and
macOS are source-supported but NOT VERIFIED. Network-share execution is NOT
VERIFIED. CI is prepared but never executed on GitHub Actions.

### KI-18 — Builds are not bit-reproducible

A clean-copy rebuild passed the same suite but produced different executable
hashes (`GrinNode 73F35D64…`, `Tests 7DE498F5…`) because of PE timestamps and
source paths. Rebuilding the same source can therefore produce a different SHA;
verify your own artifacts and do not expect `4039086C…` from a local rebuild.

## Reporting

Do not open a public issue for a security problem; follow
[SECURITY.md](SECURITY.md). For functional defects, include the commit, binary
SHA-256, network, profile layout (redacted), exact command, timestamps and the
relevant log excerpt. Never include seeds, private keys or API secrets.
