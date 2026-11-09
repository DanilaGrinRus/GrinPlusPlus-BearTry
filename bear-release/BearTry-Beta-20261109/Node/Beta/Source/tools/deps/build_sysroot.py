#!/usr/bin/env python3
"""Build a local dependency sysroot for Grin++ (Windows x64, MSVC /MT).

Why this exists: the pinned vcpkg (2020.02) predates VS2022 and its downloads
cannot work inside the current sandbox (Schannel is unavailable, WinHTTP fails
with 12185). Python/OpenSSL can download, so this script mirrors the pinned
vcpkg/overlay port recipes: exact refs, static CRT, CMake install into one
prefix, plus the handful of package config files the Grin++ CMakeLists expects.

Usage: python tools/deps/build_sysroot.py [--only NAME ...]
Logs:  C:/DHW/Gplus/.toolchain/logs/sysroot.log
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

TOOLCHAIN = r"C:\DHW\Gplus\.toolchain"
SOURCES = os.path.join(TOOLCHAIN, "sources")
SYSROOT = os.path.join(TOOLCHAIN, "sysroot")
BUILDS = os.path.join(TOOLCHAIN, "dep-build")
LOGDIR = os.path.join(TOOLCHAIN, "logs")
LOG = os.path.join(LOGDIR, "sysroot.log")
REPO = r"C:\DHW\Gplus\src\grinplusplus"
TMP = r"C:\DHW\.tmp\deps"

CMAKE = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
MSBUILD = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
GENERATOR = "Visual Studio 17 2022"
COMMON_DEFS = [
    "-G", GENERATOR, "-A", "x64",
    # Several pinned dependencies declare cmake_minimum_required < 3.15, so the
    # CMP0091 policy leaves CMAKE_MSVC_RUNTIME_LIBRARY unused and CMake injects
    # /MD through the default flags. Grin++ links a static CRT, therefore the
    # per-config flags are overridden explicitly as well.
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DCMAKE_C_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG",
    "-DCMAKE_CXX_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG",
    "-DCMAKE_C_FLAGS_DEBUG=/MTd /Zi /Ob0 /Od /RTC1",
    "-DCMAKE_CXX_FLAGS_DEBUG=/MTd /Zi /Ob0 /Od /RTC1",
    f"-DCMAKE_INSTALL_PREFIX={SYSROOT}",
    f"-DCMAKE_PREFIX_PATH={SYSROOT}",
]

# name -> (url, archive-root-fragment)
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


def log(msg):
    line = str(msg)
    print(line, flush=True)
    os.makedirs(LOGDIR, exist_ok=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def run(cmd):
    log("+ " + " ".join(cmd))
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
        log(f"[get] {name} <- {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "grinpp-sysroot/1.0"})
        with urllib.request.urlopen(req, timeout=180) as resp, open(path + ".part", "wb") as out:
            shutil.copyfileobj(resp, out)
        os.replace(path + ".part", path)
    digest = sha256(path)
    log(f"[src] {name} sha256={digest} size={os.path.getsize(path)}")
    with open(os.path.join(SOURCES, "SOURCES.sha256"), "a", encoding="ascii") as manifest:
        manifest.write(f"{digest}  {name}{ext}\n")
    return path


def extract(name, dest_name=None):
    url, rootfrag = DEPS[name]
    path = fetch(name)
    dest = os.path.join(SOURCES, "extracted", dest_name or name)
    os.makedirs(dest, exist_ok=True)
    target = os.path.join(dest, rootfrag)
    if os.path.isdir(target):
        return target
    log(f"[extract] {name}")
    with tarfile.open(path, "r:gz") as tar:
        tar.extractall(dest)
    if not os.path.isdir(target):
        dirs = [d for d in os.listdir(dest) if os.path.isdir(os.path.join(dest, d))]
        raise RuntimeError(f"unexpected archive layout for {name}: {dirs[:5]}")
    return target


def cmake_build(name, source, extra_defs, build_subdir=None, targets=("install",)):
    build = os.path.join(BUILDS, build_subdir or name)
    cache = os.path.join(build, "CMakeCache.txt")
    if os.path.exists(cache):
        with open(cache, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if f"CMAKE_HOME_DIRECTORY:INTERNAL={source}" not in text:
            log(f"[reset] {build} (cached for another source tree)")
            shutil.rmtree(build, ignore_errors=True)
    os.makedirs(build, exist_ok=True)
    run([CMAKE, "-S", source, "-B", build] + COMMON_DEFS + list(extra_defs))
    # Build the install target through MSBuild directly: `cmake --build` does not
    # reliably forward the file-tracker switch here, and inside the sandbox the
    # tracker fails with TRK0002 (build-host limitation, not a project change).
    msb_log = os.path.join(LOGDIR, f"msbuild-{name}.log")
    # /nr:false + MSBUILDDISABLENODEREUSE: orphaned MSBuild worker nodes left by an
    # interrupted build make later invocations fail silently (rc=1, no diagnostics).
    # No /m: parallel MSBuild uses named pipes for its worker nodes and those are
    # blocked in the current sandbox ("programs cannot open named pipes"), which
    # makes every parallel invocation fail before it starts compiling.
    os.environ["MSBUILDDISABLENODEREUSE"] = "1"
    run([MSBUILD, os.path.join(build, "install.vcxproj"),
         "/p:Configuration=Release", "/p:Platform=x64",
         "/p:TrackFileAccess=false", "/p:UseSharedCompilation=false",
         "/nr:false", "/v:m", "/nologo",
         f"/flp:LogFile={msb_log};Verbosity=normal"])
    return build


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    log(f"[write] {path}")


def relative_prefix_from(config_dir):
    return config_dir


# ---------------------------------------------------------------- header-only

def install_fmt():
    src = extract("fmt")
    dst = os.path.join(SYSROOT, "include")
    os.makedirs(dst, exist_ok=True)
    shutil.copytree(os.path.join(src, "include", "fmt"), os.path.join(dst, "fmt"), dirs_exist_ok=True)
    write(os.path.join(SYSROOT, "share", "fmt", "fmt-config.cmake"), """
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
""")


def install_asio():
    src = extract("asio")
    # Grin++ includes <asio.hpp> without linking an asio target, so a compile
    # definition alone is not enough. The vcpkg port rewrites the standalone
    # check in config.hpp; do the same here (ASIO never uses Boost in this tree).
    config_hpp = os.path.join(src, "asio", "include", "asio", "detail", "config.hpp")
    with open(config_hpp, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if "defined(ASIO_STANDALONE)" in text:
        text = text.replace("defined(ASIO_STANDALONE)",
                            "!defined(GRINPP_ASIO_WITH_BOOST)", 1)
        with open(config_hpp, "w", encoding="utf-8") as f:
            f.write(text)
        log("[patch] asio: standalone mode default")
    # Some translation units define a POSIX-ish macro indirectly; on a Windows
    # target there is no <unistd.h>, so keep the include strictly non-Windows.
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
    # vcpkg port rewrites ASIO_STANDALONE to be always on; an interface compile
    # definition achieves the same without patching third-party sources.
    write(os.path.join(SYSROOT, "share", "asio", "asio-config.cmake"), """
