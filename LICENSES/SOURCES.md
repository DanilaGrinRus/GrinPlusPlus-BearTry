# SOURCES — provenance of the license texts

All texts in this directory are verbatim copies. `SHA-256` is the hash of the **file as
stored here** (bytes on disk, as written by the retrieval/copy step). Retrieval date for
all network fetches: **2026-11-06** (Asia/Bangkok). Network texts were downloaded with
PowerShell `Invoke-WebRequest -UseBasicParsing` and hashed with `Get-FileHash -Algorithm
SHA256`; no text was retyped, reformatted or paraphrased.

## 1. Canonical texts (top-level `LICENSES/*.txt`)

| File | Source URL | Retrieved | Bytes | SHA-256 |
|---|---|---|---|---|
| `Apache-2.0.txt` | https://www.apache.org/licenses/LICENSE-2.0.txt (official ASF) | 2026-11-06 | 11,358 | `CFC7749B96F63BD31C3C42B5C471BF756814053E847C10F3EB003417BC523D30` |
| `MIT.txt` | https://raw.githubusercontent.com/spdx/license-list-data/main/text/MIT.txt (SPDX canonical text) | 2026-11-06 | 1,078 | `B05785F9F18E6716BAB63424B11454513B9943A222595B70411009202FC592B5` |
| `BSD-2-Clause.txt` | https://raw.githubusercontent.com/spdx/license-list-data/main/text/BSD-2-Clause.txt | 2026-11-06 | 1,267 | `F32FB3B417A194167CFAD068223FC975BA96C5960513A10F66A3C28720AEC1DF` |
| `BSD-3-Clause.txt` | https://raw.githubusercontent.com/spdx/license-list-data/main/text/BSD-3-Clause.txt | 2026-11-06 | 1,460 | `5A93D5831E1297AB10FE643E1A631E83BE392896DA14EE2951285A79012DF69D` |
| `ISC.txt` | https://raw.githubusercontent.com/spdx/license-list-data/main/text/ISC.txt | 2026-11-06 | 737 | `521C6F0ED8E64736684F89843D16A4DF6B4B7449ACBCFC6CA6630A3037CA8C53` |
| `BSL-1.0.txt` | https://raw.githubusercontent.com/spdx/license-list-data/main/text/BSL-1.0.txt | 2026-11-06 | 1,338 | `84C6EF3EA9E3254A54D0ACF5D3E0C61AE011B8FEF7DD6940591CF060E6380A8F` |
| `Zlib.txt` | https://raw.githubusercontent.com/spdx/license-list-data/main/text/Zlib.txt | 2026-11-06 | 838 | `BFB1112D49DB5B1DAECDFEF24BD7E2F3EA0BAFB33AA67AA0AB51E2BF8407C03D` |
| `GPL-2.0-only.txt` | https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt (official GNU) | 2026-11-06 | 17,984 | `EDAEF632CBB643E4E7A221717A6C441A4C1A7C918E6E4D56DEBC3D8739B233F6` |
| `GPL-3.0-only.txt` | https://www.gnu.org/licenses/gpl-3.0.txt (official GNU) | 2026-11-06 | 35,149 | `3972DC9744F6499F0F9B2DBF76696F2AE7AD8AF9B23DDE66D6AF86C9DFB36986` |
| `GCC-exception-3.1.txt` | https://raw.githubusercontent.com/spdx/license-list-data/main/text/GCC-exception-3.1.txt — see note 1 | 2026-11-06 | 3,390 | `7103D4F7F7E2F8CE10D282A05E0689637F8D6D9EF7B399D808D1DA313E69B960` |
| `OpenSSL.txt` | https://raw.githubusercontent.com/openssl/openssl/OpenSSL_1_1_1q/LICENSE (tag matching OpenSSL 1.1.1q) | 2026-11-06 | 6,121 | `C32913B33252E71190AF2066F08115C69BC9FDDADF3BF29296E20C835389841C` |
| `CC-BY-SA-4.0.txt` | https://creativecommons.org/licenses/by-sa/4.0/legalcode.txt (official Creative Commons) | 2026-11-06 | 20,138 | `28A9529C7D0BB4DC51F4BF5C116A3D16EF247A052F7591466768DDF563FD1CF5` |
| `Tor-LICENSE.txt` | https://raw.githubusercontent.com/torproject/tor/tor-0.4.7.10/LICENSE | 2026-11-06 | 19,651 | `47B54ED17E8FDCAB3C44729A1789A09B208F9A63A845A7E50DEF9DF729EEBAD0` |
| `libevent-LICENSE.txt` | https://raw.githubusercontent.com/libevent/libevent/release-2.1.12-stable/LICENSE | 2026-11-06 | 4,528 | `FF02EFFC9B331EDCDAC387D198691BFA3E575E7D244AD10CB826AA51EF085670` |

