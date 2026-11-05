# LICENSES — map and conventions

This directory contains the full license texts for every third-party component
distributed by this fork. The component-by-component notices are in
[`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md); the provenance, retrieval
date and SHA-256 of each text are in [`SOURCES.md`](SOURCES.md).

## Conventions

- The top-level files are **canonical license texts** named by their SPDX identifier
  (`<SPDX>.txt`). They are used for every component that is licensed under that
  license, regardless of the component's own copyright line.
- The project's own license is the root [`../LICENSE`](../LICENSE) (MIT,
  `Copyright (c) 2018-2019 David Burkett`). `MIT.txt` here is the SPDX MIT template
  used for **third-party** MIT components; each component's own copyright line is
  recorded in `../THIRD_PARTY_NOTICES.md` and, where available, in its per-component
  file under `upstream/`.
- `upstream/` preserves the **exact upstream license file** for components whose
  copyright line matters (static libraries whose sysroot carries no `copyright`
  file, Tor's composite license, pluggable transports, mingw-w64 winpthreads).
- `Public-Domain-NOTICE.md` is not a license text: it quotes the exact public-domain
  statements for components that have no license document.
- Non-SPDX files: `FAIR-MINING.txt` (custom license for the Cuckoo PoW code),
  `Tor-LICENSE.txt` (Tor's composite `LICENSE`, including third-party notices),
  `libevent-LICENSE.txt` (upstream libevent `LICENSE` with its `arc4` notice).

## Component → license → text

### Source-vendored components (`deps/**`, in-tree third-party code)

| Component | SPDX | Canonical text | Component-specific notice |
|---|---|---|---|
| bitcoin hash code | `MIT` | `MIT.txt` | `deps/bitcoin/src/COPYING` |
| reproc | `MIT` | `MIT.txt` | `deps/reproc/LICENSE` |
| stduuid | `MIT` | `MIT.txt` | `deps/stduuid/LICENSE` |
| Microsoft GSL (in stduuid) | `MIT` | `MIT.txt` | embedded in `deps/stduuid/include/gsl/*` |
| cppcodec | `MIT` | `MIT.txt` | `deps/cppcodec/LICENSE` |
| uint128 | `MIT` | `MIT.txt` | `deps/uint128/LICENSE` |
| caches | `BSD-3-Clause` | `BSD-3-Clause.txt` | `deps/caches/LICENSE.md` |
| scheduler | `MIT` | `MIT.txt` | `deps/scheduler/include/scheduler/License.txt` |
| CTPL (in scheduler) | `Apache-2.0` | `Apache-2.0.txt` | embedded in `deps/scheduler/include/scheduler/ctpl_stl.h` |
| spdlog | `MIT` | `MIT.txt` | `upstream/spdlog-0.17.0-LICENSE.txt` (no local file in `deps/spdlog`) |
| fmt (bundled in spdlog) | `BSD-2-Clause` | `BSD-2-Clause.txt` | `deps/spdlog/fmt/bundled/LICENSE.rst` |
| ghc::filesystem | `BSD-3-Clause` | `BSD-3-Clause.txt` | embedded in `deps/ghc/filesystem.hpp` |
| jsoncpp | Public Domain / `MIT` (dual) | `MIT.txt` + `Public-Domain-NOTICE.md` | embedded in `deps/jsoncpp/json/json.h` |
| Catch2 2.4.2 | `BSL-1.0` | `BSL-1.0.txt` | pointer only in `deps/Catch2/catch.hpp` |
| Catch2 2.2.2 (in stduuid tests) | `BSL-1.0` | `BSL-1.0.txt` | embedded in `deps/stduuid/catch/catch.hpp` |
| scrypt | `BSD-2-Clause` | `BSD-2-Clause.txt` | embedded in `deps/scrypt/crypto_scrypt-ref.cpp` |
| ChaCha20 (chachapoly) | Public Domain | `Public-Domain-NOTICE.md` | embedded in `deps/chachapoly/chacha.h` |
| Poly1305 (chachapoly) | Public Domain | `Public-Domain-NOTICE.md` | embedded in `deps/chachapoly/poly1305.cpp` |
| chachapoly AEAD wrapper | `MIT` | `MIT.txt` | embedded in `deps/chachapoly/chachapoly.cpp`, `.h` |
| libsqlite3 | Public Domain | `Public-Domain-NOTICE.md` | embedded in `deps/libsqlite3/sqlite3.h`, `sqlite3.c` |
| rfc6234 | `BSD-3-Clause` | `BSD-3-Clause.txt` | `deps/rfc6234/LICENSE` |
| sha3 | Zlib-style (bespoke) | `deps/sha3/LICENSE` (exact text; **not** identical to `Zlib.txt`) | `deps/sha3/LICENSE` |
| klib / ketopt | `MIT` | `MIT.txt` | `deps/klib/LICENSE` |
| siphash | `MIT` | `MIT.txt` | embedded in `src/Crypto/ThirdParty/siphash.h` |
| portable_endian | Public Domain (or BSD/MIT/Apache) | `Public-Domain-NOTICE.md`, `BSD-2-Clause.txt`, `MIT.txt`, `Apache-2.0.txt` | embedded in `src/PoW/portable_endian.h` |
| Cuckoo PoW | FAIR MINING **or** `GPL-2.0-or-later` | `FAIR-MINING.txt`, `GPL-2.0-only.txt` | `src/PoW/LICENSE.txt` |
| `deps/Roaring.h` | project shim | — | — |

### Statically linked vcpkg / sysroot components

| Component | SPDX | Canonical text | Component-specific notice |
|---|---|---|---|
| libsodium 1.0.18 | `ISC` | `ISC.txt` | `upstream/libsodium-LICENSE.txt` |
| RocksDB 6.6.4 | `Apache-2.0` (dual `GPL-2.0`) | `Apache-2.0.txt`, `GPL-2.0-only.txt` | `upstream/rocksdb-LICENSE.Apache.txt` |
| RocksDB / LevelDB portions | `BSD-3-Clause` | `BSD-3-Clause.txt` | `upstream/rocksdb-LICENSE.leveldb.txt` |
| zlib 1.2.11 | `Zlib` | `Zlib.txt` | `upstream/zlib-README.txt` |
| civetweb 1.12 | `MIT` | `MIT.txt` | `upstream/civetweb-LICENSE.md` |
| minizip | `Zlib` | `Zlib.txt` | `upstream/minizip-LICENSE.txt` |
| CRoaring 0.2.66 | `Apache-2.0` | `Apache-2.0.txt` | `upstream/croaring-LICENSE.txt` |
| asio 1.12.2 | `BSL-1.0` | `BSL-1.0.txt` | `upstream/asio-COPYING.txt` |
| secp256k1-zkp 1.0.0 | `MIT` | `MIT.txt` | `upstream/secp256k1-zkp-COPYING.txt` |
| mio | `MIT` | `MIT.txt` | `upstream/mio-LICENSE.txt` |
| fmt 6.2.1 | `MIT` | `MIT.txt` | `upstream/fmt-6.2.1-LICENSE.rst` |
| Catch2 2.12.1 | `BSL-1.0` | `BSL-1.0.txt` | `upstream/catch2-2.12.1-LICENSE.txt` |

### `dist/**` Tor bundle

| Component | SPDX | Canonical text | Component-specific notice |
|---|---|---|---|
| Tor 0.4.7.10 / 0.4.7.9 | `BSD-3-Clause` | `BSD-3-Clause.txt`, `Tor-LICENSE.txt` | `Tor-LICENSE.txt` (upstream composite `LICENSE`) |
| OpenSSL 1.1.1q | `OpenSSL` + SSLeay | `OpenSSL.txt` | same file (both licenses) |
| libevent 2.1.12-stable | `BSD-3-Clause` | `BSD-3-Clause.txt`, `libevent-LICENSE.txt` | `libevent-LICENSE.txt` (upstream, with `arc4` notice) |
| zlib 1.2.12 | `Zlib` | `Zlib.txt` | — |
| obfs4proxy | `BSD-2-Clause` | `BSD-2-Clause.txt` | `upstream/obfs4-LICENSE.txt` |
| snowflake-client | `BSD-3-Clause` | `BSD-3-Clause.txt` | `upstream/snowflake-LICENSE.txt` |
| mingw-w64 runtime (`libgcc`, `libssp`, `libstdc++`) | `GPL-3.0-or-later WITH GCC-exception-3.1` | `GPL-3.0-only.txt`, `GCC-exception-3.1.txt` | — |
| mingw-w64 `libwinpthread-1.dll` | `MIT` | `MIT.txt` | `upstream/winpthreads-COPYING.txt` |
| IPFire Location DB (`geoip`, `geoip6`) | `CC-BY-SA-4.0` | `CC-BY-SA-4.0.txt` | header embedded in each data file |

### Adapted Rust Grin node

| Component | SPDX | Canonical text | Attribution |
|---|---|---|---|
| Rust `grin` node 5.5.2 (PIBD/NRD adaptation) | `Apache-2.0` | `Apache-2.0.txt` | Copyright The Grin Developers; see `../THIRD_PARTY_NOTICES.md` §5 |

The exact files carrying the §4(c) attribution header are listed in [GRIN-DERIVED-FILES.md](GRIN-DERIVED-FILES.md) (30 files). The short repository-level attribution is the root [../NOTICE](../NOTICE).

### Project asset

| Asset | Status | Text |
|---|---|---|
| `Logo.png` | PARTIAL — upstream project asset, no separate license file | project [`../LICENSE`](../LICENSE) (MIT) |

## Notes

- `fmt` is required by CMake (`find_package(fmt)`) but is **not** in the `GrinNode`
  link line, so it is not confirmed as linked into `GrinNode.exe`.
- The sysroot used for the final build contains no vcpkg `copyright` files; the
  `upstream/` texts were fetched from the exact pinned upstream revisions and their
  URLs/hashes are recorded in `SOURCES.md`.
- The text of `CC-BY-SA-4.0.txt` is the complete legal code retrieved from
  <https://creativecommons.org/licenses/by-sa/4.0/legalcode.txt>.
- If a dependency is added, removed or upgraded, update `../THIRD_PARTY_NOTICES.md`,
  this map, and `SOURCES.md` in the same commit.
