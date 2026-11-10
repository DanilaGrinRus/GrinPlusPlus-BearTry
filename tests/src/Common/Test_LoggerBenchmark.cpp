#include <catch.hpp>
#include <Common/Logger.h>
#include <chrono>
#include <iostream>

TEST_CASE("Logger disabled debug formatting benchmark", "[loggerbench][.]")
{
	constexpr int iterations = 2000000;
	const auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < iterations; ++i) {
		LOG_DEBUG_F("disabled debug value={} text={}", i, "benchmark");
	}
	const auto disabledMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();

	const auto formatStart = std::chrono::steady_clock::now();
	volatile size_t sink = 0;
	for (int i = 0; i < iterations; ++i) {
		sink = StringUtil::Format("disabled debug value={} text={}", i, "benchmark").size();
	}
	const auto formatMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - formatStart).count();

	std::cout << "LOGGERBENCH iterations=" << iterations
		<< " disabled_ms=" << disabledMs
		<< " format_ms=" << formatMs
		<< " disabled_ns_per_op=" << (disabledMs * 1000000LL / iterations)
		<< " format_ns_per_op=" << (formatMs * 1000000LL / iterations)
		<< " sink=" << sink << std::endl;
	REQUIRE(disabledMs >= 0);
	REQUIRE(formatMs >= 0);
}





