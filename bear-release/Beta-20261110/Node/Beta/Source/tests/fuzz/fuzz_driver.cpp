// Deterministic, bounded mutation driver for the Grin++ ASan fuzz harness.
//
// This driver is intentionally standalone: it is not part of the normal CMake
// build and must be compiled with an AddressSanitizer profile (see
// tools/fuzz/build_asan.ps1). It does not require libFuzzer: given a corpus
// directory, a fixed seed and an iteration budget it performs reproducible
// byte-level mutations and calls one target per input.
//
// Crash preservation: the current input is flushed to <out>/current_case.bin
// before each call, so an ASan abort leaves the exact crashing input on disk
// together with <out>/last_case.txt (iteration + seed).
//
// Hang handling: a single call cannot be interrupted in-process. Bounded input
// size plus per-target limits are the first line of defence; the regression
// runner (tools/fuzz/run_fuzz_regression.ps1) additionally executes one input
// per child process with a wall-clock timeout.

#include "fuzz_target.h"

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Exceptions/FileException.h>
#include <PMMR/Models/SegmentException.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    std::uint64_t Next() {
        std::uint64_t x = state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state = x;
        return x * 0x2545F4914F6CDD1DULL;
    }
    std::size_t Range(std::size_t n) { return n == 0 ? 0 : (std::size_t)(Next() % n); }
};

std::uint64_t Fnv1a(const std::uint8_t* data, std::size_t size) {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& p, std::size_t cap) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { throw std::runtime_error("cannot read corpus file: " + p.string()); }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > cap) { bytes.resize(cap); }
    return bytes;
}

// Boundary values that exercise length/count fields in the parsers.
const std::uint64_t kBoundaries[] = {
    0ULL, 1ULL, 2ULL, 3ULL, 7ULL, 8ULL, 31ULL, 32ULL, 0xFFULL, 0x100ULL,
    0x10000ULL, 0xFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL,
};

std::vector<std::uint8_t> Mutate(const std::vector<std::uint8_t>& base, Rng& rng, std::size_t cap) {
    std::vector<std::uint8_t> out = base;
    const std::size_t ops = 1 + rng.Range(4);
    for (std::size_t i = 0; i < ops; ++i) {
        const std::size_t op = rng.Range(9);
        if (out.empty() && op != 2 && op != 7) { out.push_back((std::uint8_t)rng.Next()); continue; }
        switch (op) {
            case 0: { // flip one bit
                const std::size_t p = rng.Range(out.size());
                out[p] ^= (std::uint8_t)(1u << rng.Range(8));
                break;
            }
            case 1: // set a random byte
                out[rng.Range(out.size())] = (std::uint8_t)rng.Next();
                break;
            case 2: // insert a byte
                if (out.size() < cap) { out.insert(out.begin() + (std::ptrdiff_t)rng.Range(out.size() + 1), (std::uint8_t)rng.Next()); }
                break;
            case 3: { // erase a range
                const std::size_t p = rng.Range(out.size());
                const std::size_t n = 1 + rng.Range(std::min<std::size_t>(16, out.size() - p));
                out.erase(out.begin() + (std::ptrdiff_t)p, out.begin() + (std::ptrdiff_t)(p + n));
                break;
            }
            case 4: { // write a boundary value big-endian at an aligned-ish offset
                const std::uint64_t v = kBoundaries[rng.Range(sizeof(kBoundaries) / sizeof(kBoundaries[0]))];
                const std::size_t p = rng.Range(out.size());
                for (std::size_t b = 0; b < 8 && p + b < out.size(); ++b) {
                    out[p + b] = (std::uint8_t)(v >> (56 - 8 * b));
                }
                break;
            }
            case 5: { // duplicate a range
                const std::size_t p = rng.Range(out.size());
                const std::size_t n = 1 + rng.Range(std::min<std::size_t>(32, out.size() - p));
                if (out.size() + n <= cap) { out.insert(out.end(), out.begin() + (std::ptrdiff_t)p, out.begin() + (std::ptrdiff_t)(p + n)); }
                break;
            }
            case 6: // truncate
                if (out.size() > 1) { out.resize(1 + rng.Range(out.size() - 1)); }
                break;
            case 7: // append zeros / 0xff block
                if (out.size() < cap) {
                    const std::size_t n = std::min<std::size_t>(cap - out.size(), 1 + rng.Range(32));
                    out.insert(out.end(), n, (rng.Next() & 1) ? 0x00 : 0xFF);
                }
                break;
            default: // replace whole content with a short synthetic buffer
                out.clear();
                out.push_back((std::uint8_t)rng.Next());
                break;
        }
        if (out.size() > cap) { out.resize(cap); }
    }
    return out;
}

