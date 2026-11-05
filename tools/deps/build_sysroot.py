#!/usr/bin/env python3
"""Build a local dependency sysroot for Grin++ (Windows x64, MSVC /MT).

PORTABLE PORT (preparation draft)
---------------------------------
This is a portable rewrite of the preparation-time helper that produced the
published dependency sysroot (746 files, manifest SHA-256 82FBF0F7...).
Two problems in the original helper make it unpublishable: it hard-codes
private absolute workspace paths and it reads helper files from a local vcpkg
checkout. This draft removes both. What it preserves:

* the same pinned dependency revisions (see ``DEPS`` below);
* the same static-CRT configuration (``/MT``, ``CMAKE_MSVC_RUNTIME_LIBRARY``);
* the same "install everything into one prefix" layout and the same
  hand-written CMake package config files that Grin++ expects;
* header-only installs for fmt/asio/mio/Catch2, compiled installs for
  zlib/minizip/libsodium/secp256k1-zkp/rocksdb/civetweb/roaring.

VERIFIED end-to-end on 2026-11-06 (Windows 11 x64, VS 2022 BuildTools):
``python tools/deps/build_sysroot.py --repo <repo> --toolchain-dir <work> \
--sysroot-dir <prefix>`` exits 0 and installs 743 files; manifest
``0373CBD068518863DC7B16CB3DB56B6317F7682B7E8FE5B06549176B5E5EFA30``.
The rebuild is functionally equivalent to the published 746-file sysroot but
NOT byte-identical: 730 files are identical, 13 are compiled artifacts
(libs/DLLs, zlib.pc, two package configs) that embed build timestamps/paths,
and 3 published files are absent (the unused ``include/mio/mio.hpp`` and the
RocksDB config files, which this helper installs to the standard
``lib/cmake/rocksdb`` instead of ``share/RocksDB``). A project built against
the rebuilt sysroot compiles and passes its test suite (see reports).

Layout (defaults):
    <repo>\\..\\deps-toolchain\\sources      downloaded archives
    <repo>\\..\\deps-toolchain\\dep-build    per-dependency build trees
    <repo>\\..\\deps-toolchain\\logs         logs + sysroot.sha256
    <repo>\\..\\deps-sysroot                 the install prefix

Usage (run from the repository root, or pass --repo):
    python tools/deps/build_sysroot.py --dry-run
    python tools/deps/build_sysroot.py
    python tools/deps/build_sysroot.py --only zlib rocksdb
    python tools/deps/build_sysroot.py --sysroot-dir D:\\deps\\sysroot

Then configure the node with:
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
          -DCMAKE_BUILD_TYPE=Release
          -DCMAKE_PREFIX_PATH=D:\\deps\\sysroot
          -DGRINPP_TESTS=ON -DGRINPP_TOOLS=OFF
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

# name -> (url, archive-root-fragment)
# Pinned exactly as used for the published sysroot. Do not float these refs.
DEPS = {
    "zlib": ("https://github.com/madler/zlib/archive/refs/tags/v1.2.11.tar.gz", "zlib-1.2.11"),
    "minizip_src": ("https://github.com/madler/zlib/archive/refs/tags/v1.2.11.tar.gz", "zlib-1.2.11"),
    "libsodium": ("https://github.com/jedisct1/libsodium/releases/download/1.0.18-RELEASE/libsodium-1.0.18.tar.gz", "libsodium-1.0.18"),
    "secp256k1-zkp": ("https://github.com/mimblewimble/secp256k1-zkp/archive/5b39fbf7f1e16e126404791923511723c4c0876d.tar.gz", "secp256k1-zkp-5b39fbf7f1e16e126404791923511723c4c0876d"),
    "rocksdb": ("https://github.com/facebook/rocksdb/archive/551a110918493a19d11243f53408b97485de1411.tar.gz", "rocksdb-551a110918493a19d11243f53408b97485de1411"),
    "civetweb": ("https://github.com/civetweb/civetweb/archive/2c1caa6e690bfe3b435a10c372ab2dcd14b872e8.tar.gz", "civetweb-2c1caa6e690bfe3b435a10c372ab2dcd14b872e8"),
    "roaring": ("https://github.com/RoaringBitmap/CRoaring/archive/refs/tags/v0.2.66.tar.gz", "CRoaring-0.2.66"),
    "fmt": ("https://github.com/fmtlib/fmt/archive/refs/tags/6.2.1.tar.gz", "fmt-6.2.1"),
    "asio": ("https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-12-2.tar.gz", "asio-asio-1-12-2"),
    "mio": ("https://github.com/mandreyel/mio/archive/cafa31360fee8866be89b4c602d8b9a7a18dbf5e.tar.gz", "mio-cafa31360fee8866be89b4c602d8b9a7a18dbf5e"),
    "catch2": ("https://github.com/catchorg/Catch2/archive/refs/tags/v2.12.1.tar.gz", "Catch2-2.12.1"),
}

ORDER = ["fmt", "asio", "mio", "catch2", "zlib", "minizip", "libsodium",
         "secp256k1-zkp", "rocksdb", "civetweb", "roaring"]

# Filled in by main(); module-level so the step functions stay close to the
# historical helper and easy to diff against it.
REPO = ""
TOOLCHAIN = ""
SYSROOT = ""
SOURCES = ""
BUILDS = ""
LOGDIR = ""
TMPDIR = ""
CMAKE = ""
MSBUILD = ""
MINIZIP_PORT_DIR = ""
ROCKSDB_UTF8 = True
DRY_RUN = False

COMMON_DEFS = [
    "-G", "Visual Studio 17 2022", "-A", "x64",
    # Several pinned dependencies declare cmake_minimum_required < 3.15, so the
    # CMP0091 policy leaves CMAKE_MSVC_RUNTIME_LIBRARY unused and CMake injects
    # /MD through the default flags. Grin++ links a static CRT, therefore the
    # per-config flags are overridden explicitly as well.
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DCMAKE_C_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG",
    "-DCMAKE_CXX_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG",
    "-DCMAKE_C_FLAGS_DEBUG=/MTd /Zi /Ob0 /Od /RTC1",
    "-DCMAKE_CXX_FLAGS_DEBUG=/MTd /Zi /Ob0 /Od /RTC1",
]

_log_handle = None


def log(msg):
    line = str(msg)
    print(line, flush=True)
    if _log_handle is not None:
        _log_handle.write(line + "\n")
        _log_handle.flush()


def run(cmd):
    log("+ " + " ".join(cmd))
    if DRY_RUN:
        log("    [dry-run] not executed")
        return ""
    proc = subprocess.run(cmd, capture_output=True, text=False)
    out = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace")
    lines = out.splitlines()
    limit = 40 if proc.returncode == 0 else 300
    for line in lines[-limit:]:
        log("    " + line)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    return out


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(name):
    url, _ = DEPS[name]
    os.makedirs(SOURCES, exist_ok=True)
    ext = ".tar.gz" if url.endswith(".tar.gz") else ".zip"
    path = os.path.join(SOURCES, name + ext)
    if not (os.path.exists(path) and os.path.getsize(path) > 0):
        if DRY_RUN:
            log(f"[get] [dry-run] {name} <- {url}")
            return path
        log(f"[get] {name} <- {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "grinpp-sysroot/1.0"})
        with urllib.request.urlopen(req, timeout=180) as resp, open(path + ".part", "wb") as out:
            shutil.copyfileobj(resp, out)
        os.replace(path + ".part", path)
    if not DRY_RUN:
        digest = sha256(path)
        log(f"[src] {name} sha256={digest} size={os.path.getsize(path)}")
        manifest = os.path.join(SOURCES, "SOURCES.sha256")
        line = f"{digest}  {name}{ext}\n"
        existing = ""
        if os.path.exists(manifest):
            with open(manifest, encoding="ascii") as f:
                existing = f.read()
        if line not in existing:
            with open(manifest, "a", encoding="ascii") as f:
                f.write(line)
    return path


def extract(name, dest_name=None):
    url, rootfrag = DEPS[name]
    path = fetch(name)
    dest = os.path.join(SOURCES, "extracted", dest_name or name)
    target = os.path.join(dest, rootfrag)
    if DRY_RUN:
        log(f"[extract] [dry-run] {name} -> {target}")
        return target
    os.makedirs(dest, exist_ok=True)
    if os.path.isdir(target):
        return target
    log(f"[extract] {name}")
    with tarfile.open(path, "r:gz") as tar:
        try:
            tar.extractall(dest, filter="data")  # Python >= 3.12
        except TypeError:
            tar.extractall(dest)
    if not os.path.isdir(target):
        dirs = [d for d in os.listdir(dest) if os.path.isdir(os.path.join(dest, d))]
        raise RuntimeError(f"unexpected archive layout for {name}: {dirs[:5]}")
    return target


def cmake_build(name, source, extra_defs, build_subdir=None, targets=("install",)):
    build = os.path.join(BUILDS, build_subdir or name)
    cache = os.path.join(build, "CMakeCache.txt")
    if os.path.exists(cache) and not DRY_RUN:
        with open(cache, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if f"CMAKE_HOME_DIRECTORY:INTERNAL={source}" not in text:
            log(f"[reset] {build} (cached for another source tree)")
            shutil.rmtree(build, ignore_errors=True)
    if not DRY_RUN:
        os.makedirs(build, exist_ok=True)
    run([CMAKE, "-S", source, "-B", build,
         f"-DCMAKE_INSTALL_PREFIX={SYSROOT}",
         f"-DCMAKE_PREFIX_PATH={SYSROOT}"] + COMMON_DEFS + list(extra_defs))
    # Build the install target through MSBuild directly: `cmake --build` does not
    # reliably forward the file-tracker switch here, and the tracker can fail
    # with TRK0002 in restricted shells (host limitation, not a project change).
    msb_log = os.path.join(LOGDIR, f"msbuild-{name}.log")
    # /nr:false + MSBUILDDISABLENODEREUSE: orphaned MSBuild worker nodes left by
    # an interrupted build make later invocations fail silently (rc=1, no
    # diagnostics). No /m: parallel MSBuild uses named pipes for its worker
    # nodes; keep the dependency build serial so it works in restricted shells.
    os.environ["MSBUILDDISABLENODEREUSE"] = "1"
    run([MSBUILD, os.path.join(build, "install.vcxproj"),
         "/p:Configuration=Release", "/p:Platform=x64",
         "/p:TrackFileAccess=false", "/p:UseSharedCompilation=false",
         "/nr:false", "/v:m", "/nologo",
         f"/flp:LogFile={msb_log};Verbosity=normal"])
    return build


def write(path, text):
    log(f"[write] {path}")
    if DRY_RUN:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def copy_helper(src, dst):
    log(f"[copy] {src} -> {dst}")
    if not DRY_RUN:
        shutil.copy2(src, dst)


# ---------------------------------------------------------------- header-only

FMT_CONFIG = """
get_filename_component(_fmt_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if (NOT TARGET fmt::fmt-header-only)
  add_library(fmt::fmt-header-only INTERFACE IMPORTED)
  set_target_properties(fmt::fmt-header-only PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_fmt_prefix}/include"
    INTERFACE_COMPILE_DEFINITIONS "FMT_HEADER_ONLY=1")