**Note 1 — GCC exception URL:** `https://www.gnu.org/licenses/gcc-exception-3.1.txt`
returned HTTP 404 on 2026-11-06. The canonical text was therefore taken from the SPDX
license-list-data repository (the same text, `GCC RUNTIME LIBRARY EXCEPTION Version 3.1,
31 March 2009`). The gnu.org HTML page is `https://www.gnu.org/licenses/gcc-exception-3.1.html`
(not used, to avoid HTML-to-text conversion of license text).

## 2. Copied local texts (top-level)

| File | Source path on this host | Bytes | SHA-256 |
|---|---|---|---|
| `FAIR-MINING.txt` | `repository/src/PoW/LICENSE.txt` (verbatim copy) | 1,532 | `97303485BF2B4F50CE083B0F956BE871BB1FCBD72BC3D7249B48FCF2F11E5840` |

## 3. Per-component upstream texts (`LICENSES/upstream/`)

| File | Source URL | Retrieved | Bytes | SHA-256 |
|---|---|---|---|---|
| `libsodium-LICENSE.txt` | https://raw.githubusercontent.com/jedisct1/libsodium/1.0.18/LICENSE | 2026-11-06 | 823 | `DEA1855C9809F3FAF22AA4A1FBA20EC8AF5A5587F23115012E5B98279CEDC4AF` |
| `rocksdb-LICENSE.Apache.txt` | https://raw.githubusercontent.com/facebook/rocksdb/v6.6.4/LICENSE.Apache | 2026-11-06 | 11,358 | `CFC7749B96F63BD31C3C42B5C471BF756814053E847C10F3EB003417BC523D30` |
| `rocksdb-LICENSE.leveldb.txt` | https://raw.githubusercontent.com/facebook/rocksdb/v6.6.4/LICENSE.leveldb | 2026-11-06 | 1,572 | `B568F1F37A5A1D37A3E981E7C8B7FC4856CA33B2CA0A45BC8E847AAAF7AFE485` |
| `zlib-README.txt` | https://raw.githubusercontent.com/madler/zlib/v1.2.11/README (contains the zlib license; zlib has no separate LICENSE file at this tag) | 2026-11-06 | 5,187 | `7960B6B1CC63E619ABB77ACAEA5427159605AFEE8C8B362664F4EFFC7D7F7D15` |
| `civetweb-LICENSE.md` | https://raw.githubusercontent.com/civetweb/civetweb/v1.12/LICENSE.md | 2026-11-06 | 10,047 | `291622DFBBF4FB35829726FBE97177B7227C0D4D6392B350660AF73AF4F86DF8` |
| `croaring-LICENSE.txt` | https://raw.githubusercontent.com/RoaringBitmap/CRoaring/v0.2.66/LICENSE | 2026-11-06 | 11,352 | `5B223235C64F595576D15BA2D983EF14F8E9E3EC45AAFCAD689DC40008E1B29E` |
| `asio-COPYING.txt` | https://raw.githubusercontent.com/chriskohlhoff/asio/asio-1-12-2/asio/COPYING | 2026-11-06 | 224 | `F41DF74D893C6078F697413BE45147A42D8FF360517FE2AFB8EFED4BA3B53C55` — see note 2 |
| `fmt-6.2.1-LICENSE.rst` | https://raw.githubusercontent.com/fmtlib/fmt/6.2.1/LICENSE.rst | 2026-11-06 | 1,408 | `825C9324E70F8C839C8BA910543DD4A7DAEE243B86EF960594C11381A19980B8` |
| `catch2-2.12.1-LICENSE.txt` | https://raw.githubusercontent.com/catchorg/Catch2/v2.12.1/LICENSE.txt | 2026-11-06 | 1,338 | `C9BFF75738922193E67FA726FA225535870D2AA1059F91452C411736284AD566` |
| `secp256k1-zkp-COPYING.txt` | https://raw.githubusercontent.com/mimblewimble/secp256k1-zkp/5b39fbf7f1e16e126404791923511723c4c0876d/COPYING | 2026-11-06 | 1,057 | `A735999C7E5649DF6FCDA6FB06AB97435851C392B1B93494AE8725F37441632F` |
| `spdlog-0.17.0-LICENSE.txt` | https://raw.githubusercontent.com/gabime/spdlog/v0.17.0/LICENSE | 2026-11-06 | 1,142 | `7D40B1967FCA203D22B0EAF75F54566167DED823863ABEDF4DE869FCEA057038` |
| `winpthreads-COPYING.txt` | https://raw.githubusercontent.com/mingw-w64/mingw-w64/master/mingw-w64-libraries/winpthreads/COPYING — see note 3 | 2026-11-06 | 2,883 | `63263614CDD29F2F93CBA85E992F041B31F9FC7B4033692F31269489A8A1B177` |
| `obfs4-LICENSE.txt` | https://gitlab.com/yawning/obfs4/-/raw/master/LICENSE — see note 4 | 2026-11-06 | 2,895 | `E8FD2E9F9AA4FD960C2B55E72E45FD1DB156912F6ACF5AA5D0A21FEC0A481390` |
| `snowflake-LICENSE.txt` | Go module proxy: https://proxy.golang.org/git.torproject.org/pluggable-transports/snowflake.git/v2/@v/v2.3.0.zip → member `…@v2.3.0/LICENSE` — see note 4 | 2026-11-06 | 1,822 | `C5BEEE15F5CF06F1A779DC3FFF8F31137A153E73865DAA65206D05483C255C88` |
| `minizip-LICENSE.txt` | copied from the local dependency sysroot (`include/minizip/unzip.h`, exact notice block; sysroot manifest SHA-256 `82FBF0F7F2D5E5BFFB8C049479E677A10F47177E6A55459269960EAD54EC5C53`) | 2026-11-06 | 1,732 | `C9C39961A41F7EBD1E414FFB430835D909751FAA24B75416454F91A090496CED` |
| `mio-LICENSE.txt` | copied from the local sysroot: `…\sysroot\include\mio\mio.hpp` (lines 1–19, exact MIT notice block) | 2026-11-06 | 1,142 | `822934B4BF1874D4D0F7747BC92B4D3843DAF5DA9C70E3216656EA034C14CDCB` |

