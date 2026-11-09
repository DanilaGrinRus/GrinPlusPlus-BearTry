// Fuzz target: Segment/SegmentProof VALIDATION against the full-project ASan
// build (hashes through Crypto/Hasher are instrumented here, unlike the
// standalone parse-only `segment` target).
//
// Selector byte chooses the parser/validator:
//   0 SegmentProof::Deserialize + bounded ReconstructRoot
//   1 Segment<BitmapChunk>::Deserialize + bounded Root/Validate
//   2 Segment<RangeProof>::Deserialize + bounded Validate
//   3 Segment<OutputIdentifier>::Deserialize + bounded Validate
//   4 Segment<TransactionKernel>::Deserialize + bounded Validate
//   5 SegmentProof::Deserialize + Validate with fixed peak geometry
//
// Segment geometry is fixed to a small MMR size and attacker identifiers are
// range-checked before any loop, so malformed IDs cannot drive unbounded work.
// Proof/root mismatches raise SegmentException (controlled rejection).

#include "fuzz_target.h"

#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/Segment.h>
#include <PMMR/Models/SegmentProof.h>

#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Crypto/Models/Hash.h>
#include <Crypto/Models/RangeProof.h>

#include <cstdint>
#include <vector>

namespace {

constexpr std::uint64_t kFixedMmrSize = 8192;

template <typename T>
bool BoundedGeometry(const Segment<T>& segment) {
    const SegmentIdentifier& id = segment.GetIdentifier();
    if (id.GetHeight() > 13) { return false; }
    if (id.SegmentUnprunedSize(kFixedMmrSize) == 0) { return false; }
    const std::pair<std::uint64_t, std::uint64_t> range = id.SegmentPosRange(kFixedMmrSize);
    if (range.first > range.second || range.second >= kFixedMmrSize) { return false; }
    return true;
}

template <typename T>
void ValidateBounded(const Segment<T>& segment) {
    if (!BoundedGeometry(segment)) { return; }
    try {
        (void)segment.Root(kFixedMmrSize);
    } catch (const SegmentException&) {
    }
    try {
        segment.Validate(kFixedMmrSize, HASH::ZERO);
    } catch (const SegmentException&) {
    }
}

} // namespace

extern "C" const char* FuzzTargetName() { return "segment_validate"; }

extern "C" const char* FuzzTargetDescription() {
    return "Segment<T>/SegmentProof deserialize + instrumented Root/Validate/ReconstructRoot";
}

extern "C" void FuzzTargetCase(const std::uint8_t* data, std::size_t size) noexcept(false) {
    if (size == 0) { return; }
    const std::uint8_t op = data[0];
    const std::vector<std::uint8_t> body(data + 1, data + size);
    ByteBuffer buffer(body);

    switch (op % 6) {
        case 0: {
            const SegmentProof proof = SegmentProof::Deserialize(buffer);
            (void)proof.ReconstructRoot(kFixedMmrSize, 0, kFixedMmrSize - 1, HASH::ZERO, kFixedMmrSize);
            break;
        }
        case 1: {
            ValidateBounded(Segment<BitmapChunk>::Deserialize(buffer));
            break;
        }
        case 2: {
            ValidateBounded(Segment<RangeProof>::Deserialize(buffer));
            break;
        }
        case 3: {
            ValidateBounded(Segment<OutputIdentifier>::Deserialize(buffer));
            break;
        }
        case 4: {
            ValidateBounded(Segment<TransactionKernel>::Deserialize(buffer));
            break;
        }
        default: {
            const SegmentProof proof = SegmentProof::Deserialize(buffer);
            (void)proof.ReconstructRoot(kFixedMmrSize, 0, kFixedMmrSize / 2, HASH::ZERO, kFixedMmrSize / 2 + 1);
            break;
        }
    }
}
