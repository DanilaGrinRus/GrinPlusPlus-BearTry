// Fuzz target: Core/Serialization/ByteBuffer.h
//
// Exercises the read primitives used by every wire/snapshot parser:
// fixed-width big/little endian reads, 64-bit length-prefixed strings/vectors,
// fixed-size big integers, raw arrays and remaining-bytes reads. The buffer is
// attacker controlled; a correct implementation must either succeed or throw a
// DeserializationException - never read/write out of bounds or allocate without
// bound.

#include "fuzz_target.h"

#include <Core/Serialization/ByteBuffer.h>

#include <array>
#include <cstdint>
#include <vector>

extern "C" const char* FuzzTargetName() { return "bytebuffer"; }

extern "C" const char* FuzzTargetDescription() {
    return "ByteBuffer primitive reads incl. 64-bit length prefixes and overflow boundaries";
}

extern "C" void FuzzTargetCase(const std::uint8_t* data, std::size_t size) noexcept(false) {
    std::vector<std::uint8_t> bytes(data, data + size);
    ByteBuffer buffer(bytes);

    std::uint32_t ops = 0;
    // Bounded number of operations: the opcode stream is itself part of the
    // input, so this exercises long op sequences without unbounded work.
    while (buffer.GetRemainingSize() > 0 && ops++ < 512) {
        const std::uint8_t op = buffer.ReadU8();
        switch (op % 16) {
            case 0: (void)buffer.Read8(); break;
            case 1: (void)buffer.ReadU8(); break;
            case 2: (void)buffer.Read16(); break;
            case 3: (void)buffer.ReadU16(); break;
            case 4: (void)buffer.Read32(); break;
            case 5: (void)buffer.ReadU32(); break;
            case 6: (void)buffer.Read64(); break;
            case 7: (void)buffer.ReadU64(); break;
            case 8: (void)buffer.ReadU64_LE(); break;
            case 9: (void)buffer.ReadVarStr(); break;
            case 10: (void)buffer.ReadString((std::size_t)buffer.ReadU8()); break;
            case 11: (void)buffer.ReadBigInteger<32>(); break;
            case 12: (void)buffer.ReadBigInteger<64>(); break;
            case 13: {
                // Length prefix is parsed by the caller, exactly like the wire
                // format: attacker declared size, then a copy of that size.
                const std::uint64_t declared = buffer.ReadU64();
                (void)buffer.ReadVector(declared);
                break;
            }
            case 14: (void)buffer.ReadArray<16>(); break;
            default: (void)buffer.ReadRemainingBytes(); break;
        }
    }

    // Trait-based read path (ISerializable) also goes through RequireRemaining.
    if (buffer.GetRemainingSize() >= 2) {
        ByteBuffer second(bytes);
        (void)second.Read<std::uint64_t>();
    }
}
