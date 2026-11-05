#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace NodeAPIParsing
{
	inline std::optional<uint64_t> UnsignedDecimal(const std::string& text) noexcept
	{
		if (text.empty()) return std::nullopt;
		uint64_t value = 0;
		for (const char c : text) {
			if (c < '0' || c > '9') return std::nullopt;
			const uint64_t digit = static_cast<uint64_t>(c - '0');
			if (value > ((std::numeric_limits<uint64_t>::max)() - digit) / 10) return std::nullopt;
			value = value * 10 + digit;
		}
		return value;
	}
}
