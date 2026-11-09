// Fuzz target: PIBD segment/proof/bitmap deserialization (parse-only harness).
//
// Selector byte chooses the parser:
//   0 SegmentIdentifier, 1 SegmentProof, 2 BitmapBlock, 3 BitmapChunk,
//   4 BitmapSegment, 5 Segment<BitmapChunk>, 6 Segment<T> GetLeafDataAt probe,
//   7 SegmentProof with an absurd declared hash count (overflow boundary).
//
// Every count in these formats is bounded by the remaining payload size or by
// MAX_SEGMENT_READ_ITEMS/MAX_HASHES inside the parser.
//
// Validation (Segment::Root / SegmentProof::ReconstructRoot) hashes data through
// Crypto.lib's Hasher, and the ordinary project libraries are built without
// AddressSanitizer: their MSVC STL detect_mismatch pragmas (annotate_string /
// annotate_vector) are incompatible with ASan translation units (LNK2038).
// Root reconstruction is therefore NOT part of this ASan harness and remains
// covered by the ordinary unit tests; see docs/modernization/SANITIZERS.md.

#include "fuzz_target.h"

#include <PMMR/Models/BitmapBlock.h>
#include <PMMR/Models/BitmapChunk.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/Segment.h>
#include <PMMR/Models/SegmentProof.h>

#include <Core/Serialization/ByteBuffer.h>

#include <cstdint>
#include <vector>

extern "C" const char* FuzzTargetName() { return "segment"; }

extern "C" const char* FuzzTargetDescription() {
    return "Segment/SegmentProof/SegmentIdentifier/BitmapBlock/BitmapChunk/BitmapSegment parsing";
}

extern "C" void FuzzTargetCase(const std::uint8_t* data, std::size_t size) noexcept(false) {
    if (size == 0) { return; }
    const std::uint8_t op = data[0];
    const std::vector<std::uint8_t> body(data + 1, data + size);
    ByteBuffer buffer(body);

    switch (op % 8) {
        case 0: {
            (void)SegmentIdentifier::Deserialize(buffer);
            break;
        }
        case 1: {
            (void)SegmentProof::Deserialize(buffer);
            break;
        }
        case 2: {
            (void)BitmapBlock::Deserialize(buffer);
            break;
        }
        case 3: {
            (void)BitmapChunk::Deserialize(buffer);
            break;
        }
        case 4: {
            (void)BitmapSegment::Deserialize(buffer);
            break;
        }
        case 5: {
            (void)Segment<BitmapChunk>::Deserialize(buffer);
            break;
        }
        case 6: {
            const Segment<BitmapChunk> segment = Segment<BitmapChunk>::Deserialize(buffer);
            (void)segment.GetLeafDataAt(0);
            break;
        }
        default: {
            // Overflow boundary on the declared hash count.
            const std::vector<std::uint8_t> huge = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            ByteBuffer crafted(huge);
            (void)SegmentProof::Deserialize(crafted);
            break;
        }
    }
}
