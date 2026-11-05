# Dependency sysroot helper (`build_sysroot.py`)

**Status:** dry-run PASS on the preparation host (`--dry-run` resolves paths,
toolchain and the full pinned plan without downloading or building).
**End-to-end dependency rebuild is NOT VERIFIED** during the public-fork
preparation. Do not claim the published sysroot can be reproduced
byte-for-byte from this helper until someone runs it and compares the manifest.

## What this is

`build_sysroot.py` is a portable rewrite of the preparation-time helper that built
the prebuilt Windows x64 dependency sysroot used by the accepted candidate
(private preparation sysroot: 746 files, 162,287,187 bytes, manifest SHA-256
`82FBF0F7F2D5E5BFFB8C049479E677A10F47177E6A55459269960EAD54EC5C53`).

The historical helper could not be published as-is:

| Problem in the historical copy | Fix in this helper |
|---|---|
| Hard-coded private absolute workspace paths (`.toolchain`, `src/grinplusplus`, `.tmp/deps`) | all paths are `--repo`-relative defaults (`<repo>\..\deps-toolchain`, `<repo>\..\deps-sysroot`) with explicit overrides |
| Hard-coded Program Files build-tools path | VS installation discovered with `vswhere.exe` (VC tools component required); `--toolchain-dir`, `--cmake`, `--msbuild` overrides |
| Reads the minizip port helper from a local vcpkg checkout | the helper files are bundled under `vcpkg-ports/minizip/`; `--minizip-port-dir` override |
| No plan/dry-run mode | `--dry-run` / `--what-if` prints the resolved plan and every CMake/MSBuild command without network or disk side effects |
| RockDB UTF-8 flag was `OFF` in the audit copy | defaults to `WITH_WINDOWS_UTF8_FILENAMES=ON` + `PORTABLE=ON`, matching the published sysroot and the repository vcpkg port (`WITH_WINDOWS_UTF8_FILENAMES=TRUE`); `--rocksdb-utf8 off` restores the old behaviour |
| Appended duplicate rows to `SOURCES.sha256` on every run | de-duplicates |
| Wrote no manifest | writes `logs/sysroot.sha256` (canonical `<sha256>  <posix-rel-path>` lines) and prints its SHA-256 |

The historical copies (identical content, only line endings differ): a private
audit-tree copy = 20,687 B, SHA-256
`E22B675516989D7F14411775595BCA1AE5F2A65FCB6C2A39A12B5951963098B0`;
a private working-copy path = 20,213 B (LF), SHA-256
`643ECBCE45557EC6002F7E1B69EC35C7A5467D5D1754D0FE4AF2C049FEB1E3A9`.
Both contain local paths and are therefore not distributable.

## Pinned dependencies (unchanged)

| Step | Revision / URL |
|---|---|
| zlib (and minizip sources) | `https://github.com/madler/zlib/archive/refs/tags/v1.2.11.tar.gz` |
| libsodium | `https://github.com/jedisct1/libsodium/releases/download/1.0.18-RELEASE/libsodium-1.0.18.tar.gz` |
| secp256k1-zkp | commit `5b39fbf7f1e16e126404791923511723c4c0876d` |
| rocksdb | commit `551a110918493a19d11243f53408b97485de1411` |
| civetweb | commit `2c1caa6e690bfe3b435a10c372ab2dcd14b872e8` |
| CRoaring | `v0.2.66` |
| fmt | `6.2.1` (header-only install, `fmt-config.cmake` generated) |
| asio | `asio-1-12-2` (header-only, standalone default + unistd guard) |
| mio | commit `cafa31360fee8866be89b4c602d8b9a7a18dbf5e` (header-only) |
| Catch2 | `v2.12.1` (single-header + CMake modules) |

Helper files from the repository's own `vcpkg/custom_ports` are reused unchanged:
`libsodium/CMakeLists.txt`, `libsodium/sodiumConfig.cmake.in`,
`secp256k1-zkp/CMakeLists.txt`, `secp256k1-zkp/windows/libsecp256k1-config.h`.

All builds use the static CRT (`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`,
`/MT` release flags) because the node links `/MT`.

## Usage

This helper ships at `tools/deps/build_sysroot.py`, with `vcpkg-ports/minizip/`
next to it, then:

```powershell
# plan only, no downloads
python tools\deps\build_sysroot.py --dry-run

# full build into <repo>\..\deps-sysroot
python tools\deps\build_sysroot.py

# custom locations
python tools\deps\build_sysroot.py --toolchain-dir D:\tool\work --sysroot-dir D:\tool\sysroot

# one dependency
python tools\deps\build_sysroot.py --only zlib rocksdb
```

Then the node build (same command as the accepted candidate build):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release `
      "-DCMAKE_PREFIX_PATH=D:\tool\sysroot" -DGRINPP_TESTS=ON -DGRINPP_TOOLS=OFF
cmake --build build --config Release --target GrinNode Tests --parallel 12
```

## `vcpkg-ports/minizip/` provenance

`CMakeLists.txt`, `minizipConfig.cmake.in` and `CONTROL` were copied unchanged from
the local vcpkg `ports/minizip` (the pinned vcpkg checkout used at preparation time;
MIT-licensed vcpkg port metadata). They are bundled only because the historical
helper read them from a local vcpkg tree. `minizip.patch` from that port is **not**
applied, matching the historical helper's behaviour; whether the published
`minizip.lib` included that patch was not verified.

## Integrity anchor

The published sysroot was measured by the release verifier:

- file count 746, total bytes 162,287,187
- manifest SHA-256 `82FBF0F7F2D5E5BFFB8C049479E677A10F47177E6A55459269960EAD54EC5C53`
  (PowerShell procedure: sorted `FullName`, each line `<path-relative>` + space +
  uppercase SHA-256 of the file, lines joined with `\n`, UTF-8; the exact
  procedure and the generated manifest file are kept with the release evidence)

The portable script writes its own canonical `logs/sysroot.sha256`; its SHA-256 is a
different, clearer anchor and will not equal the historical value by construction.
Compare file counts and per-file hashes, not only the aggregate string.