get_filename_component(_asio_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if (NOT TARGET asio::asio)
  add_library(asio::asio INTERFACE IMPORTED)
  set_target_properties(asio::asio PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_asio_prefix}/include"
    INTERFACE_COMPILE_DEFINITIONS "ASIO_STANDALONE=1;ASIO_NO_DEPRECATED=1")
endif()
set(ASIO_INCLUDE_DIR "${_asio_prefix}/include")
set(asio_FOUND TRUE)
""")


def install_mio():
    src = extract("mio")
    dst = os.path.join(SYSROOT, "include", "mio")
    # Grin++ includes <mio/mmap.hpp>; the multi-header layout is the one needed.
    multi = os.path.join(src, "include", "mio")
    if os.path.isdir(multi):
        shutil.copytree(multi, dst, dirs_exist_ok=True)
    else:
        single = os.path.join(src, "single_include", "mio")
        shutil.copytree(single, dst, dirs_exist_ok=True)
    write(os.path.join(SYSROOT, "share", "mio", "mioConfig.cmake"), """
get_filename_component(_mio_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if (NOT TARGET mio::mio)
  add_library(mio::mio INTERFACE IMPORTED)
  set_target_properties(mio::mio PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_mio_prefix}/include")
endif()
set(mio_FOUND TRUE)
""")


def install_catch2():
    src = extract("catch2")
    dst = os.path.join(SYSROOT, "include")
    shutil.copytree(os.path.join(src, "single_include", "catch2"), os.path.join(dst, "catch2"), dirs_exist_ok=True)
    modules = os.path.join(SYSROOT, "share", "Catch2")
    os.makedirs(modules, exist_ok=True)
    cmake_dir = os.path.join(src, "extras")
    if not os.path.isdir(cmake_dir):
        cmake_dir = os.path.join(src, "contrib")
    for f in os.listdir(cmake_dir):
        if f.endswith(".cmake"):
            shutil.copy2(os.path.join(cmake_dir, f), os.path.join(modules, f))
    write(os.path.join(modules, "Catch2Config.cmake"), """
get_filename_component(_catch_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if (NOT TARGET Catch2::Catch2)
  add_library(Catch2::Catch2 INTERFACE IMPORTED)
  set_target_properties(Catch2::Catch2 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_catch_prefix}/include/catch2")
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
set(Catch2_FOUND TRUE)
set(CATCH_INCLUDE_DIR "${_catch_prefix}/include/catch2")
""")


# ------------------------------------------------------------------ built deps

def build_zlib():
    src = extract("zlib")
    cmake_build("zlib", src, ["-DBUILD_SHARED_LIBS=OFF"])
    # zlib 1.2.11 always builds a shared flavour alongside the static one.
    # Grin++ links a static-CRT binary, so remove the DLL import library to keep
    # FindZLIB on zlibstatic.lib.
    for stale in (os.path.join(SYSROOT, "lib", "zlib.lib"),
                  os.path.join(SYSROOT, "bin", "zlib.dll"),
                  os.path.join(SYSROOT, "lib", "zlib.dll")):
        if os.path.exists(stale):
            os.remove(stale)
            log(f"[clean] {stale}")


def build_minizip():
    src = extract("minizip_src", dest_name="minizip")
    helper = os.path.join(TOOLCHAIN, "vcpkg", "ports", "minizip")
    shutil.copy2(os.path.join(helper, "CMakeLists.txt"), os.path.join(src, "CMakeLists.txt"))
    os.makedirs(os.path.join(src, "cmake"), exist_ok=True)
    shutil.copy2(os.path.join(helper, "minizipConfig.cmake.in"), os.path.join(src, "cmake", "minizipConfig.cmake.in"))
    cmake_build("minizip", src, ["-DDISABLE_INSTALL_TOOLS=ON"], build_subdir="minizip")


def build_libsodium():
    src = extract("libsodium")
    port = os.path.join(REPO, "vcpkg", "custom_ports", "libsodium")
    shutil.copy2(os.path.join(port, "CMakeLists.txt"), os.path.join(src, "CMakeLists.txt"))
    shutil.copy2(os.path.join(port, "sodiumConfig.cmake.in"), os.path.join(src, "sodiumConfig.cmake.in"))
    cmake_build("libsodium", src, ["-DBUILD_SHARED_LIBS=OFF"])


def build_secp256k1():
    src = extract("secp256k1-zkp")
    port = os.path.join(REPO, "vcpkg", "custom_ports", "secp256k1-zkp")
    shutil.copy2(os.path.join(port, "windows", "libsecp256k1-config.h"), os.path.join(src, "libsecp256k1-config.h"))
    shutil.copy2(os.path.join(port, "CMakeLists.txt"), os.path.join(src, "CMakeLists.txt"))
    cmake_build("secp256k1-zkp", src, ["-DINSTALL_HEADERS=ON"])


def build_rocksdb():
    src = extract("rocksdb")
    # rocksdb ships a Findzlib.cmake that resolves zlib from NuGet-style paths;
    # the vcpkg port removes it for exactly this reason (see portfile.cmake).
    stale_zlib_module = os.path.join(src, "cmake", "modules", "Findzlib.cmake")
    if os.path.exists(stale_zlib_module):
        os.remove(stale_zlib_module)
        log(f"[clean] {stale_zlib_module}")

    # thirdparty.inc hardcodes NuGet-style paths (\ZLIB.Library\...). The vcpkg
    # port disables it via VCPKG_TOOLCHAIN; here it is replaced directly so that
    # find_package(ZLIB) resolves against the sysroot.
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
        "-DWITH_WINDOWS_UTF8_FILENAMES=OFF",
        "-DROCKSDB_INSTALL_ON_WINDOWS=ON",
        # rocksdb defaults to /MD; the node links a static CRT (/MT).
        "-DWITH_MD_LIBRARY=OFF",
        "-DFAIL_ON_WARNINGS=OFF",
        "-DUSE_RTTI=ON",
    ])


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
    if not os.path.exists(config) and not os.path.exists(upstream):
        write(config, """
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
""")


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
    if not os.path.exists(config) and not os.path.exists(upstream):
        write(config, """
get_filename_component(_roar_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_library(_roar_lib NAMES roaring HINTS "${_roar_prefix}/lib" NO_DEFAULT_PATH)
if (NOT TARGET roaring::roaring)
  add_library(roaring::roaring UNKNOWN IMPORTED)
  set_target_properties(roaring::roaring PROPERTIES
    IMPORTED_LOCATION "${_roar_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_roar_prefix}/include")
endif()
set(roaring_FOUND TRUE)
""")


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

ORDER = ["fmt", "asio", "mio", "catch2", "zlib", "minizip", "libsodium",
         "secp256k1-zkp", "rocksdb", "civetweb", "roaring"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", nargs="*", default=None)
    args = parser.parse_args()

    os.makedirs(TMP, exist_ok=True)
    os.environ["TMP"] = TMP
    os.environ["TEMP"] = TMP
    os.makedirs(SYSROOT, exist_ok=True)
    os.makedirs(BUILDS, exist_ok=True)
    os.makedirs(LOGDIR, exist_ok=True)
    log(f"=== sysroot build start ===")

    names = args.only or ORDER
    failed = []
    for name in names:
        log(f"--- {name} ---")
        try:
            STEPS[name]()
        except Exception as exc:  # noqa: BLE001 - keep going, report at the end
            log(f"[FAIL] {name}: {exc}")
            failed.append(name)

    log(f"=== done, failed: {failed} ===")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

