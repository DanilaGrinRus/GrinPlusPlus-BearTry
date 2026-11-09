// Fuzz target: full SegmentResponseMessage bodies for PIBD types 22/24/26/28,
// linked against the FULL-project ASan build (so OutputIdentifier/RangeProof/
// TransactionKernel deserializers and segment validation are instrumented).
//
// Selector byte chooses the wire type; the rest is the body:
//   0 -> OutputBitmapSegment (22): block_hash || BitmapSegment || output_root
//   1 -> OutputSegment (24):       block_hash || Segment<OutputIdentifier> || output_bitmap_root
//   2 -> RangeProofSegment (26):  block_hash || Segment<RangeProof>
//   3 -> KernelSegment (28):      block_hash || Segment<TransactionKernel>
//
// After deserialization the payload is re-serialized and, for a bounded segment
// geometry, Root()/Validate() are exercised. Validation against a zero root is
// expected to throw SegmentException (controlled rejection); the point is that
// the instrumented code never reads/writes out of bounds or allocates unboundedly.

#include "fuzz_target.h"

#include <P2P/Messages/SegmentResponseMessage.h>

#include <Core/Serialization/ByteBuffer.h>
#include <Crypto/Models/Hash.h>

#include <cstdint>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

constexpr std::uint64_t kFixedMmrSize = 8192; // bounded geometry for Root/Validate

template <typename T>
void BoundedValidate(const Segment<T>& segment) {
    const SegmentIdentifier& id = segment.GetIdentifier();
    if (id.GetHeight() > 13) { return; }
    if (id.SegmentUnprunedSize(kFixedMmrSize) == 0) { return; }
    const std::pair<std::uint64_t, std::uint64_t> range = id.SegmentPosRange(kFixedMmrSize);
    if (range.first > range.second || range.second >= kFixedMmrSize) { return; }

    try {
        (void)segment.Root(kFixedMmrSize);
    } catch (const SegmentException&) {
        // missing leaf/hash for a mutated payload: controlled expectation
    }
    try {
        segment.Validate(kFixedMmrSize, HASH::ZERO);
    } catch (const SegmentException&) {
        // proof mismatch against the zero root: controlled rejection
    }
}

} // namespace

extern "C" const char* FuzzTargetName() { return "p2p_responses"; }

extern "C" const char* FuzzTargetDescription() {
    return "SegmentResponseMessage 22/24/26/28 deserialize + SerializePayload + bounded Root/Validate";
}

extern "C" void FuzzTargetCase(const std::uint8_t* data, std::size_t size) noexcept(false) {
    static const MessageTypes::EMessageType kTypes[4] = {
        MessageTypes::OutputBitmapSegment,
        MessageTypes::OutputSegment,
        MessageTypes::RangeProofSegment,
        MessageTypes::KernelSegment,
    };

    if (size == 0) {
        ByteBuffer empty(std::vector<std::uint8_t>{});
        (void)SegmentResponseMessage::Deserialize(empty, MessageTypes::OutputSegment);
        return;
    }

    const MessageTypes::EMessageType type = kTypes[data[0] % 4];
    const std::vector<std::uint8_t> body(data + 1, data + size);
    ByteBuffer buffer(body);
    const SegmentResponseMessage response = SegmentResponseMessage::Deserialize(buffer, type);
    (void)response.SerializePayload();

    std::visit([](const auto& payload) {
        using P = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<P, BitmapSegment>) {
            BoundedValidate(payload.ToSegment());
        } else {
            BoundedValidate(payload);
        }
    }, response.GetPayload());
}
