// Fuzz target: ZIP/snapshot parser (src/PMMR/Zip/ZipFile.cpp + minizip).
//
// Writes the input to a one-shot file inside GRINPP_FUZZ_STAGING, opens it with
// ZipFile::Load, lists entries and extracts the first entry into the staging
// directory. Malformed archives must fail with FileException (controlled
// rejection) - CRC mismatches, truncated central directories, duplicate names,
// absurd declared sizes and path-traversal entry names are all in scope.
//
// Extraction destination is always chosen by this harness (never derived from
// the entry name), so a traversal-named entry cannot escape the staging dir; the
// traversal-named corpus entry is kept to prove the parser does not itself write
// outside that destination.
//
// Instrumentation note: ZipFile.cpp is compiled with ASan by the harness build;
// minizip/zlib come from the pinned sysroot and are NOT instrumented.

#include "fuzz_target.h"

#include <PMMR/Zip/ZipFile.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path StagingDir() {
    const char* env = std::getenv("GRINPP_FUZZ_STAGING");
    if (env == nullptr || *env == '\0') {
        throw std::runtime_error("GRINPP_FUZZ_STAGING is not set; refusing to write outside a disposable staging dir");
    }
    std::filesystem::path dir(env);
    std::filesystem::create_directories(dir);
    return dir;
}

std::uint64_t NextCaseId() {
    static std::uint64_t counter = 0;
    return ++counter;
}

} // namespace

extern "C" const char* FuzzTargetName() { return "zip"; }

extern "C" const char* FuzzTargetDescription() {
    return "ZipFile::Load/ListFiles/ExtractFile over attacker archives (minizip uninstrumented)";
}

extern "C" void FuzzTargetCase(const std::uint8_t* data, std::size_t size) noexcept(false) {
    const std::filesystem::path staging = StagingDir();
    const std::uint64_t id = NextCaseId();
    const std::filesystem::path archive = staging / ("case_" + std::to_string(id) + ".zip");
    const std::filesystem::path destination = staging / ("out_" + std::to_string(id) + ".bin");

    {
        std::ofstream out(archive, std::ios::binary | std::ios::trunc);
        out.write((const char*)data, (std::streamsize)size);
        if (!out) { throw std::runtime_error("failed to write fuzz archive into staging"); }
    }

    try {
        const std::shared_ptr<ZipFile> zip = ZipFile::Load(archive);
        const std::vector<std::string> files = zip->ListFiles();
        if (!files.empty()) {
            zip->ExtractFile(files.front(), destination);
        }
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(archive, ec);
        std::filesystem::remove(destination, ec);
        throw;
    }

    std::error_code ec;
    std::filesystem::remove(archive, ec);
    std::filesystem::remove(destination, ec);
}
