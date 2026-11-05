#pragma once

#include <algorithm>
#include <string>

// Extract the address part of /v1/peers/<address>[/command]. Brackets are
// required when an IPv6 address is supplied together with a port.
namespace PeerAPIPath
{
	inline bool ValidPort(const std::string& port)
	{
		if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(),
			[](const char ch) { return ch >= '0' && ch <= '9'; })) return false;
		const unsigned long value = std::stoul(port);
		return value > 0 && value <= 65535;
	}

	inline std::string Address(const std::string& request)
	{
		const std::string host = request.substr(0, request.find('/'));
		if (host.empty()) return {};
		if (host.front() == '[') {
			const auto close = host.find(']');
			if (close == std::string::npos || close == 1) return {};
			const auto suffix = host.substr(close + 1);
			if (!suffix.empty() && (suffix.front() != ':' || !ValidPort(suffix.substr(1)))) return {};
			return host.substr(1, close - 1);
		}
		const auto first = host.find(':');
		if (first == std::string::npos || first != host.rfind(':')) return host;
		return ValidPort(host.substr(first + 1)) ? host.substr(0, first) : std::string();
	}
}
