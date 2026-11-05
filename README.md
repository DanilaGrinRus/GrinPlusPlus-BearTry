# Grin++ — community fork (Node Beta 1 candidate)

This repository is an **independent fork** of [GrinPlusPlus](https://github.com/GrinPlusPlus/GrinPlusPlus).
It is a community fork, **not** an official release of the upstream project, and is not
maintained by the upstream team. The fork is focused on the **node module first** (GrinNode);
the wallet and UI are **not** part of the Node Beta acceptance.

| | |
|---|---|
| Upstream project | https://github.com/GrinPlusPlus/GrinPlusPlus |
| Base tag | `v1.2.9` |
| Base commit | `a0958a391c30cf630e3e08534fbc6fe9fb194326` (2024-10-22) |
| Project license | MIT (see [LICENSE](LICENSE), upstream copyright preserved) |
| Development stage | **Node Beta 1 candidate** — not a stable release, not Mainnet-ready |
| Build/source tag | `node-beta.1-candidate` — annotated object `fc5174ec…`, tagged commit `ccc4f356…` |
| Release tag | `node-beta.1` — points at the final documentation commit; the tag object is recorded with the release |

## Status at a glance

Read [docs/STATUS.md](docs/STATUS.md) for the full evidence-based status and
[docs/ROADMAP.md](docs/ROADMAP.md) for the remaining work.

| Area | Status |
|---|---|
| Node (Windows 11 x64, MSVC 14.44) | Candidate builds, passes the full test suite and completed a live Floonet sync (`PIBD 4999/4999` segments, then validation, index rebuild and catch-up) to `FULLY_SYNCED` with a current index on restart (**PASS, scoped**; see [docs/STATUS.md](docs/STATUS.md)). |
| Clean-copy rebuild | **PASS** — clean clone builds and passes the same suite; not bit-identical to the candidate (build metadata) |
| Mainnet | **NOT TESTED** |
| Node HTTP/RPC API | **PARTIAL** — registered methods work locally, full Rust parity is not claimed |
| Wallet | **NOT READY** — included from upstream with some fixes, not accepted for this release |
| UI | Not in this repository; **NOT READY** |
| FastInit header checkpoint | **EXPERIMENTAL**, opt-in only |
| PIHD header sync extension | **PLANNED / EXPERIMENTAL**, disabled by default |
| Multi-peer PIBD scheduler | Implemented and deterministically tested; live multi-peer PIBD verified in the combined run |
| Automatic range-proof workers | Opt-in (`GRINPP_RANGEPROOF_WORKERS=auto`); default remains 1 |

**Node Beta is not whole-product readiness.** A Floonet PASS is not a Mainnet PASS.
No result is transferred to another binary: every claim in the documentation is
bound to the commit and executable SHA listed in [docs/evidence/INDEX.md](docs/evidence/INDEX.md).

## Candidate identity

Final candidate source tree and binaries for this documentation snapshot:

| Artifact | SHA-256 |
|---|---|
| `GrinNode.exe` | `4039086CE339106DE37D7C6A98D54873171458699507DB18E938AFD938C80EFC` |
| `Tests.exe` | `A9594508FBDF5B48AA123A862CBB68B20882AD98BB8AA437A9E9A9C60D4FAC8C` |
| Candidate source manifest (1528 files) | `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627` |

The source/build tag is `node-beta.1-candidate`; the release tag is
`node-beta.1`. Rebuilding the same source does not reproduce a bit-identical
executable automatically; build results can differ by toolchain, SDK and paths.
Run `sha256sum` on your own artifacts and record them.

`Tests.exe` is **not part of the user-facing release package**. It is provided
separately under `verification/` for reproducibility only, because a test run
against a real user profile caused a `0xC0000409` abort on the preparation
machine. If you run it, use an isolated `HOMEDRIVE`/`HOMEPATH`/`TEMP`; do not
assume it is safe against a live user profile.

## Verified platform

- **Verified:** Windows 11 x64, Visual Studio 2022 MSVC 14.44, Release,
  `/W4 /WX`, static CRT. All PASS results in this repository were produced on
  this platform.
- **Source-supported, not verified here:** Windows 10 x64; Linux x64 with GCC.
  The upstream configuration also built macOS with GCC; Clang is rejected by
  `CMakeLists.txt`. No Windows 10, Linux or macOS runtime result is claimed.

## Build, test, run

Prerequisites, pinned dependencies and exact commands are in
[docs/BUILD.md](docs/BUILD.md).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=<prebuilt-sysroot>" `
  -DGRINPP_TESTS=ON -DGRINPP_TOOLS=OFF
cmake --build build --config Release --target GrinNode Tests --parallel

# Run a headless Floonet node without a wallet
.\bin\Release\Release\GrinNode.exe --floonet --headless --no-wallet
```

Without arguments the node defaults to **Mainnet** and starts the wallet. For
any test, use an isolated profile (`--config-path` and a `DATA_PATH`) and never
point it at a profile that holds real funds.

Default locations: profile `%USERPROFILE%\.GrinPP\<NETWORK>`, config
`%USERPROFILE%\.GrinPP\<NETWORK>\server_config.json`. REST/P2P ports are
3413/3414 on Mainnet and 13413/13414 on Floonet by default.

## Documentation

- [docs/STATUS.md](docs/STATUS.md) — what is implemented, verified and open, with
  PASS / PARTIAL / FAIL / NOT VERIFIED / PLANNED labels.
- [docs/ROADMAP.md](docs/ROADMAP.md) — remaining work and exit criteria.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — node structure, sync, storage,
  KernelIndex, FastInit trust model and on-disk layout.
- [docs/API.md](docs/API.md) — node HTTP/REST and JSON-RPC surface, auth, status
  and known differences from Rust Grin 5.5.2.
- [docs/BUILD.md](docs/BUILD.md) — clean build, tests, run, common errors.
- [docs/history/FROM-UPSTREAM-TO-CURRENT.en.md](docs/history/FROM-UPSTREAM-TO-CURRENT.en.md)
  — what changed since upstream v1.2.9, why, and what evidence supports it.
- [docs/evidence/INDEX.md](docs/evidence/INDEX.md) — claim → evidence map.
- [KNOWN_ISSUES.md](KNOWN_ISSUES.md) — known limitations and open defects.
- [CHANGELOG.md](CHANGELOG.md) — release-oriented summary of changes.
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to contribute and how evidence is
  required.
- [SECURITY.md](SECURITY.md) — responsible disclosure and safe-testing rules.
- [docs/HISTORY-RECONSTRUCTION.md](docs/HISTORY-RECONSTRUCTION.md) — how this
  repository's commit history was reconstructed.

Russian and German translations are published next to the canonical English
documents (`*.ru.md`, `*.de.md`). If a translation and the English text differ,
the English text is authoritative.

## Testing

The candidate full suite passes (`373` test cases, `4982` assertions, exit 0).
`Tests.exe` is verification-only and is **not** shipped in the user-facing
release package. Run it with an isolated `HOMEDRIVE`/`HOMEPATH`/`TEMP`; a run
against a real user profile caused a `0xC0000409` abort on the preparation
machine, so it must not be treated as safe against a live profile. Some checks
need external fixtures or explicit tags and are **skipped**, not passed; see
[docs/BUILD.md](docs/BUILD.md) and [docs/STATUS.md](docs/STATUS.md) for the
exact list.

## Safety

This is development software for test networks. Do not use it with real funds.
Do not publish seed phrases, private keys, API secrets (`.api_secret`) or personal
profile data. See [SECURITY.md](SECURITY.md).

## License and notices

The fork is MIT — see [LICENSE](LICENSE); upstream copyright is preserved.
Thirty files derived from the Rust `grin` 5.5.2 node carry comment-only
Apache-2.0 attribution headers; the canonical list is
[LICENSES/GRIN-DERIVED-FILES.md](LICENSES/GRIN-DERIVED-FILES.md), with the full
Apache-2.0 text in [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) and the
component notice in [NOTICE](NOTICE). The Apache-2.0 §4(c) file-level
attribution requirement is **met**. Bundled dependencies and their notices are
inventoried in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), with full
license texts in [LICENSES/](LICENSES/README.md).

`Logo.png` is **PARTIAL**: it is the upstream project asset from initial commit
`7a4a6619` (2018-12-17, David Burkett), SHA-256 `52658BE6…`, with no separate
upstream license. It is retained as an upstream MIT project asset with the
upstream copyright; no third-party license text is asserted for it.

---

English is the canonical language. [Русская версия](README.ru.md) ·
[Deutsche Fassung](README.de.md)
