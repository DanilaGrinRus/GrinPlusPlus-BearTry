# Changelog

All notable changes to this fork are documented here. The format follows
"Keep a Changelog" loosely; the project does not yet use semantic versioning for
the fork (the upstream engine version remains `1.2.9`).

## [Node Beta 1 candidate] — 2026-11-06

A candidate snapshot, not an accepted Beta 1 and not a stable release. Bind all
results to the commit `node-beta.1-candidate` and to the executable hashes in
[docs/STATUS.md](docs/STATUS.md).

### Added

- PIBD state sync: segment models/messages, download, validation, staged MMR
  assembly, atomic activation with crash marker recovery, resume and peer
  rotation; legacy `txhashset.zip` remains as fallback.
- Multi-peer PIBD scheduler with per-peer/global in-flight limits, send-failure
  and disconnect release, wrong-peer rejection, late/duplicate response policy.
- Persistent, tip-bound KernelIndex for `get_kernel`, with apply/rewind in the
  chain write batch, bounded incremental extension and full-rebuild fallback;
  orphan repair after commit.
- KernelIndex fault-injection tests at the transaction boundary and a
  forced-kill recovery harness.
- Range-proof batch verification with explicit worker counts and an opt-in
  `GRINPP_RANGEPROOF_WORKERS=auto` policy (CPU affinity, physical cores,
  available memory, Windows Job Object limits). Default remains 1.
- Early `/v1/health` (`starting`/`ready`) before the chain is open.
- Node API authentication: 64-hex owner-only `.api_secret`, Bearer /
  `X-API-Secret`, PUBLIC/PRIVATE/ADMIN enforcement, loopback Host/Origin checks,
  protected shutdown/resync/config/peer operations.
- Owner `ban_peer`/`unban_peer`; DB-only unban.
- Rust-compatible `Status` object under `/v1/status.rust` and owner
  `get_status`, driven by one live snapshot builder.
- `get_config` (foreign and owner) and transactional `update_config` (owner) as
  C++ extensions.
- Experimental, opt-in FastInit header checkpoint (`HeaderStateCheckpoint`) with
  full-verification fallback and trust-anchor-gated creation.
- Documentation in English with Russian and German translations; evidence index.
- `THIRD_PARTY_NOTICES.md` and preserved upstream MIT license.

### Changed

- Header requests are paced (one per 200 ms); abuse rate limiting counts inbound
  messages only.
- Block-sync bookkeeping distinguishes real deadline expiry from cooldown
  reassignment; retries and requests are separate counters.
- `GET /v1/peers/<ip>` returns an object and `lastkernels`/`lastoutputs`/
  `lastrangeproofs` return `{hash: …}` objects on newer candidates (intentional
  schema change; external consumers may need adaptation).
- Kernel parsing and PMMR loading handle variable-size Rust v2 kernel records.
- DB recovery/repair is exposed through `db_repaired` and `db_last_repair`
  counters (see the API doc for exact names used).

### Fixed

- Header-download ban caused by counting the node's own outbound messages.
- Orphan block could commit while the kernel index append was refused.
- Manual unban did not clear peers present only in the database.
- Chain/DB inconsistency on crash could publish partial state; reorg preflight
  now refuses before mutation when confirmed bodies are missing
  (`LOCAL_REWIND_DATA_MISSING`).
- Chain-store writer deadlock fixed; selected API/DB lock-ordering paths were
  reworked. A full concurrency proof (TSan/ABBA matrix) is not claimed.
- Config update could leave runtime and disk divergent; update is now
  validate → persist → apply.
- Missing API authentication on owner endpoints and `/v1/shutdown`.

### Security

- The API secret is generated with OS randomness, stored owner-only, checked
  with constant-time comparison, never logged and never placed in URLs.
- Admin operations require the secret and a loopback Host/Origin.
- Never run this build on Mainnet with real funds.

### Known limitations

See [KNOWN_ISSUES.md](KNOWN_ISSUES.md) and [docs/STATUS.md](docs/STATUS.md).
Highlights: Mainnet NOT TESTED; the live run on the final candidate is IN
PROGRESS; four Rust owner chain-control methods are NOT IMPLEMENTED; full API
parity and restart-at-each-stage are NOT VERIFIED; real RocksDB failure and
power loss are NOT VERIFIED; FastInit is EXPERIMENTAL; wallet/UI are not part of
this release.

---

Older candidate snapshots (RC1/RC2 and intermediate builds) are historical and
are not releases of this fork. Their numbers are not carried forward to this
tag.
