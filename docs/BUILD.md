# Build, test and run

This is the canonical build guide. It applies to the source tree tagged
`node-beta.1-candidate`. The root [`BUILD.md`](../BUILD.md) is a pointer to this
file.

## 1. Prerequisites

**Verified toolchain (all PASS results in this repository):**

- Windows 11 x64.
- Visual Studio 2022 MSVC 14.44 (v143) with a Windows 10/11 SDK.
- CMake 3.20 or newer (the project declares 3.8, but the VS 2022 generator is
  what was tested).
- PowerShell 7 or Windows PowerShell 5.1.
- Python 3.10+ *only* for the dependency-sysroot helper (optional).
- At least 16 GB RAM for the build; the node itself peaks at roughly
  `2.4–2.5 GB` working set on normal startup and up to `5.7 GB` during a full
  kernel-index rebuild on a ~3.36M-kernel state.

**Supported by the source, not verified in this fork:** Windows 10 x64 and
Linux x64 with GCC. `CMakeLists.txt` rejects Clang, so macOS requires a GCC
toolchain and is not verified here either. Do not present Windows 10, Linux or
macOS binaries as tested.

## 2. Dependencies

The project links a static CRT (`/MT`) and expects prebuilt libraries with CMake
package configs. Two routes are available.

### 2.1 Prebuilt sysroot (the route used for the candidate build)

The candidate was built against a local Windows x64 sysroot assembled from the
pinned revisions below. Prebuilt binaries are **not** committed to this
repository (about 162 MB). Obtain the sysroot from the release package or build
one with the same pins. The reference sysroot used for the candidate has 746
files and 162,287,187 bytes; its manifest SHA-256 is
`82FBF0F7F2D5E5BFFB8C049479E677A10F47177E6A55459269960EAD54EC5C53`.
Use that manifest as the integrity anchor when receiving a prebuilt bundle from
someone else.

```powershell
# The portable helper (tools/deps/build_sysroot.py) is published as a reviewed starting point.
python tools/deps/build_sysroot.py --dry-run   # inspect first
python tools/deps/build_sysroot.py
# Then pass the resulting prefix:
#   -DCMAKE_PREFIX_PATH=<absolute path to sysroot>
```

**Dependency rebuild status (verified 2026-11-06):** the portable helper was fixed
and executed end-to-end on Windows 11 x64 with VS 2022 BuildTools. It exits 0 and
installs 743 files; manifest SHA-256
`0373CBD068518863DC7B16CB3DB56B6317F7682B7E8FE5B06549176B5E5EFA30`. A clean project
build against this rebuilt sysroot compiled and passed the full suite (373 cases /
4982 assertions). The rebuilt sysroot is functionally equivalent but **not
byte-identical** to the published 746-file sysroot: 730 files identical, 13 compiled
artifacts differ by build timestamps/paths, and 3 published files are absent (the
unused `include/mio/mio.hpp`; RocksDB configs installed to the standard
`lib/cmake/rocksdb`). Two helper defects were fixed: missing
`CMAKE_INSTALL_PREFIX`/`CMAKE_PREFIX_PATH`, and `--toolchain-dir` being misused as
the Visual Studio path.

If the helper is not present, reproduce the pins from the table below. A sysroot
manifest with SHA-256 values is published with the release evidence; use it as
the integrity anchor when receiving a prebuilt bundle from someone else.

### 2.2 vcpkg (upstream route)

The upstream project used vcpkg commit
`6185aa76504a5025f36754324abf307cc776f3da` (2020.06) with the overlay ports and
triplets in `vcpkg/` and the package list in `vcpkg/packages.txt`. This route
predates VS 2022; it is documented for compatibility but **not verified with the
VS 2022 toolchain used here**.

### 2.3 Pinned revisions

| Dependency | Revision |
|---|---|
| zlib / minizip | 1.2.11 |
| libsodium | 1.0.18 |
| secp256k1-zkp | `5b39fbf7f1e16e126404791923511723c4c0876d` |
| RocksDB | 6.6.4 (`551a110918493a19d11243f53408b97485de1411`) |
| civetweb | `2c1caa6e690bfe3b435a10c372ab2dcd14b872e8` |
| CRoaring | 0.2.66 |
| fmt | 6.2.1 |
| asio | 1.12.2 |
| mio | `cafa31360fee8866be89b4c602d8b9a7a18dbf5e` |
| Catch2 | 2.12.1 |
| vcpkg (upstream pin) | `6185aa76504a5025f36754324abf307cc776f3da` |