**Note 2 — asio:** the text stored is the upstream `asio/COPYING` pointer file
(`Copyright (c) 2003-2018 Christopher M. Kohlhoff … Distributed under the Boost Software
License, Version 1.0`, 224 bytes). The full BSL-1.0 text is `../BSL-1.0.txt`.

**Note 3 — winpthreads:** the mingw-w64 *project-wide* `COPYING` is Zope Public License
2.1, but that is **not** the license of the shipped `libwinpthread-1.dll`. The
winpthreads subproject has its own MIT `COPYING`, which is the text stored here. The
exact winpthreads revision used by the GCC 10.3.0 toolchain is not embedded in the DLL
(PARTIAL); the license text is stable across revisions.

**Note 4 — obfs4proxy / snowflake-client:** the binaries do not embed a version string.
`obfs4` was fetched from the current upstream `master`, and `snowflake` from Go module
`v2.3.0` through the official Go module proxy; these revision identifiers are recorded
for provenance but are **not** claims about the exact revisions linked into
`dist/**`. The license identities (BSD-2-Clause and BSD-3-Clause respectively) and the
copyright lines are stable; the exact bundled revisions remain **NOT VERIFIED**.

## 4. Verification-only artefacts (not shipped as license texts)

These were inspected to verify the audit's claims; they are not duplicated in this
package.