endif()
if (NOT TARGET fmt::fmt)
  add_library(fmt::fmt INTERFACE IMPORTED)
  set_target_properties(fmt::fmt PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_fmt_prefix}/include"
    INTERFACE_COMPILE_DEFINITIONS "FMT_HEADER_ONLY=1")
endif()
set(fmt_FOUND TRUE)
"""

ASIO_CONFIG = """
get_filename_component(_asio_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if (NOT TARGET asio::asio)
  add_library(asio::asio INTERFACE IMPORTED)
  set_target_properties(asio::asio PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_asio_prefix}/include"
    INTERFACE_COMPILE_DEFINITIONS "ASIO_STANDALONE=1;ASIO_NO_DEPRECATED=1")
endif()
set(ASIO_INCLUDE_DIR "${_asio_prefix}/include")
set(asio_FOUND TRUE)
"""

MIO_CONFIG = """
get_filename_component(_mio_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if (NOT TARGET mio::mio)
  add_library(mio::mio INTERFACE IMPORTED)
  set_target_properties(mio::mio PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_mio_prefix}/include")
endif()
set(mio_FOUND TRUE)
"""

CATCH2_CONFIG = """
get_filename_component(_catch_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if (NOT TARGET Catch2::Catch2)
  add_library(Catch2::Catch2 INTERFACE IMPORTED)
  set_target_properties(Catch2::Catch2 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_catch_prefix}/include/catch2")
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
set(Catch2_FOUND TRUE)
set(CATCH_INCLUDE_DIR "${_catch_prefix}/include/catch2")
"""


def install_fmt():
    src = extract("fmt")
    dst = os.path.join(SYSROOT, "include")
    if not DRY_RUN:
        os.makedirs(dst, exist_ok=True)
        shutil.copytree(os.path.join(src, "include", "fmt"), os.path.join(dst, "fmt"), dirs_exist_ok=True)
    write(os.path.join(SYSROOT, "share", "fmt", "fmt-config.cmake"), FMT_CONFIG)


def install_asio():
    src = extract("asio")
    # Grin++ includes <asio.hpp> without linking an asio target, so a compile
    # definition alone is not enough. The vcpkg port rewrites the standalone
    # check in config.hpp; do the same here (ASIO never uses Boost here).
    config_hpp = os.path.join(src, "asio", "include", "asio", "detail", "config.hpp")
    if DRY_RUN:
        log("[patch] [dry-run] asio: standalone mode default + unistd gate")
    else:
        with open(config_hpp, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if "defined(ASIO_STANDALONE)" in text:
            text = text.replace("defined(ASIO_STANDALONE)",
                                "!defined(GRINPP_ASIO_WITH_BOOST)", 1)
            with open(config_hpp, "w", encoding="utf-8") as f:
                f.write(text)
            log("[patch] asio: standalone mode default")
        with open(config_hpp, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if "#if defined(ASIO_HAS_UNISTD_H)\n# include <unistd.h>" in text:
            text = text.replace("#if defined(ASIO_HAS_UNISTD_H)\n# include <unistd.h>",
                                "#if defined(ASIO_HAS_UNISTD_H) && !defined(_WIN32)\n# include <unistd.h>", 1)
            with open(config_hpp, "w", encoding="utf-8") as f:
                f.write(text)
            log("[patch] asio: unistd include gated on non-Windows")
        dst = os.path.join(SYSROOT, "include")
        os.makedirs(dst, exist_ok=True)
        shutil.copytree(os.path.join(src, "asio", "include"), dst, dirs_exist_ok=True)
    write(os.path.join(SYSROOT, "share", "asio", "asio-config.cmake"), ASIO_CONFIG)


def install_mio():
    src = extract("mio")
    dst = os.path.join(SYSROOT, "include", "mio")
    if not DRY_RUN:
        # Grin++ includes <mio/mmap.hpp>; the multi-header layout is the one needed.
        multi = os.path.join(src, "include", "mio")
        if os.path.isdir(multi):
            shutil.copytree(multi, dst, dirs_exist_ok=True)
        else:
            single = os.path.join(src, "single_include", "mio")
            shutil.copytree(single, dst, dirs_exist_ok=True)
    write(os.path.join(SYSROOT, "share", "mio", "mioConfig.cmake"), MIO_CONFIG)


def install_catch2():
    src = extract("catch2")
    dst = os.path.join(SYSROOT, "include")
    if not DRY_RUN:
        shutil.copytree(os.path.join(src, "single_include", "catch2"), os.path.join(dst, "catch2"), dirs_exist_ok=True)
    modules = os.path.join(SYSROOT, "share", "Catch2")
    if not DRY_RUN:
        os.makedirs(modules, exist_ok=True)
        cmake_dir = os.path.join(src, "extras")
        if not os.path.isdir(cmake_dir):
            cmake_dir = os.path.join(src, "contrib")
        for f in os.listdir(cmake_dir):
            if f.endswith(".cmake"):
                shutil.copy2(os.path.join(cmake_dir, f), os.path.join(modules, f))
    write(os.path.join(modules, "Catch2Config.cmake"), CATCH2_CONFIG)


# ------------------------------------------------------------------ built deps

def build_zlib():
    src = extract("zlib")
    cmake_build("zlib", src, ["-DBUILD_SHARED_LIBS=OFF"])
    # zlib 1.2.11 always builds a shared flavour alongside the static one.
    # Grin++ links a static-CRT binary, so remove the DLL import library to keep
    # FindZLIB on zlibstatic.lib.
    if not DRY_RUN:
        for stale in (os.path.join(SYSROOT, "lib", "zlib.lib"),
                      os.path.join(SYSROOT, "bin", "zlib.dll"),
                      os.path.join(SYSROOT, "lib", "zlib.dll")):
            if os.path.exists(stale):
                os.remove(stale)
                log(f"[clean] {stale}")


def build_minizip():
    src = extract("minizip_src", dest_name="minizip")
    helper = MINIZIP_PORT_DIR
    copy_helper(os.path.join(helper, "CMakeLists.txt"), os.path.join(src, "CMakeLists.txt"))
    if not DRY_RUN:
        os.makedirs(os.path.join(src, "cmake"), exist_ok=True)
    copy_helper(os.path.join(helper, "minizipConfig.cmake.in"),
                os.path.join(src, "cmake", "minizipConfig.cmake.in"))
    cmake_build("minizip", src, ["-DDISABLE_INSTALL_TOOLS=ON"], build_subdir="minizip")


def build_libsodium():
    src = extract("libsodium")
    port = os.path.join(REPO, "vcpkg", "custom_ports", "libsodium")
    copy_helper(os.path.join(port, "CMakeLists.txt"), os.path.join(src, "CMakeLists.txt"))
    copy_helper(os.path.join(port, "sodiumConfig.cmake.in"), os.path.join(src, "sodiumConfig.cmake.in"))
    cmake_build("libsodium", src, ["-DBUILD_SHARED_LIBS=OFF"])


def build_secp256k1():
    src = extract("secp256k1-zkp")
    port = os.path.join(REPO, "vcpkg", "custom_ports", "secp256k1-zkp")
    copy_helper(os.path.join(port, "windows", "libsecp256k1-config.h"),
                os.path.join(src, "libsecp256k1-config.h"))
    copy_helper(os.path.join(port, "CMakeLists.txt"), os.path.join(src, "CMakeLists.txt"))
    cmake_build("secp256k1-zkp", src, ["-DINSTALL_HEADERS=ON"])


def build_rocksdb():
    src = extract("rocksdb")
    # rocksdb ships a Findzlib.cmake that resolves zlib from NuGet-style paths;
    # the vcpkg port removes it for exactly this reason (see portfile.cmake).
    stale_zlib_module = os.path.join(src, "cmake", "modules", "Findzlib.cmake")
    if not DRY_RUN and os.path.exists(stale_zlib_module):
        os.remove(stale_zlib_module)
        log(f"[clean] {stale_zlib_module}")
    # thirdparty.inc hardcodes NuGet-style paths (\ZLIB.Library\...). The vcpkg
    # port disables it via VCPKG_TOOLCHAIN; here it is replaced directly so that
    # find_package(ZLIB) resolves against the sysroot.
    if not DRY_RUN:
        cmake_lists = os.path.join(src, "CMakeLists.txt")
        with open(cmake_lists, encoding="utf-8", errors="replace") as f:
            text = f.read()
        thirdparty_include = "include(${CMAKE_CURRENT_SOURCE_DIR}/thirdparty.inc)"
        if thirdparty_include in text:
            text = text.replace(
                thirdparty_include,
                "# thirdparty.inc disabled: dependencies resolve via find_package(sysroot)")
            with open(cmake_lists, "w", encoding="utf-8") as f:
                f.write(text)
            log("[patch] rocksdb: thirdparty.inc disabled")
    cmake_build("rocksdb", src, [
        "-DROCKSDB_BUILD_SHARED=OFF",
        "-DWITH_GFLAGS=OFF",
        "-DWITH_TESTS=OFF",
        "-DWITH_TOOLS=OFF",
        "-DWITH_CORE_TOOLS=OFF",
        "-DWITH_BENCHMARK_TOOLS=OFF",
        "-DWITH_ZLIB=ON",
        "-DWITH_LZ4=OFF",
        "-DWITH_SNAPPY=OFF",
        "-DWITH_ZSTD=OFF",
        "-DWITH_TBB=OFF",
        # The published sysroot used a RocksDB rebuilt for Windows UTF-8 paths
        # (the vcpkg port sets WITH_WINDOWS_UTF8_FILENAMES=TRUE and PORTABLE).
        f"-DWITH_WINDOWS_UTF8_FILENAMES={'ON' if ROCKSDB_UTF8 else 'OFF'}",
        "-DPORTABLE=ON",
        "-DROCKSDB_INSTALL_ON_WINDOWS=ON",
        # rocksdb defaults to /MD; the node links a static CRT (/MT).
        "-DWITH_MD_LIBRARY=OFF",
        "-DFAIL_ON_WARNINGS=OFF",
        "-DUSE_RTTI=ON",
    ])


CIVETWEB_CONFIG = """
get_filename_component(_cw_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_library(_cw_lib NAMES civetweb HINTS "${_cw_prefix}/lib" NO_DEFAULT_PATH)
find_library(_cw_cpp NAMES civetweb-cpp HINTS "${_cw_prefix}/lib" NO_DEFAULT_PATH)
if (NOT TARGET civetweb::civetweb)
  add_library(civetweb::civetweb UNKNOWN IMPORTED)
  set_target_properties(civetweb::civetweb PROPERTIES
    IMPORTED_LOCATION "${_cw_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_cw_prefix}/include")
endif()
if (_cw_cpp AND NOT TARGET civetweb::civetweb-cpp)
  add_library(civetweb::civetweb-cpp UNKNOWN IMPORTED)
  set_target_properties(civetweb::civetweb-cpp PROPERTIES
    IMPORTED_LOCATION "${_cw_cpp}"
    INTERFACE_INCLUDE_DIRECTORIES "${_cw_prefix}/include"
    INTERFACE_LINK_LIBRARIES "civetweb::civetweb")
endif()
set(civetweb_FOUND TRUE)
"""

ROARING_CONFIG = """
get_filename_component(_roar_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_library(_roar_lib NAMES roaring HINTS "${_roar_prefix}/lib" NO_DEFAULT_PATH)
if (NOT TARGET roaring::roaring)
  add_library(roaring::roaring UNKNOWN IMPORTED)
  set_target_properties(roaring::roaring PROPERTIES
    IMPORTED_LOCATION "${_roar_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_roar_prefix}/include")
endif()
set(roaring_FOUND TRUE)
"""


def build_civetweb():
    src = extract("civetweb")
    cmake_build("civetweb", src, [
        "-DCIVETWEB_BUILD_TESTING=OFF",
        "-DCIVETWEB_ENABLE_CXX=ON",
        "-DCIVETWEB_ENABLE_IPV6=ON",
        "-DCIVETWEB_ENABLE_SERVER_EXECUTABLE=OFF",
        "-DCIVETWEB_ENABLE_SSL=OFF",
        "-DCIVETWEB_ENABLE_WEBSOCKETS=OFF",
        "-DCIVETWEB_ENABLE_ASAN=OFF",
    ])
    config = os.path.join(SYSROOT, "share", "civetweb", "civetwebConfig.cmake")
    upstream = os.path.join(SYSROOT, "lib", "cmake", "civetweb", "civetweb-config.cmake")
    if DRY_RUN or (not os.path.exists(config) and not os.path.exists(upstream)):
        write(config, CIVETWEB_CONFIG)


def build_roaring():
    src = extract("roaring")
    cmake_build("roaring", src, [
        "-DROARING_BUILD_STATIC=ON",
        "-DENABLE_ROARING_TESTS=OFF",
        "-DROARING_DISABLE_AVX=ON",
        "-DROARING_DISABLE_NEON=ON",
        "-DROARING_DISABLE_NATIVE=ON",
    ])
    config = os.path.join(SYSROOT, "share", "roaring", "roaringConfig.cmake")
    upstream = os.path.join(SYSROOT, "lib", "cmake", "roaring", "roaring-config.cmake")
    if DRY_RUN or (not os.path.exists(config) and not os.path.exists(upstream)):
        write(config, ROARING_CONFIG)


STEPS = {
    "fmt": install_fmt,
    "asio": install_asio,
    "mio": install_mio,
    "catch2": install_catch2,
    "zlib": build_zlib,
    "minizip": build_minizip,
    "libsodium": build_libsodium,
    "secp256k1-zkp": build_secp256k1,
    "rocksdb": build_rocksdb,
    "civetweb": build_civetweb,
    "roaring": build_roaring,
}


# ------------------------------------------------------------------- discovery

def find_repo(start):
    """Walk up from `start` until a CMakeLists.txt with GRINPP_TESTS is found."""
    cur = os.path.abspath(start)
    while True:
        candidate = os.path.join(cur, "CMakeLists.txt")
        if os.path.isfile(candidate):
            try:
                with open(candidate, encoding="utf-8", errors="replace") as f:
                    if "GRINPP_TESTS" in f.read():
                        return cur
            except OSError:
                pass
        parent = os.path.dirname(cur)
        if parent == cur:
            return ""
        cur = parent


def find_vswhere():
    roots = [os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")]
    for root in roots:
        if not root:
            continue
        p = os.path.join(root, "Microsoft Visual Studio", "Installer", "vswhere.exe")
        if os.path.isfile(p):
            return p
    return ""


def vs_install_path():
    vswhere = find_vswhere()
    if not vswhere:
        return ""
    try:
        out = subprocess.check_output(
            [vswhere, "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return ""
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    return lines[0] if lines else ""


def resolve_toolchain(vs_dir, cmake_override, msbuild_override):
    cmake = cmake_override
    msbuild = msbuild_override
    if not vs_dir:
        vs_dir = vs_install_path()
    if vs_dir:
        log(f"[toolchain] Visual Studio: {vs_dir}")
        if not cmake:
            cmake = os.path.join(vs_dir, "Common7", "IDE", "CommonExtensions",
                                 "Microsoft", "CMake", "CMake", "bin", "cmake.exe")
        if not msbuild:
            msbuild = os.path.join(vs_dir, "MSBuild", "Current", "Bin", "MSBuild.exe")
    if not cmake:
        cmake = shutil.which("cmake") or ""
    if not msbuild:
        msbuild = shutil.which("MSBuild.exe") or shutil.which("msbuild") or ""
    return cmake, msbuild


def write_manifest():
    """Write a canonical per-file manifest next to the logs.

    Format: "<sha256>  <posix-relative-path>" lines sorted by path. The hash of
    this file is the integrity anchor for the produced sysroot. (The published
    sysroot's 82FBF0F7... anchor was produced by the release verifier with a
    documented PowerShell one-liner; see reports/drafts/release/BUILD.md.)
    """
    rows = []
    for dirpath, _dirnames, filenames in os.walk(SYSROOT):
        for name in filenames:
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, SYSROOT).replace("\\", "/")
            rows.append((rel, sha256(path)))
    rows.sort()
    out = os.path.join(LOGDIR, "sysroot.sha256")
    with open(out, "w", encoding="ascii", newline="\n") as f:
        for rel, digest in rows:
            f.write(f"{digest}  {rel}\n")
    digest = sha256(out)
    log(f"[manifest] {out}")
    log(f"[manifest] files={len(rows)} sha256={digest}")


def main():
    global REPO, TOOLCHAIN, SYSROOT, SOURCES, BUILDS, LOGDIR, TMPDIR
    global CMAKE, MSBUILD, MINIZIP_PORT_DIR, ROCKSDB_UTF8, DRY_RUN, _log_handle

    parser = argparse.ArgumentParser(
        description="Build the pinned Grin++ dependency sysroot (Windows x64, MSVC /MT).")
    parser.add_argument("--repo", default="",
                        help="Grin++ repository root (default: auto-detect upward from the script, else cwd)")
    parser.add_argument("--toolchain-dir", default="",
                        help="working dir for sources/builds/logs, NOT the Visual Studio "
                             "installation (default: <repo>/../deps-toolchain)")
    parser.add_argument("--sysroot-dir", default="",
                        help="install prefix (default: <repo>/../deps-sysroot)")
    parser.add_argument("--minizip-port-dir", default="",
                        help="dir with the minizip CMakeLists.txt + minizipConfig.cmake.in "
                             "(default: <script>/vcpkg-ports/minizip, then <repo>/vcpkg/ports/minizip)")
    parser.add_argument("--cmake", default="", help="explicit cmake.exe path")
    parser.add_argument("--msbuild", default="", help="explicit MSBuild.exe path")
    parser.add_argument("--only", nargs="*", default=None,
                        help=f"subset of: {' '.join(ORDER)}")
    parser.add_argument("--rocksdb-utf8", choices=["on", "off"], default="on",
                        help="WITH_WINDOWS_UTF8_FILENAMES for RocksDB (default: on, matches published sysroot)")
    parser.add_argument("--dry-run", "--what-if", dest="dry_run", action="store_true",
                        help="print the resolved plan without downloading or building anything")
    args = parser.parse_args()

    DRY_RUN = args.dry_run
    script_dir = os.path.dirname(os.path.abspath(__file__))

    repo = os.path.abspath(args.repo) if args.repo else find_repo(script_dir) or find_repo(os.getcwd())
    if not repo:
        print("ERROR: cannot locate the Grin++ repository (no CMakeLists.txt with GRINPP_TESTS). "
              "Pass --repo <path>.", file=sys.stderr)
        return 2
    if not os.path.isfile(os.path.join(repo, "CMakeLists.txt")):
        print(f"ERROR: {repo} does not look like the Grin++ repository root.", file=sys.stderr)
        return 2
    REPO = repo

    TOOLCHAIN = args.toolchain_dir or os.path.join(os.path.dirname(repo), "deps-toolchain")
    SYSROOT = args.sysroot_dir or os.path.join(os.path.dirname(repo), "deps-sysroot")
    SOURCES = os.path.join(TOOLCHAIN, "sources")
    BUILDS = os.path.join(TOOLCHAIN, "dep-build")
    LOGDIR = os.path.join(TOOLCHAIN, "logs")
    TMPDIR = os.path.join(TOOLCHAIN, "tmp")
    ROCKSDB_UTF8 = args.rocksdb_utf8 == "on"

    MINIZIP_PORT_DIR = args.minizip_port_dir or os.path.join(script_dir, "vcpkg-ports", "minizip")
    if not os.path.isdir(MINIZIP_PORT_DIR):
        MINIZIP_PORT_DIR = os.path.join(REPO, "vcpkg", "ports", "minizip")

    log("=== Grin++ dependency sysroot build ===")
    log(f"repo        : {REPO}")
    log(f"toolchain   : {TOOLCHAIN}")
    log(f"sysroot     : {SYSROOT}")
    log(f"minizip port: {MINIZIP_PORT_DIR}")
    log(f"rocksdb utf8: {'on' if ROCKSDB_UTF8 else 'off'}")
    log(f"dry-run     : {DRY_RUN}")

    for required in (os.path.join(REPO, "vcpkg", "custom_ports", "libsodium", "CMakeLists.txt"),
                     os.path.join(REPO, "vcpkg", "custom_ports", "secp256k1-zkp", "CMakeLists.txt"),
                     os.path.join(MINIZIP_PORT_DIR, "CMakeLists.txt")):
        if not os.path.isfile(required):
            print(f"ERROR: missing helper file: {required}", file=sys.stderr)
            return 2

    # NOTE: --toolchain-dir is the dependency work directory, NOT the Visual Studio
    # installation. Passing it there made resolve_toolchain() look for CMake/MSBuild
    # under <toolchain-dir>/Common7/... and fail with WinError 2.
    cmake, msbuild = resolve_toolchain("", args.cmake, args.msbuild)
    CMAKE, MSBUILD = cmake, msbuild
    log(f"cmake       : {cmake or '(not found)'}")
    log(f"msbuild     : {msbuild or '(not found)'}")
    if not DRY_RUN and (not cmake or not msbuild):
        print("ERROR: cmake/MSBuild not found. Install VS 2022 Build Tools with C++, "
              "or pass --cmake/--msbuild.", file=sys.stderr)
        return 2

    names = args.only or ORDER
    unknown = [n for n in names if n not in STEPS]
    if unknown:
        print(f"ERROR: unknown step(s): {unknown}; known: {ORDER}", file=sys.stderr)
        return 2

    if not DRY_RUN:
        os.makedirs(TMPDIR, exist_ok=True)
        os.environ["TMP"] = TMPDIR
        os.environ["TEMP"] = TMPDIR
        os.makedirs(SYSROOT, exist_ok=True)
        os.makedirs(BUILDS, exist_ok=True)
        os.makedirs(LOGDIR, exist_ok=True)
        _log_handle = open(os.path.join(LOGDIR, "sysroot.log"), "w", encoding="utf-8")
        log(f"=== start {names} ===")

    failed = []
    for name in names:
        log(f"--- {name} ---")
        try:
            STEPS[name]()
        except Exception as exc:  # noqa: BLE001 - keep going, report at the end
            log(f"[FAIL] {name}: {exc}")
            failed.append(name)

    if DRY_RUN:
        log("=== dry-run complete; nothing downloaded or built ===")
        return 0

    log(f"=== done, failed: {failed} ===")
    if not failed:
        write_manifest()
    if _log_handle is not None:
        _log_handle.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