struct OutcomeCounts {
    std::uint64_t ok = 0;
    std::uint64_t rejected_deserialization = 0;
    std::uint64_t rejected_file = 0;
    std::uint64_t rejected_segment = 0;
    std::uint64_t oom = 0;
    std::uint64_t exception = 0;
    std::uint64_t unknown = 0;
    std::uint64_t total() const {
        return ok + rejected_deserialization + rejected_file + rejected_segment + oom + exception + unknown;
    }
};

std::string ExecuteOne(const std::uint8_t* data, std::size_t size, OutcomeCounts& counts) {
    try {
        FuzzTargetCase(data, size);
        ++counts.ok;
        return "ok";
    } catch (const DeserializationException&) {
        ++counts.rejected_deserialization;
        return "rejected_deserialization";
    } catch (const FileException&) {
        ++counts.rejected_file;
        return "rejected_file";
    } catch (const SegmentException&) {
        ++counts.rejected_segment;
        return "rejected_segment";
    } catch (const std::bad_alloc&) {
        ++counts.oom;
        return "oom";
    } catch (const std::exception&) {
        ++counts.exception;
        return "exception";
    } catch (...) {
        ++counts.unknown;
        return "unknown_exception";
    }
}

void Usage() {
    std::fprintf(stderr,
        "usage: %s --corpus DIR --out DIR [--iterations N] [--seed S]\n"
        "          [--max-input-size B] [--max-seconds T] [--slow-ms T] [--quiet]\n"
        "       %s --one-shot FILE\n",
        FuzzTargetName(), FuzzTargetName());
}

} // namespace