| Artefact | Location / URL | Bytes | SHA-256 / evidence |
|---|---|---|---|
| Project `LICENSE` (CRLF working-tree copy) | `repository/LICENSE` | 1,104 | `8C68E9882A81FDBD2D495D45552E91F0DF8435EC23956F0FD99F0273442578B6` |
| Project `LICENSE` (canonical LF blob) | upstream v1.2.9 git blob | 1,084 | `A5E9383C3CB97AA3034E5E4BF1C94A71DB0C59B3A7EC1FBF198232FB9DCC5E53` (from the baseline audit) |
| Grin 5.5.2 source archive | local reference archive `grin-5.5.2.zip` (not published) | 7,974,593 | `C60BE3BB38C66FCAD3BC9D057F8C98D8A193D8B65E44AFC71F43E4A270FD9349` |
| Grin 5.5.2 `LICENSE` member | `grin-5.5.2/LICENSE` in the zip | 10,174 | `0D542E0C8804E39AA7F37EB00DA5A762149DC682D7829451287E11B938E94594`; first 177 lines are byte-identical to `Apache-2.0.txt` after newline normalisation; the archive copy omits only the non-normative appendix |
| Grin 5.5.2 `NOTICE` | archive | — | **absent** (no NOTICE file in the archive) |
| `Logo.png` | `repository/Logo.png` | 10,732 | `52658BE6CEFE92685411A733D044C19306B0456B41797904A849498FE7471D92` |
| `src/PoW/LICENSE.txt` | `repository/src/PoW/LICENSE.txt` | 1,532 | `97303485BF2B4F50CE083B0F956BE871BB1FCBD72BC3D7249B48FCF2F11E5840` |
| Tor version string | `dist/win|linux/tor/tor` | — | `tor 0.4.7.10` (ASCII strings) |
| Tor version string (mac) | `dist/mac/tor/tor` | — | `0.4.7.9` |
| OpenSSL version string | `dist/win/tor/libcrypto-1_1-x64.dll`, `dist/linux/tor/libcrypto.so.1.1` | — | `OpenSSL 1.1.1q  5 Jul 2022` |
| libevent version string | `dist/win/tor/libevent_core-2-1-7.dll` | — | `2.1.12-stable` |
| GCC version string | `dist/win/tor/libgcc_s_seh-1.dll` | — | `GCC: (GNU) 10.3.0` |
| GeoIP header | `dist/win/tor/geoip` | — | “converted from the IPFire Location database … Vendor: IPFire Project … License: CC BY-SA 4.0 … Generated: Tue, 09 Aug 2022” |
| Final EXE import table | `release-staging/release/GrinNode.exe` and `Tests.exe` (llvm-objdump -p) | — | OS DLLs only: SHLWAPI, RPCRT4, ADVAPI32, KERNEL32, ole32, WS2_32, MSWSOCK, bcrypt |
| Link line | `release-staging/clean-build/build/src/Server/GrinNode.vcxproj` | — | contains libsodium, rocksdb, zlibstatic, minizip, roaring, civetweb, secp256k1-zkp; **not** fmt, **not** Catch2 |

## 5. Integrity manifest (authoritative hashes of all shipped texts)

Use this table to verify the package after copying it into the repository. It is
regenerated from the files on disk at the end of the preparation.