Bundled in `deps/` and built from this tree: bitcoin, reproc, stduuid, cppcodec,
uint128, caches, scheduler, spdlog, ghc::filesystem, jsoncpp, Catch2 headers,
scrypt, chachapoly, sqlite3, rfc6234, sha3, klib. Notices are in
[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

## 3. Get the source

```powershell
git clone <repository-url> grinpp
cd grinpp
git checkout node-beta.1-candidate
```

There are no required submodules; `deps/` is vendored. Do not copy a
machine-specific profile, `bin/`, logs or build directories into the source tree.

## 4. Configure and build

```powershell
$sysroot = "C:\path\to\sysroot"   # absolute path; must contain CMake configs
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=$sysroot" `
  -DGRINPP_TESTS=ON `
  -DGRINPP_TOOLS=OFF
cmake --build build --config Release --target GrinNode Tests --parallel 12
```

Expected output location (the project writes into the source tree):

```text
bin\Release\Release\GrinNode.exe
bin\Release\Release\Tests.exe
```

`-DGRINPP_TOOLS=OFF` builds only the node and tests. `slate_tool` and
`tx_verifier` are auxiliary and not needed for a node release.

If MSBuild fails with file-tracker or named-pipe errors in a restricted
environment, build without parallel MSBuild nodes (`--parallel 1`) and keep the
temp directory short (`$env:TMP = $env:TEMP = "C:\tmp"`).

Build results are not guaranteed to be bit-identical across machines: compiler
version, SDK, paths and link order can change the binary. Record the SHA-256 of
your own artifacts.

## 5. Tests

`Tests.exe` is **not shipped in the user-facing release package**; it is
provided separately under `verification/` for reproducibility only. It binds a
hard-coded wallet-owner port **3421**: run only one instance, with port 3421
free, and from the repository/build root as the working directory. Run it with
an isolated `HOMEDRIVE`/`HOMEPATH`/`TEMP`; a run against a real user profile
caused a `0xC0000409` abort on the preparation machine. Do not claim general
safe execution. The known-good full-suite result is the release workstream's
clean-copy run (`373` cases / `4982` assertions).

Always run tests in an isolated home directory. The test binaries write profiles
under `%HOME%\.GrinPP`; pointing them at a real profile can modify it.

```powershell
$testRoot = Join-Path $PWD ("test-run-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force (Join-Path $testRoot "temp") | Out-Null
$rootPath = [IO.Path]::GetPathRoot($testRoot).TrimEnd('\')
$env:HOMEDRIVE = $rootPath
$env:HOMEPATH  = $testRoot.Substring($rootPath.Length)
$env:TEMP = $env:TMP = Join-Path $testRoot "temp"

& .\bin\Release\Release\Tests.exe "~[.]"
```

On the candidate this prints:

```text
All tests passed (4982 assertions in 373 test cases)
```

Hidden tags (`[.]`) are excluded by `~[.]`; run them explicitly when their
fixtures exist:

```powershell
.\bin\Release\Release\Tests.exe "[orphan]"
.\bin\Release\Release\Tests.exe "[fault]"
.\bin\Release\Release\Tests.exe "[pibd]"
.\bin\Release\Release\Tests.exe "PibdScheduler*"
.\bin\Release\Release\Tests.exe "BlockPipe*"
.\bin\Release\Release\Tests.exe "REORG*"
.\bin\Release\Release\Tests.exe "PibdSegmentService::FullSweepPpCkpt"
```

Notes:

- External PIBD/live vectors are not shipped. If a fixture is missing, the check
  is **skipped** — that is not a PASS. The `FullSweepPpCkpt` test can pass with
  an empty sweep (`bitmap 0/7, output 0/1678, …`) while printing a warning; that
  result does not exercise a populated sweep.
- The killed-process recovery harness is driven by
  `tools/audit/kernel-index-crash.ps1` and requires the `Tests.exe` of the same
  source tree. It forces a kill at a chosen transaction point; it is not a
  power-loss test.
- Never run two test binaries against the same profile or port. Use unique
  ports and unique home directories per run.

## 6. Run the node

```powershell
# Headless Floonet node, no wallet, default profile
.\bin\Release\Release\GrinNode.exe --floonet --headless --no-wallet
```

Useful options:

| Option | Meaning |
|---|---|
| `--floonet` | Floonet instead of Mainnet |
| `--automated-testing` | Test-only chain (do not use for network testing) |
| `--headless` | No interactive console display |
| `--no-wallet` | Node only |
| `--config-path <file>` | Use a specific `server_config.json` |
| `--shared-node host:port` | Use a remote node (client mode) |

Default paths and ports:

| Item | Mainnet | Floonet |
|---|---|---|
| Profile | `%USERPROFILE%\.GrinPP\MAINNET` | `%USERPROFILE%\.GrinPP\FLOONET` |
| Config | `…\server_config.json` | `…\server_config.json` |
| REST API | `3413` | `13413` |
| P2P | `3414` | `13414` |
| Data / logs | `<profile>\NODE`, `<profile>\LOGS` | same |

For an isolated run, create a config with an explicit `DATA_PATH` and separate
ports, then start with `--config-path`. Example:

```json
{
  "DATA_PATH": "D:\\grinpp-test\\floonet",
  "P2P": { "MIN_PEERS": 3, "MAX_PEERS": 20, "P2P_PORT": 23414 },
  "SERVER": { "REST_API_PORT": 23413 },
  "LOGGER": { "LOG_LEVEL": "DEBUG" }
}
```

The API secret is generated on first start at `<DATA_PATH>\.api_secret` (64 hex
characters, owner-only ACL). Never copy it into logs, reports or commits.
Diagnostic logs are written next to the executable in `logs\`; if that directory
is not writable the node falls back to `<DATA_PATH>\LOGS\node-diagnostics`.

Common errors:

| Symptom | Cause / action |
|---|---|
| `Could not find a package configuration file provided by …` | `CMAKE_PREFIX_PATH` does not point at the sysroot that contains all pinned configs. |
| `could not find any instance of Visual Studio` | VS 2022 Build Tools/C++ workload is missing. |
| Node exits at start with a bind/`address in use` error | REST/P2P port already used (another node or test). Change `REST_API_PORT`/`P2P_PORT`. |
| Two instances on one `DATA_PATH` | RocksDB lock. Do not share a profile between instances; profile-lock behaviour is not fully verified. |
| Startup seems stuck (minutes) | A full kernel-index rebuild and header validation are part of the design; see [ARCHITECTURE.md](ARCHITECTURE.md). Do not kill unless you accept a rebuild. |
| `GET /v1/status` returns 404 right after start | The status endpoint is registered later than `/v1/health`; use health to detect readiness of the process. |
| Owner RPC returns `-32001` / REST returns 401 | Missing/invalid secret, or the `.api_secret` file is not owner-only. Fix the ACL instead of weakening the check. |
| Skipped test warnings | External fixtures are absent. The skipped check is NOT VERIFIED, not PASS. |

## 7. Build verification status

| Item | Status |
|---|---|
| Candidate Release build `/W4 /WX` | **PASS** (local build log) |
| Clean-clone configure | **PASS** (independent release verification) |
| Clean-clone build and test run | **PASS** (clean clone built Release x64 and passed the full suite 373/4982; binaries not bit-identical to the candidate) |
| CI workflow prepared | **YES** (`.github/workflows/ci.yml`) |
| CI executed on GitHub Actions | **NO** — hosted CI is not evidence for this snapshot |
| `Tests.exe` packaging | **Verification-only**, not in the user-facing release package (separate `verification/`; isolated home/TEMP, free port 3421, repo/build root as cwd) |
| Windows 10 x64 build/run | **NOT VERIFIED** (source-supported) |
| Linux/macOS build | **NOT VERIFIED** |
| Windows executable imports only OS DLLs on the tested machine | **PASS** (release inspection); network-share execution NOT VERIFIED |

---

[Русская версия](BUILD.ru.md) · [Deutsche Fassung](BUILD.de.md) ·
[Back to README](../README.md)
