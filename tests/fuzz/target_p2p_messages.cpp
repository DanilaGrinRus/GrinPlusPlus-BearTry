// Fuzz target: PIBD P2P message bodies (Grin v5.5.2 ids 21..28).
//
// Selector byte chooses one wire path:
//   0..3 -> real SegmentRequestMessage::Deserialize for GetOutputBitmapSegment(21),
//           GetOutputSegment(23), GetRangeProofSegment(25), GetKernelSegment(27);
//   4    -> real BitmapSegment::Deserialize (body of OutputBitmapSegment(22)
//           after the 32-byte block hash), plus the trailing root read;
//   5..7 -> same request parser with a truncated body.
//
// SegmentResponseMessage::Deserialize for ids 24/26/28 instantiates
// Segment<OutputIdentifier/RangeProof/TransactionKernel>, which needs the
// ordinary Core/Crypto static libraries (built without ASan) and their MSVC STL
// annotation pragmas are incompatible with ASan objects (LNK2038). Those paths
// are therefore exercised by the normal unit tests only; this limitation is
// recorded in docs/modernization/SANITIZERS.md.

#include "fuzz_target.h"

#include <P2P/Messages/SegmentRequestMessage.h>
#include <PMMR/Models/BitmapSegment.h>

#include <Core/Serialization/ByteBuffer.h>
#include <Crypto/Models/Hash.h>

#include <cstdint>
#include <vector>

extern "C" const char* FuzzTargetName() { return "p2p_body"; }

extern "C" const char* FuzzTargetDescription() {
    return "SegmentRequestMessage bodies (21/23/25/27) and OutputBitmapSegment bitmap body (22)";
}

extern "C" void FuzzTargetCase(const std::uint8_t* data, std::size_t size) noexcept(false) {
    static const MessageTypes::EMessageType kRequests[4] = {
        MessageTypes::GetOutputBitmapSegment,
        MessageTypes::GetOutputSegment,
        MessageTypes::GetRangeProofSegment,
        MessageTypes::GetKernelSegment,
    };

    if (size == 0) {
        ByteBuffer empty(std::vector<std::uint8_t>{});
        (void)SegmentRequestMessage::Deserialize(empty, MessageTypes::GetOutputSegment);
        return;
    }

    const std::uint8_t selector = data[0];
    const std::vector<std::uint8_t> body(data + 1, data + size);

    if (selector % 8 < 4) {
        ByteBuffer buffer(body);
        const SegmentRequestMessage request =
            SegmentRequestMessage::Deserialize(buffer, kRequests[selector % 4]);
        (void)request.GetIdentifier();
        return;
    }

    if (selector % 8 == 4) {
        // OutputBitmapSegment body: block_hash(32) || BitmapSegment || output_root(32)
        ByteBuffer buffer(body);
        (void)buffer.ReadBigInteger<32>();
        (void)BitmapSegment::Deserialize(buffer);
        (void)buffer.ReadBigInteger<32>();
        return;
    }

    // Truncated request bodies (deep rejection path).
    const std::size_t keep = body.size() / 2;
    ByteBuffer truncated(std::vector<std::uint8_t>(body.begin(), body.begin() + (std::ptrdiff_t)keep));
    (void)SegmentRequestMessage::Deserialize(truncated, kRequests[selector % 4]);
}