```text
CFC7749B96F63BD31C3C42B5C471BF756814053E847C10F3EB003417BC523D30  11358  Apache-2.0.txt
F32FB3B417A194167CFAD068223FC975BA96C5960513A10F66A3C28720AEC1DF  1267  BSD-2-Clause.txt
5A93D5831E1297AB10FE643E1A631E83BE392896DA14EE2951285A79012DF69D  1460  BSD-3-Clause.txt
84C6EF3EA9E3254A54D0ACF5D3E0C61AE011B8FEF7DD6940591CF060E6380A8F  1338  BSL-1.0.txt
28A9529C7D0BB4DC51F4BF5C116A3D16EF247A052F7591466768DDF563FD1CF5  20138  CC-BY-SA-4.0.txt
97303485BF2B4F50CE083B0F956BE871BB1FCBD72BC3D7249B48FCF2F11E5840  1532  FAIR-MINING.txt
7103D4F7F7E2F8CE10D282A05E0689637F8D6D9EF7B399D808D1DA313E69B960  3390  GCC-exception-3.1.txt
EDAEF632CBB643E4E7A221717A6C441A4C1A7C918E6E4D56DEBC3D8739B233F6  17984  GPL-2.0-only.txt
3972DC9744F6499F0F9B2DBF76696F2AE7AD8AF9B23DDE66D6AF86C9DFB36986  35149  GPL-3.0-only.txt
521C6F0ED8E64736684F89843D16A4DF6B4B7449ACBCFC6CA6630A3037CA8C53  737  ISC.txt
FF02EFFC9B331EDCDAC387D198691BFA3E575E7D244AD10CB826AA51EF085670  4528  libevent-LICENSE.txt
B05785F9F18E6716BAB63424B11454513B9943A222595B70411009202FC592B5  1078  MIT.txt
C32913B33252E71190AF2066F08115C69BC9FDDADF3BF29296E20C835389841C  6121  OpenSSL.txt
4E86610280D5D8AE8CD1E02B83B2C6978142B8AADDEFBA425B6CC7F21D8E02AE  2800  Public-Domain-NOTICE.md
47B54ED17E8FDCAB3C44729A1789A09B208F9A63A845A7E50DEF9DF729EEBAD0  19651  Tor-LICENSE.txt
F41DF74D893C6078F697413BE45147A42D8FF360517FE2AFB8EFED4BA3B53C55  224  upstream/asio-COPYING.txt
C9BFF75738922193E67FA726FA225535870D2AA1059F91452C411736284AD566  1338  upstream/catch2-2.12.1-LICENSE.txt
291622DFBBF4FB35829726FBE97177B7227C0D4D6392B350660AF73AF4F86DF8  10047  upstream/civetweb-LICENSE.md
5B223235C64F595576D15BA2D983EF14F8E9E3EC45AAFCAD689DC40008E1B29E  11352  upstream/croaring-LICENSE.txt
825C9324E70F8C839C8BA910543DD4A7DAEE243B86EF960594C11381A19980B8  1408  upstream/fmt-6.2.1-LICENSE.rst
DEA1855C9809F3FAF22AA4A1FBA20EC8AF5A5587F23115012E5B98279CEDC4AF  823  upstream/libsodium-LICENSE.txt
C9C39961A41F7EBD1E414FFB430835D909751FAA24B75416454F91A090496CED  1732  upstream/minizip-LICENSE.txt
822934B4BF1874D4D0F7747BC92B4D3843DAF5DA9C70E3216656EA034C14CDCB  1142  upstream/mio-LICENSE.txt
E8FD2E9F9AA4FD960C2B55E72E45FD1DB156912F6ACF5AA5D0A21FEC0A481390  2895  upstream/obfs4-LICENSE.txt
CFC7749B96F63BD31C3C42B5C471BF756814053E847C10F3EB003417BC523D30  11358  upstream/rocksdb-LICENSE.Apache.txt
B568F1F37A5A1D37A3E981E7C8B7FC4856CA33B2CA0A45BC8E847AAAF7AFE485  1572  upstream/rocksdb-LICENSE.leveldb.txt
A735999C7E5649DF6FCDA6FB06AB97435851C392B1B93494AE8725F37441632F  1057  upstream/secp256k1-zkp-COPYING.txt
C5BEEE15F5CF06F1A779DC3FFF8F31137A153E73865DAA65206D05483C255C88  1822  upstream/snowflake-LICENSE.txt
7D40B1967FCA203D22B0EAF75F54566167DED823863ABEDF4DE869FCEA057038  1142  upstream/spdlog-0.17.0-LICENSE.txt
63263614CDD29F2F93CBA85E992F041B31F9FC7B4033692F31269489A8A1B177  2883  upstream/winpthreads-COPYING.txt
7960B6B1CC63E619ABB77ACAEA5427159605AFEE8C8B362664F4EFFC7D7F7D15  5187  upstream/zlib-README.txt
BFB1112D49DB5B1DAECDFEF24BD7E2F3EA0BAFB33AA67AA0AB51E2BF8407C03D  838  Zlib.txt
```
