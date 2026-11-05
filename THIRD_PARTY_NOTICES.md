# Third-Party Notices

**Project:** Grin++ — public fork of GrinPlusPlus v1.2.9
**Project license:** MIT — see [`LICENSE`](LICENSE) (`Copyright (c) 2018-2019 David Burkett`)
**Notices revision:** 2026-11-06
**Applies to:** the public source tree (`repository/`) and the Windows release assets
(`GrinNode.exe`, `Tests.exe`).

This file is the distribution notice map for every third-party component that is
distributed as source, compiled into a distributed binary, or shipped as a binary
artifact by this fork. Full license texts are in [`LICENSES/`](LICENSES/); the exact
provenance, retrieval date and SHA-256 of every text are in
[`LICENSES/SOURCES.md`](LICENSES/SOURCES.md); a component-to-license map is in
[`LICENSES/README.md`](LICENSES/README.md).

Status vocabulary used below: **PASS** (verified against the repository tree),
**PARTIAL** (license established but a local notice file is missing or incomplete),
**NOT VERIFIED** (claim could not be established from the distributed artifacts),
**BLOCKED** (action required before publication, or a component that cannot be
published as-is).

This is an engineering notices package, not legal advice.

## 1. Distribution scope

| Channel | License-relevant contents |
|---|---|
| Public **source repository** | full upstream v1.2.9 tree plus the fork overlay: `deps/**`, `src/PoW/**`, `src/Crypto/ThirdParty/**`, the 28 files under `dist/**` (Tor/PT bundles), `Logo.png`, `vcpkg/custom_ports/**`, and fork source |
| Windows **release package** | `GrinNode.exe`, `Tests.exe`, docs, `LICENSE`, this file, manifests. No Tor bundle, no sysroot, no MSVC runtime DLLs |
| Historical **RC1 package** (`Beta1/RC1/fix4node.zip`) | **NOT part of this publication**; see §6 |
| UI / Electron (`GrinPlusPlusUI`) | separate repository, **not distributed**; see §8 |

`GrinNode.exe` is a statically linked Release/x64 build with the MSVC static CRT
(`/MT`). The libraries listed in §3 are therefore compiled into the executable, even
though their source does not ship in the release package.

## 2. Bundled source components (`deps/**` and in-tree third-party code)

### 2.1 Vendored libraries and components

