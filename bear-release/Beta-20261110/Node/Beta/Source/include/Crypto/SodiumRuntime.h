#pragma once

#include <sodium.h>

// Called before any node/test crypto. libsodium chooses only implementations
// supported by the current CPU and OS; a failed init must stop startup.
namespace SodiumRuntime
{
    inline bool Initialize() noexcept { return sodium_init() >= 0; }
}