int main(int argc, char** argv) {
    std::string corpusDir;
    std::string outDir;
    std::string oneShot;
    std::uint64_t iterations = 1000;
    std::uint64_t seed = 1;
    std::size_t maxInputSize = 64 * 1024;
    double maxSeconds = 60.0;
    long slowMs = 500;
    bool quiet = false;
    bool asanSelfTest = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { throw std::runtime_error(std::string("missing value for ") + name); }
            return argv[++i];
        };
        if (a == "--corpus") { corpusDir = next("--corpus"); }
        else if (a == "--out") { outDir = next("--out"); }
        else if (a == "--iterations") { iterations = std::strtoull(next("--iterations").c_str(), nullptr, 10); }
        else if (a == "--seed") { seed = std::strtoull(next("--seed").c_str(), nullptr, 10); }
        else if (a == "--max-input-size") { maxInputSize = (std::size_t)std::strtoull(next("--max-input-size").c_str(), nullptr, 10); }
        else if (a == "--max-seconds") { maxSeconds = std::strtod(next("--max-seconds").c_str(), nullptr); }
        else if (a == "--slow-ms") { slowMs = std::strtol(next("--slow-ms").c_str(), nullptr, 10); }
        else if (a == "--one-shot") { oneShot = next("--one-shot"); }
        else if (a == "--asan-selftest") { asanSelfTest = true; }
        else if (a == "--quiet") { quiet = true; }
        else { Usage(); return 2; }
    }

    try {
        std::printf("target=%s\n", FuzzTargetName());
        std::printf("description=%s\n", FuzzTargetDescription());

        if (asanSelfTest) {
            // Instrumentation self-test: a deliberate one-byte heap overflow in
            // this (instrumented) translation unit must be reported by ASan.
            volatile std::uint8_t* p = new std::uint8_t[8];
            p[8] = 0xAB;
            const std::uint8_t sink = p[8];
            (void)sink;
            delete[] p;
            std::printf("self_test=not_detected\n");
            return 0;
        }

        if (!oneShot.empty()) {
            const std::vector<std::uint8_t> bytes = ReadFile(oneShot, maxInputSize);
            OutcomeCounts counts;
            const std::string outcome = ExecuteOne(bytes.data(), bytes.size(), counts);
            std::printf("one_shot=%s outcome=%s input_bytes=%zu input_fnv1a=%016llx\n",
                oneShot.c_str(), outcome.c_str(), bytes.size(),
                (unsigned long long)Fnv1a(bytes.data(), bytes.size()));
            return 0;
        }

        if (corpusDir.empty() || outDir.empty()) { Usage(); return 2; }
        std::filesystem::create_directories(outDir);
        std::filesystem::create_directories(std::filesystem::path(outDir) / "rejected");

        std::vector<std::filesystem::path> corpusFiles;
        for (const auto& e : std::filesystem::directory_iterator(corpusDir)) {
            if (e.is_regular_file()) { corpusFiles.push_back(e.path()); }
        }
        std::sort(corpusFiles.begin(), corpusFiles.end());
        if (corpusFiles.empty()) { throw std::runtime_error("corpus directory is empty: " + corpusDir); }

        std::vector<std::vector<std::uint8_t>> corpus;
        std::uint64_t corpusFnv = 1469598103934665603ULL;
        for (const auto& p : corpusFiles) {
            corpus.push_back(ReadFile(p, maxInputSize));
            const std::string name = p.filename().string();
            for (char c : name) { corpusFnv = (corpusFnv ^ (std::uint8_t)c) * 1099511628211ULL; }
            corpusFnv = (corpusFnv ^ (std::uint8_t)0x7C) * 1099511628211ULL;
            const std::uint64_t f = Fnv1a(corpus.back().data(), corpus.back().size());
            corpusFnv ^= f;
            corpusFnv *= 1099511628211ULL;
        }

        const std::string currentPath = (std::filesystem::path(outDir) / "current_case.bin").string();
        const std::string lastPath = (std::filesystem::path(outDir) / "last_case.txt").string();

        Rng rng(seed);
        OutcomeCounts counts;
        std::uint64_t executed = 0;
        std::uint64_t slow = 0;
        double slowestMs = 0.0;
        std::uint64_t rejectedSaved = 0;
        const auto start = std::chrono::steady_clock::now();

        for (std::uint64_t it = 0; it < iterations; ++it) {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= maxSeconds) { break; }
            const std::size_t base = rng.Range(corpus.size());
            std::vector<std::uint8_t> input = Mutate(corpus[base], rng, maxInputSize);

            {   // preserve the exact input before it can crash the process
                std::ofstream f(currentPath, std::ios::binary | std::ios::trunc);
                f.write((const char*)input.data(), (std::streamsize)input.size());
            }
            {
                std::ofstream f(lastPath, std::ios::trunc);
                f << "iteration=" << it << " seed=" << seed << " base=" << corpusFiles[base].filename().string()
                  << " bytes=" << input.size() << "\n";
            }

            const auto caseStart = std::chrono::steady_clock::now();
            const std::string outcome = ExecuteOne(input.data(), input.size(), counts);
            const double caseMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - caseStart).count();
            slowestMs = std::max(slowestMs, caseMs);
            if (caseMs > (double)slowMs) { ++slow; }
            ++executed;

            if (outcome != "ok" && rejectedSaved < 20) {
                char name[64];
                std::snprintf(name, sizeof(name), "rejected/%06llu_%s.bin", (unsigned long long)it, outcome.c_str());
                std::ofstream f(std::filesystem::path(outDir) / name, std::ios::binary | std::ios::trunc);
                f.write((const char*)input.data(), (std::streamsize)input.size());
                ++rejectedSaved;
            }
            if (!quiet && (it % 100 == 0)) {
                std::printf("iter=%llu bytes=%zu outcome=%s elapsed=%.1fs\n",
                    (unsigned long long)it, input.size(), outcome.c_str(), elapsed);
                std::fflush(stdout);
            }
        }

        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("SUMMARY target=%s seed=%llu executed=%llu ok=%llu rejected_deserialization=%llu "
                    "rejected_file=%llu rejected_segment=%llu oom=%llu exception=%llu unknown=%llu "
                    "slow_cases=%llu slowest_ms=%.3f wall_sec=%.3f corpus_files=%zu corpus_fnv1a=%016llx\n",
            FuzzTargetName(), (unsigned long long)seed, (unsigned long long)executed,
            (unsigned long long)counts.ok, (unsigned long long)counts.rejected_deserialization,
            (unsigned long long)counts.rejected_file, (unsigned long long)counts.rejected_segment,
            (unsigned long long)counts.oom, (unsigned long long)counts.exception,
            (unsigned long long)counts.unknown, (unsigned long long)slow, slowestMs, wall,
            corpus.size(), (unsigned long long)corpusFnv);

        std::ofstream summary(std::filesystem::path(outDir) / "summary.txt", std::ios::trunc);
        summary << "target=" << FuzzTargetName() << "\n"
                << "seed=" << seed << "\n"
                << "executed=" << executed << "\n"
                << "ok=" << counts.ok << "\n"
                << "rejected_deserialization=" << counts.rejected_deserialization << "\n"
                << "rejected_file=" << counts.rejected_file << "\n"
                << "rejected_segment=" << counts.rejected_segment << "\n"
                << "oom=" << counts.oom << "\n"
                << "exception=" << counts.exception << "\n"
                << "unknown_exception=" << counts.unknown << "\n"
                << "slow_cases=" << slow << "\n"
                << "slowest_ms=" << slowestMs << "\n"
                << "wall_sec=" << wall << "\n"
                << "corpus_files=" << corpus.size() << "\n"
                << "corpus_fnv1a=" << std::hex << corpusFnv << std::dec << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "driver error: %s\n", e.what());
        return 1;
    }
}
