#include <catch.hpp>
#include <Crypto/SodiumRuntime.h>
#include <Crypto/Hasher.h>
#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("SodiumRuntime initialization is repeatable and thread-safe")
{
    std::atomic<bool> ok{true};
    std::array<std::thread, 8> workers;
    for (auto& worker : workers) worker = std::thread([&ok] { if (!SodiumRuntime::Initialize()) ok.store(false); });
    for (auto& worker : workers) worker.join();
    REQUIRE(ok.load());
    REQUIRE(SodiumRuntime::Initialize());
}

TEST_CASE("SodiumRuntime BLAKE2b-256 known vectors")
{
    const std::vector<uint8_t> empty;
    const std::vector<uint8_t> abc = { 0x61, 0x62, 0x63 };
    REQUIRE(Hasher::Blake2b(empty).ToHex() == "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");
    REQUIRE(Hasher::Blake2b(abc).ToHex() == "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");
}
