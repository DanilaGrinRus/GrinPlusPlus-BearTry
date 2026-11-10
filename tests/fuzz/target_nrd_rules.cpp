// Fuzz target: production NRD rule functions (include/PMMR/NrdRules.h).
//
// This is the pure, header-only rule set used by the kernel index / block
// validation: IsRelativeHeightSatisfied() and RecordBlockKernel(). It has no
// dependency on the ordinary (non-ASan) project static libraries, so it can be
// instrumented directly by this standalone harness.
//
// Input layout (big-endian), see tools/fuzz/gen_nrd_seeds.ps1 which derives
// seeds from the line-2 differential cases (tools/audit/nrd/cases/nrd_cases.json):
//   [0]        selector
//   [1..8]     previousHeight
//   [9..16]    currentHeight
//   [17..24]   relativeHeight
//   [25..32]   cutoffHeight
//   [33..]     excess bytes, consumed as 33-byte commitments
//
// Selector:
//   0 - one rule evaluation with the four raw heights
//   1 - duplicate-excess detection over the excess chunks
//   2 - boundary sweep: relative_height 0/1/2/WEEK_HEIGHT/WEEK_HEIGHT+1/0xFFFF/
//       UINT64_MAX and currentHeight below/at/above previousHeight
//   3 - combined: sweep + duplicate detection
//
// Categories like "accepted"/"invalid_nrd_relative_height" are covered by the
// ordinary differential tests; this target only asserts absence of OOB/UB/hang
// in the rule functions for attacker-shaped values.

#include "fuzz_target.h"

#include <PMMR/NrdRules.h>

#include <cstdint>
#include <map>
#include <vector>

namespace {

std::uint64_t ReadU64(const std::uint8_t* data, std::size_t size, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8);
        if (offset + i < size) { value |= data[offset + i]; }
    }
    return value;
}

// Mirrors Grin's WEEK_HEIGHT = 2 weeks of one-minute blocks.
constexpr std::uint64_t kWeekHeight = 2 * 7 * 24 * 60;

const std::uint64_t kRelativeHeights[] = {
    0, 1, 2, kWeekHeight - 1, kWeekHeight, kWeekHeight + 1, 0xFFFF, 0xFFFFFFFFFFFFFFFFULL,
};

void Sweep(const std::uint8_t* data, std::size_t size) {
    const std::uint64_t previous = ReadU64(data, size, 1);
    const std::uint64_t current = ReadU64(data, size, 9);
    const std::uint64_t cutoff = ReadU64(data, size, 25);

    // current relative to previous: below, equal, above by rel-1/rel
    const std::uint64_t currents[] = {
        previous == 0 ? 0 : previous - 1,
        previous,
        previous + 1,
        previous + kWeekHeight,
        current,
    };

    for (const std::uint64_t rel : kRelativeHeights) {
        for (const std::uint64_t cur : currents) {
            for (const std::uint64_t prev : {previous, previous == 0 ? 0 : previous - 1, cutoff}) {
                (void)NrdRules::IsRelativeHeightSatisfied(prev, cur, rel, cutoff);
            }
        }
    }
}

void DuplicateDetection(const std::uint8_t* data, std::size_t size) {
    std::map<Commitment, std::uint64_t> blockKernels;
    const std::size_t chunks = (size > 33) ? (size - 33) / 33 : 0;
    const std::size_t limit = chunks > 64 ? 64 : chunks;
    for (std::size_t i = 0; i < limit; ++i) {
        std::vector<std::uint8_t> raw(data + 33 + i * 33, data + 33 + (i + 1) * 33);
        const Commitment excess(std::move(raw));
        (void)NrdRules::RecordBlockKernel(blockKernels, excess, i);
    }
    // Second pass: every existing excess must now be reported as duplicate.
    for (const auto& entry : blockKernels) {
        (void)NrdRules::RecordBlockKernel(blockKernels, entry.first, entry.second + 1);
    }
}

} // namespace

extern "C" const char* FuzzTargetName() { return "nrd_rules"; }

extern "C" const char* FuzzTargetDescription() {
    return "NrdRules::IsRelativeHeightSatisfied / RecordBlockKernel boundary and duplicate-excess fuzz";
}

extern "C" void FuzzTargetCase(const std::uint8_t* data, std::size_t size) noexcept(false) {
    if (size == 0) { return; }
    switch (data[0] % 4) {
        case 0: {
            (void)NrdRules::IsRelativeHeightSatisfied(
                ReadU64(data, size, 1), ReadU64(data, size, 9),
                ReadU64(data, size, 17), ReadU64(data, size, 25));
            break;
        }
        case 1: {
            DuplicateDetection(data, size);
            break;
        }
        case 2: {
            Sweep(data, size);
            break;
        }
        default: {
            Sweep(data, size);
            DuplicateDetection(data, size);
            break;
        }
    }
}
