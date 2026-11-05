#pragma once

// Interface between the reusable bounded driver and one fuzz target.
// Each fuzz_<target>.exe links exactly one target translation unit.

#include <cstddef>
#include <cstdint>

extern "C" const char* FuzzTargetName();
extern "C" const char* FuzzTargetDescription();

// Executes one input. May throw (the driver classifies controlled rejections vs
// unexpected exceptions); must never perform unbounded work and must not write
// outside the directory named by GRINPP_FUZZ_STAGING. noexcept(false) is
// required because MSVC /EHc assumes extern "C" functions do not throw.
extern "C" void FuzzTargetCase(const std::uint8_t* data, std::size_t size) noexcept(false);