| Component | Version | License (SPDX) | Copyright notice | Notice path / text |
|---|---|---|---|---|
| bitcoin hash code | unversioned snapshot | `MIT` | Copyright (c) 2016 Pieter Wuille | `deps/bitcoin/src/COPYING` |
| reproc | unversioned snapshot | `MIT` | Copyright (c) Daan De Meyer | `deps/reproc/LICENSE` |
| stduuid | unversioned snapshot | `MIT` | Copyright (c) 2017 (the upstream file does not name the holder) | `deps/stduuid/LICENSE` |
| Microsoft GSL (bundled with stduuid) | unversioned | `MIT` | Copyright (c) 2015 Microsoft Corporation | embedded in `deps/stduuid/include/gsl/*`; text `LICENSES/MIT.txt` |
| cppcodec | unversioned snapshot | `MIT` | Copyright (c) 2015 Topology Inc.; Copyright (c) 2018 Jakob Petsovits; other contributors per file | `deps/cppcodec/LICENSE` |
| uint128 | unversioned snapshot | `MIT` | Copyright (c) 2013–2017 Jason Lee | `deps/uint128/LICENSE` |
| caches | unversioned snapshot | `BSD-3-Clause` | Copyright (c) 2015–2017 Vladimir Petrigo | `deps/caches/LICENSE.md` |
| scheduler | unversioned snapshot | `MIT` | Copyright (c) 2017 Bosma | `deps/scheduler/include/scheduler/License.txt` |
| CTPL (bundled with scheduler) | 2014 | `Apache-2.0` | Copyright (C) 2014 Vitaliy Vitsentiy | embedded in `deps/scheduler/include/scheduler/ctpl_stl.h`; text `LICENSES/Apache-2.0.txt` |
| spdlog | 0.17.0 (`SPDLOG_VERSION "0.17.0"`) | `MIT` | Copyright (c) 2016 Gabi Melman | **no standalone file in `deps/spdlog`**; headers carry only a short MIT reference; text `LICENSES/MIT.txt` and `LICENSES/upstream/spdlog-0.17.0-LICENSE.txt` |
| fmt (bundled with spdlog) | not declared | `BSD-2-Clause` | Copyright (c) 2012–2016 Victor Zverovich | `deps/spdlog/fmt/bundled/LICENSE.rst` |
| ghc::filesystem | 1.2.10 (`GHC_FILESYSTEM_VERSION 10210L`) | `BSD-3-Clause` | Copyright (c) 2018 Steffen Schümann | embedded in `deps/ghc/filesystem.hpp` |
| jsoncpp | unversioned amalgamation (version macros left as `@…@` placeholders) | Public Domain / `MIT` (dual) | Baptiste Lepilleur and The JsonCpp Authors | embedded in `deps/jsoncpp/json/json.h` |
| Catch2 | 2.4.2 | `BSL-1.0` | Copyright (c) 2018 Two Blue Cubes Ltd. | `deps/Catch2/catch.hpp` contains only a pointer to `LICENSE_1_0.txt`, which is absent; text `LICENSES/BSL-1.0.txt` |
| Catch2 (bundled in stduuid tests) | 2.2.2 | `BSL-1.0` | Copyright (c) 2018 Two Blue Cubes Ltd. | embedded in `deps/stduuid/catch/catch.hpp`; text `LICENSES/BSL-1.0.txt` |
| scrypt | unversioned | `BSD-2-Clause` | Copyright 2009 Colin Percival | full text embedded in `deps/scrypt/crypto_scrypt-ref.cpp` |
| chachapoly — ChaCha20 | unversioned | Public Domain | D. J. Bernstein (`chacha-merged.c` version 20080118) | embedded in `deps/chachapoly/chacha.h` |
| chachapoly — Poly1305 | unversioned | Public Domain | poly1305-donna implementation (RFC 7539 variant) | embedded in `deps/chachapoly/poly1305.cpp` |
| chachapoly — AEAD wrapper | unversioned | `MIT` | Copyright (c) 2015 Grigori Goronzy | full text embedded in `deps/chachapoly/chachapoly.cpp` and `.h` |
| libsqlite3 | 3.28.0 (`SQLITE_VERSION "3.28.0"`) | Public Domain (author's blessing) | SQLite authors | embedded in `deps/libsqlite3/sqlite3.h` and `sqlite3.c` |
| rfc6234 | unversioned | `BSD-3-Clause` | Copyright (c) 2011 IETF Trust and the persons identified as authors of the code | `deps/rfc6234/LICENSE` |
| sha3 | unversioned | Zlib-style (custom) | Copyright (c) 2014,2015 Stephan Brumme | `deps/sha3/LICENSE` |
| klib / ketopt | unversioned | `MIT` | Copyright (c) 2008– Attractive Chaos | `deps/klib/LICENSE` |
| siphash | unversioned | `MIT` | Copyright 2013 Masatoshi Teruya | embedded in `src/Crypto/ThirdParty/siphash.h` |
| portable_endian | unversioned | Public Domain; alternatively `BSD` / `MIT` / `Apache-2.0` at the recipient's choice | Mathias Panzenböck (as stated in the file) | embedded in `src/PoW/portable_endian.h` |
| Cuckoo PoW | unversioned | FAIR MINING License **or** `GPL-2.0-or-later` (choice) | Copyright (c) 2013–2019 John Tromp | `src/PoW/LICENSE.txt`; text `LICENSES/FAIR-MINING.txt` |
| `deps/Roaring.h` | n/a | project shim (no third-party code) | — | — |

### 2.2 Notes and corrections for this group

- **bitcoin:** the notice is `deps/bitcoin/src/COPYING`, not `deps/bitcoin/COPYING`.
  The MIT text was verified locally.
- **caches:** the local file is the **BSD-3-Clause** license (Vladimir Petrigo), not MIT.
- **uint128:** the local file is the **MIT** license (Jason Lee), not BSD-2-Clause.
- **spdlog:** the `deps/spdlog` snapshot has **no** `LICENSE.rst`; `deps/spdlog/LICENSE.rst`
  does not exist. The license is MIT (`Copyright (c) 2016 Gabi Melman`), verified from the
  upstream v0.17.0 `LICENSE` and from the per-file headers. The full text is supplied in
  `LICENSES/MIT.txt` and `LICENSES/upstream/spdlog-0.17.0-LICENSE.txt`.
- **Catch2:** `deps/Catch2/catch.hpp` (v2.4.2) only points at a `LICENSE_1_0.txt` that is
  absent from the tree. The BSL-1.0 text is supplied in `LICENSES/BSL-1.0.txt`. A second
  Catch2 copy (v2.2.2) is vendored in `deps/stduuid/catch/catch.hpp`.
- **Bundled subcomponents:** CTPL is inside `deps/scheduler` (Apache-2.0, not MIT), and the
  Microsoft GSL is inside `deps/stduuid` (MIT). Both are listed separately above.
- **chachapoly is a composite:** the ChaCha20 and Poly1305 primitives are public domain,
  but the AEAD wrapper (`chachapoly.cpp`, `chachapoly.h`) is MIT, Copyright (c) 2015
  Grigori Goronzy.
- **portable_endian.h** (inside `src/PoW`) is third-party code (Mathias Panzenböck) and is
  public domain, with a stated BSD/MIT/Apache dual-license fallback for jurisdictions that
  do not recognise public-domain dedication.
- **FAIR MINING / PoW:** the Cuckoo PoW code is distributed here under the **FAIR MINING
  License**; the license also offers `GPL-2.0-or-later` as an alternative. The FAIR MINING
  condition requires that a derived miner charging a developer fee for mining a fair coin
  share half of that fee revenue with the coin developers. This fork does not add a mining
  developer fee. `LICENSES/GPL-2.0-only.txt` is included for the alternative.
- **Public-domain components** (ChaCha20, Poly1305, SQLite, and the public-domain option of
  jsoncpp) have no license text to reproduce; their exact statements are quoted in
  `LICENSES/Public-Domain-NOTICE.md`.

## 3. Statically linked vcpkg / sysroot dependencies

These libraries are used by the distributed Windows build; those marked “linked” in the
last column are compiled into the distributed binaries. The prebuilt sysroot itself is
**not** published. Versions below were verified from the pinned vcpkg port files
in `vcpkg/custom_ports/**`, from the sysroot headers actually used by the build, and from
the link line of `GrinNode.vcxproj`.

| Component | Version / pin | License (SPDX) | Copyright notice | Notice text | Linked into |
|---|---|---|---|---|---|
| libsodium | 1.0.18 (port `REF 1.0.18`; `SODIUM_VERSION_STRING "1.0.18"`) | `ISC` | Copyright (c) 2013–2019 Frank Denis | `LICENSES/ISC.txt`, `LICENSES/upstream/libsodium-LICENSE.txt` | `GrinNode.exe` (`libsodium.lib`) |
| RocksDB | 6.6.4 (port `REF 551a110918493a19d11243f53408b97485de1411`; `ROCKSDB_MAJOR/MINOR/PATCH = 6/6/4`) | `Apache-2.0` (dual `GPL-2.0`/`Apache-2.0`; this distribution uses Apache-2.0) | Copyright (c) 2011–present Facebook, Inc.; contains LevelDB code Copyright (c) 2011 The LevelDB Authors (`BSD-3-Clause`) | `LICENSES/Apache-2.0.txt`, `LICENSES/upstream/rocksdb-LICENSE.Apache.txt`, `LICENSES/upstream/rocksdb-LICENSE.leveldb.txt` | `GrinNode.exe` (`rocksdb.lib`) |
| zlib | 1.2.11 | `Zlib` | Copyright (C) 1995–2017 Jean-loup Gailly and Mark Adler | `LICENSES/Zlib.txt`, `LICENSES/upstream/zlib-README.txt` | `GrinNode.exe` (`zlibstatic.lib`) |
| civetweb | 1.12 (port `2019-07-05-1`, `REF 2c1caa6e690bfe3b435a10c372ab2dcd14b872e8`) | `MIT` | Copyright (c) 2013–2020 The CivetWeb developers; Copyright (c) 2004–2013 Sergey Lyubka; Copyright (c) 2013 No Face Press, LLC (Thomas Davis); Copyright (c) 2013 F-Secure Corporation | `LICENSES/upstream/civetweb-LICENSE.md` (composite upstream file; optional Lua/SQLite components are not compiled in this build) | `GrinNode.exe` (`civetweb.lib`, `civetweb-cpp.lib`) |
| minizip | version not declared (header says “Version 1.1, February 14h, 2010”) | `Zlib` | Copyright (C) 1998–2010 Gilles Vollant; Copyright (C) 2007–2008 Even Rouault; Copyright (C) 2009–2010 Mathias Svensson | `LICENSES/Zlib.txt`, `LICENSES/upstream/minizip-LICENSE.txt` | `GrinNode.exe` (`minizip.lib`) |
| CRoaring | 0.2.66 (port `REF v0.2.66`; `ROARING_VERSION_* = 0.2.66`) | `Apache-2.0` | Copyright holders as listed in the upstream Apache-2.0 `LICENSE` (no separate copyright line in the license text) | `LICENSES/Apache-2.0.txt`, `LICENSES/upstream/croaring-LICENSE.txt` | `GrinNode.exe` (`roaring.lib`) |
| asio | 1.12.2 (`ASIO_VERSION 101202`) | `BSL-1.0` | Copyright (c) 2003–2018 Christopher M. Kohlhoff | `LICENSES/BSL-1.0.txt`, `LICENSES/upstream/asio-COPYING.txt` | headers only |
| secp256k1-zkp | 1.0.0 (port `REF 5b39fbf7f1e16e126404791923511723c4c0876d`) | `MIT` | Copyright (c) 2013 Pieter Wuille | `LICENSES/MIT.txt`, `LICENSES/upstream/secp256k1-zkp-COPYING.txt` | `GrinNode.exe` (`secp256k1-zkp.lib`) |
| mio | version not declared | `MIT` | Copyright 2017 https://github.com/mandreyel | `LICENSES/upstream/mio-LICENSE.txt` (copied from the sysroot header) | headers only |
| fmt | 6.2.1 (`FMT_VERSION 60201`) | `MIT` | Copyright (c) 2012–present Victor Zverovich | `LICENSES/MIT.txt`, `LICENSES/upstream/fmt-6.2.1-LICENSE.rst` | required by CMake (`find_package(fmt)`); **not** in the `GrinNode` link line |
| Catch2 | 2.12.1 (`Catch v2.12.1`) | `BSL-1.0` | Copyright (c) 2020 Two Blue Cubes Ltd. | `LICENSES/BSL-1.0.txt`, `LICENSES/upstream/catch2-2.12.1-LICENSE.txt` | `Tests.exe` only |

**Provenance note (PARTIAL, not blocking):** the sysroot `share/` directory used by the
final build contains **no vcpkg `copyright` files** for these libraries. The licenses and
versions above were established from the pinned upstream revisions and the local headers;
the exact upstream license texts are shipped under `LICENSES/upstream/` and their URLs,
retrieval dates and SHA-256 values are recorded in `LICENSES/SOURCES.md`.

**Link-line note:** `fmt` is found by CMake but does not appear in the
`AdditionalDependencies` of `GrinNode.vcxproj`, so it is not confirmed as linked into
`GrinNode.exe`. It is listed because it is a required build dependency.

## 4. `dist/**` — Tor / pluggable-transport bundle

The upstream v1.2.9 tree tracks `dist/**`; this fork publishes the full upstream tree, so
these binaries ship with the source repository. They are **not** included in the Windows
release package.

### 4.1 Artifacts verified in the repository

| Platform | Files |
|---|---|
| `dist/win/tor/` | `tor.exe`, `libcrypto-1_1-x64.dll`, `libssl-1_1-x64.dll`, `libevent-2-1-7.dll`, `libevent_core-2-1-7.dll`, `libevent_extra-2-1-7.dll`, `libgcc_s_seh-1.dll`, `libssp-0.dll`, `libwinpthread-1.dll`, `zlib1.dll`, `PluggableTransports/obfs4proxy.exe`, `PluggableTransports/snowflake-client.exe`, `geoip`, `geoip6` |
| `dist/linux/tor/` | `tor`, `libcrypto.so.1.1`, `libssl.so.1.1`, `libevent-2.1.so.7`, `libstdc++/libstdc++.so.6`, `PluggableTransports/obfs4proxy`, `PluggableTransports/snowflake-client`, `geoip`, `geoip6` |
| `dist/mac/tor/` | `tor`, `PluggableTransports/obfs4proxy`, `PluggableTransports/snowflake-client`, `geoip`, `geoip6` (dependencies are statically embedded in `tor`; no separate OpenSSL/libevent/zlib files) |

### 4.2 Components and licenses

| Component | Version | License | Copyright notice | Notice text |
|---|---|---|---|---|
| Tor | 0.4.7.10 (win/linux, verified from `tor` version strings); 0.4.7.9 (mac) | `BSD-3-Clause` | Copyright (c) 2001–2004 Roger Dingledine; Copyright (c) 2004–2006 Roger Dingledine, Nick Mathewson; Copyright (c) 2007–2019 The Tor Project, Inc. | `LICENSES/Tor-LICENSE.txt` (full upstream Tor `LICENSE`, including the third-party notices compiled into Tor) |
| OpenSSL | 1.1.1q (win/linux, verified string `OpenSSL 1.1.1q  5 Jul 2022`); mac exact version **NOT VERIFIED** (statically embedded) | `OpenSSL` + original SSLeay License (both apply) | Copyright (c) 1998–2019 The OpenSSL Project; Copyright (C) 1995–1998 Eric Young; portions by Tim Hudson | `LICENSES/OpenSSL.txt` |
| libevent | 2.1.12-stable (verified version string) | `BSD-3-Clause` (plus the OpenBSD `arc4` and other notices contained in the upstream file) | Copyright (c) 2000–2007 Niels Provos; Copyright (c) 2007–2012 Niels Provos and Nick Mathewson; additional notices for Dug Song (2000), The Regents of the University of California (1993) and Todd C. Miller (1998) | `LICENSES/libevent-LICENSE.txt` |
| zlib | 1.2.12 (`dist/win/tor/zlib1.dll`) | `Zlib` | Copyright (C) 1995–2017 Jean-loup Gailly and Mark Adler | `LICENSES/Zlib.txt` |
| obfs4proxy | version string not embedded (Tor 0.4.7.x-era build; Go 1.17.13) | `BSD-2-Clause` | Copyright (c) 2014, Yawning Angel | `LICENSES/upstream/obfs4-LICENSE.txt`; **PARTIAL** — exact bundled revision not established |
| snowflake-client | Go module path `…/snowflake.git/v2`; exact version not embedded (Go 1.17.13) | `BSD-3-Clause` | Copyright (c) 2016 Serene Han, Arlo Breault; Copyright (c) 2019–2020 The Tor Project, Inc. | `LICENSES/upstream/snowflake-LICENSE.txt`; **PARTIAL** — exact bundled revision not established |
| mingw-w64 runtime: `libgcc_s_seh-1.dll`, `libssp-0.dll`, linux `libstdc++.so.6` | GCC 10.3.0 (version strings in `libgcc_s_seh-1.dll`) | `GPL-3.0-or-later WITH GCC-exception-3.1` | Copyright (C) Free Software Foundation, Inc. | `LICENSES/GPL-3.0-only.txt` and `LICENSES/GCC-exception-3.1.txt` |
| `libwinpthread-1.dll` | 1.0.0.0 (PE version string) | `MIT` | Copyright (c) 2011 mingw-w64 project | `LICENSES/MIT.txt`, `LICENSES/upstream/winpthreads-COPYING.txt` |
| GeoIP data: `geoip`, `geoip6` | IPFire Location DB exports generated 2022-08-09 (win/linux) and 2022-08-11 (mac) | `CC-BY-SA-4.0` | IPFire Project — see `LICENSES/CC-BY-SA-4.0.txt` | embedded license header in each data file; attribution below |

### 4.3 OpenSSL advertising acknowledgment (required by both OpenSSL and SSLeay licenses)

The following acknowledgments are included in this distribution documentation, as
required by clause 3/6 of the OpenSSL License and clause 3 of the original SSLeay
License:

> This product includes software developed by the OpenSSL Project
> for use in the OpenSSL Toolkit (http://www.openssl.org/)
>
> This product includes cryptographic software written by
> Eric Young (eay@cryptsoft.com)
>
> This product includes software written by Tim Hudson (tjh@cryptsoft.com)

Eric Young is acknowledged as the author of the parts of the SSLeay library used.

### 4.4 GeoIP attribution and ShareAlike

The `geoip` / `geoip6` files are converted from the **IPFire Location database**
(Vendor: IPFire Project, <https://location.ipfire.org/>), not from MaxMind GeoLite2.
The embedded header states: *“License: CC BY-SA 4.0”*. The data is redistributed
**unmodified**, with the embedded attribution/license header retained. Redistribution is
under the Creative Commons Attribution-ShareAlike 4.0 International license; if a
downstream recipient modifies or adapts the database, the ShareAlike obligation applies
to the adapted database. The full legal code is in `LICENSES/CC-BY-SA-4.0.txt`.

### 4.5 Tor's own third-party notices

`dist/**/tor` is built from the Tor source release whose `LICENSE` file also lists
licenses for components compiled into Tor (for example Todd C. Miller's `strlcpy`/
`strlcat`, NetBSD code, and LLVM `compiler-rt`). The complete upstream Tor `LICENSE`
is shipped verbatim as `LICENSES/Tor-LICENSE.txt`; no attempt is made here to
re-license or paraphrase it.

## 5. Adapted Rust `grin` node 5.5.2 — Apache-2.0

### 5.1 Provenance

| Property | Value |
|---|---|
| Upstream work | **`grin` — the Rust Grin *node***, version **5.5.2** — **not** `grin-wallet` |
| Copyright | **Copyright The Grin Developers** |
| License | **Apache License, Version 2.0** |
| Source archive | Local reference archive `grin-5.5.2.zip` (not published), 7,974,593 bytes, SHA-256 `C60BE3BB38C66FCAD3BC9D057F8C98D8A193D8B65E44AFC71F43E4A270FD9349` |
| License file in archive | `grin-5.5.2/LICENSE`, 10,174 bytes, SHA-256 `0D542E0C8804E39AA7F37EB00DA5A762149DC682D7829451287E11B938E94594` (the same Apache-2.0 terms, without the non-normative appendix) |
| `NOTICE` file in archive | **none** |
| Nature of reuse | PIBD (parallel initial block download) / NRD algorithms and protocol structures were **ported/reimplemented** into C++ from the public Rust reference; this is a derivative work of grin 5.5.2 |
| Rust binary distributed | **no** — no Rust source or binary is shipped; only the C++ adaptation |

### 5.2 Affected areas in this repository

The **canonical, verified list** is [`LICENSES/GRIN-DERIVED-FILES.md`](LICENSES/GRIN-DERIVED-FILES.md)
(30 files, each carrying a `Derived from grin 5.5.2 …` attribution header). The list below
is a representative summary of the main areas:

- `include/Consensus.h` — `TXHASHSET_ARCHIVE_INTERVAL = 12 * 60`, taken from grin's `global::…`.
- `src/P2P/PIBD/PibdParams.h` — per-tree segment heights ported from grin 5.5.2.
- `src/P2P/PIBD/PibdArchiveServer.h` — archive-header selection mirrors grin 5.5.2 `chain/src/chain.rs`.
- `src/P2P/MessageProcessor.cpp`, `src/P2P/Messages/MessageTypes.h` — wire message IDs and rules mirroring grin.
- `src/BlockChain/Validators/BlockValidator.cpp`, `src/Core/Config.cpp`,
  `src/BlockChain/Processors/TxHashSetProcessor.cpp`, `src/PMMR/TxHashSetImpl.cpp`,
  `src/TxPool/TransactionPoolImpl.h`, `include/Core/Config.h` — NRD/PIBD rules and protocol invariants.
- `tests/src/Net/Test_P2PCompat.cpp` and `tests/src/PMMR/Test_NrdDifferential.cpp`,
  `tests/src/PMMR/Test_PibdServing.cpp`, `tests/src/PMMR/Test_NrdPolicy.cpp` — test vectors pinned to grin 5.5.2.
- `tools/audit/nrd/README.md` and `tools/audit/nrd/cases/nrd_cases.json` — Rust-oracle comparison material.

### 5.3 Apache-2.0 §4 obligations and how they are met

| §4 requirement | Status in this package |
|---|---|
| §4(a) — give recipients a copy of the License | **Met.** Full text in `LICENSES/Apache-2.0.txt` and Appendix A below. |
| §4(b) — retain copyright, patent, trademark and attribution notices | **Met in this file.** “Copyright The Grin Developers” is stated here; the upstream archive contains no `NOTICE` file. |
| §4(c) — cause modified files to carry prominent notices stating that the recipient changed the files | **Met.** All 30 files in `LICENSES/GRIN-DERIVED-FILES.md` carry a comment-only attribution header: “Derived from grin 5.5.2 (Rust), Copyright The Grin Developers … Modified for the Grin++ C++ fork.” No executable statement was changed. |
| §4(d) — include any `NOTICE` file contents | **Not applicable.** The upstream archive has no `NOTICE`; nothing to reproduce, and none is fabricated. |
| §4(e) — do not use Grin/trademarks without permission | **Acknowledged.** This file makes no trademark grant; the fork is not presented as an official Grin/Grin++ product. |

The full Apache-2.0 text is reproduced in Appendix A and shipped as
`LICENSES/Apache-2.0.txt` (verbatim from <https://www.apache.org/licenses/LICENSE-2.0.txt>).

## 6. MSVC / Windows runtime

- `GrinNode.exe` and `Tests.exe` are built with the **static MSVC runtime**:
  `CMAKE_MSVC_RUNTIME_LIBRARY` is set to `MultiThreaded$<$<CONFIG:Debug>:Debug>` and
  `/MD` is replaced with `/MT` in `CMakeLists.txt` (lines 34 and 54).
- The PE import table of the published `GrinNode.exe` and `Tests.exe` was inspected with
  `llvm-objdump -p`: the only imported DLLs are operating-system DLLs —
  `SHLWAPI.dll`, `RPCRT4.dll`, `ADVAPI32.dll`, `KERNEL32.dll`, `ole32.dll`, `WS2_32.dll`,
  `MSWSOCK.dll`, `bcrypt.dll`. **No** `msvcp140*.dll`, `vcruntime140*.dll` or
  `concrt140.dll` is imported.
- Consequently, the MSVC runtime is **not redistributed** in the final release: it is
  statically linked, and no Microsoft runtime DLL is shipped.
- The historical **RC1 package** `Beta1/RC1/fix4node.zip` is **not part of this
  publication**. It additionally contains `msvcp140*.dll`, `vcruntime140*.dll`,
  `concrt140.dll` and `rocksdb-shared.dll`. If it is ever republished, it needs its own
  Microsoft Visual Studio Distributable Code notice and a RocksDB Apache-2.0 notice.

## 7. Tests and release assets

- The release staging includes **`Tests.exe`** (10,710,016 bytes) next to `GrinNode.exe`.
  `Tests.exe` links Catch2 2.12.1; the **BSL-1.0** text is therefore required and is
  supplied in `LICENSES/BSL-1.0.txt`.
- The source tree also contains Catch2 2.4.2 (`deps/Catch2`) and Catch2 2.2.2
  (`deps/stduuid/catch`), both BSL-1.0; the same text covers them.
- If `Tests.exe` is dropped from a future release, the BSL-1.0 obligation for the source
  headers remains; do not remove the text.

## 8. Assets, fonts, UI

- **`Logo.png`** (10,732 bytes, SHA-256
  `52658BE6CEFE92685411A733D044C19306B0456B41797904A849498FE7471D92`) is the upstream
  project asset introduced in the initial upstream commit `7a4a6619` (2018-12-17, David
  Burkett), present unchanged at tag `v1.2.9`. Upstream ships **no separate license file**
  for it, so its status is **PARTIAL**: it is retained as an upstream project asset under
  the repository-level MIT license, with the upstream copyright retained; no third-party
  license text is asserted. It is not used by the build and can be removed from a future
  derivative if stricter asset separation is required. No trademark right is claimed.
- **Fonts:** none distributed (no `.ttf`, `.otf`, `.woff`, `.woff2` or `.eot` in the
  repository). Not applicable.
- **UI / Electron (`GrinPlusPlusUI`):** a separate repository, not part of upstream
  v1.2.9 and **not distributed** by this fork. No LICENSE file was found in the local UI
  checkout; if the UI is ever published, it needs its own license audit.

## 9. Full license text index

| SPDX / license | File in `LICENSES/` | Used by |
|---|---|---|
| `MIT` | `MIT.txt` | third-party MIT components: bitcoin, reproc, stduuid, GSL, cppcodec, uint128, scheduler, spdlog, jsoncpp (alternative), chachapoly wrapper, secp256k1-zkp, winpthreads, fmt, siphash (the project's own MIT grant is the root `LICENSE`) |
| `Apache-2.0` | `Apache-2.0.txt` | Rust grin 5.5.2 adaptation, RocksDB, CRoaring, CTPL |
| `BSD-2-Clause` | `BSD-2-Clause.txt` | scrypt, fmt (bundled with spdlog), obfs4proxy |
| `BSD-3-Clause` | `BSD-3-Clause.txt` | caches, ghc::filesystem, rfc6234, Tor, libevent, snowflake-client, RocksDB/LevelDB portions |
| `ISC` | `ISC.txt` | libsodium |
| `BSL-1.0` | `BSL-1.0.txt` | Catch2 2.4.2, 2.2.2 and 2.12.1; asio |
| `Zlib` | `Zlib.txt` | zlib 1.2.11, minizip, zlib 1.2.12 (in `dist/`); sha3 uses its own zlib-style text in `deps/sha3/LICENSE` |
| `OpenSSL` | `OpenSSL.txt` | OpenSSL 1.1.1q (OpenSSL License + original SSLeay License) |
| `CC-BY-SA-4.0` | `CC-BY-SA-4.0.txt` | IPFire Location database (`geoip`, `geoip6`) |
| `GPL-2.0-only` | `GPL-2.0-only.txt` | alternative offered by the FAIR MINING license; dual-license alternative for RocksDB |
| `GPL-3.0-only` | `GPL-3.0-only.txt` | mingw-w64 runtime (`libgcc_s_seh-1.dll`, `libssp-0.dll`, `libstdc++.so.6`), `GPL-3.0-or-later` |
| GCC Runtime Library Exception 3.1 | `GCC-exception-3.1.txt` | exception to the GPL for the GCC runtime libraries above |
| FAIR MINING License | `FAIR-MINING.txt` | Cuckoo PoW (`src/PoW`) |
| Tor composite notice | `Tor-LICENSE.txt` | Tor `LICENSE` with its bundled third-party notices |
| libevent notice | `libevent-LICENSE.txt` | libevent 2.1.12-stable |
| Public-domain statements | `Public-Domain-NOTICE.md` | ChaCha20, Poly1305, SQLite, jsoncpp public-domain option, portable_endian |
| Per-component upstream texts | `upstream/*` | exact upstream license files used for copyright lines; see `SOURCES.md` |

## 10. Verification status and remaining BLOCKED items

| Item | Status |
|---|---|
| Project `LICENSE` is MIT, Copyright (c) 2018–2019 David Burkett, retained | **PASS** |
| Every `deps/**` component has a license and a locatable notice | **PASS** (with the packaging defects noted in §2.2) |
| All vcpkg/sysroot static libraries have an established license and text | **PASS** (provenance note in §3) |
| `dist/**` Tor bundle: Tor/OpenSSL/libevent/zlib/GeoIP licenses included | **PASS** |
| `dist/**` obfs4proxy and snowflake-client exact versions | **PARTIAL / NOT VERIFIED** — versions not embedded in the binaries; licenses established, texts included |
| `dist/mac/tor` exact OpenSSL version | **NOT VERIFIED** — statically embedded, version string not present; the license terms are the same OpenSSL + SSLeay terms |
| Rust grin 5.5.2 Apache-2.0 attribution and license text | **PASS for this notices package**; file-level §4(c) headers remain **BLOCKED** (source edit) |
| MSVC runtime not redistributed; final EXE static `/MT` | **PASS** (import table verified OS-DLL-only) |
| Catch2 BSL-1.0 for `Tests.exe` | **PASS** |
| `Logo.png` separate license | **BLOCKED / PARTIAL** — no separate license; treated as upstream project asset under repo MIT; Lead should confirm before publication |
| RC1 `fix4node.zip` Microsoft/RocksDB DLL notices | **BLOCKED if republished**; not part of this publication |
| Electron UI | Not distributed; needs its own audit if published |

### BLOCKED items requiring action before publication

1. **Apache-2.0 §4(c) file-level notices** — add a short header to each file listed in
   §5.2 stating that it is derived from / pinned to `grin` 5.5.2, Copyright The Grin
   Developers, Apache-2.0. This is a source edit and is outside this notices draft.
2. **`Logo.png`** — no separate license can be established from the repository. Keep it
   as an upstream project asset with the upstream copyright, or remove it; the Lead must
   decide and record the decision.
3. **RC1 package / MSVC DLLs** — if `Beta1/RC1/fix4node.zip` (or any package containing
   `msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll`, `rocksdb-shared.dll`) is ever
   republished, add the Microsoft Visual Studio Distributable Code notice and the RocksDB
   Apache-2.0 notice. It is not part of this publication.
4. **`GrinPlusPlusUI`** — not distributed; if it is published separately it needs its own
   license audit (no LICENSE file was found locally).

## Appendix A — Apache License, Version 2.0

The complete text below is the Apache License, Version 2.0, reproduced verbatim from
<https://www.apache.org/licenses/LICENSE-2.0.txt> (retrieved 2026-11-06; SHA-256
`CFC7749B96F63BD31C3C42B5C471BF756814053E847C10F3EB003417BC523D30`). The same text is
shipped as `LICENSES/Apache-2.0.txt`.


                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

