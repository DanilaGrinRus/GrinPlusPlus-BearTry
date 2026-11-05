#include "DNSSeeder.h"

#include <Common/Logger.h>
#include <Core/Config.h>
#include <Core/Global.h>
#include <asio.hpp>

std::vector<SocketAddress> DNSSeeder::GetPeersFromDNS()
{
	std::vector<SocketAddress> addresses;

	std::vector<std::string> dnsSeeds;
	if (Global::IsMainnet())
	{
		// DNS seeds from Grin 5.5.2; these are discovery sources, not an allowlist.
		dnsSeeds = {
			"mainnet-seed.grinnode.live",
			"grincoin.org",
			"main.gri.mw",
			"mainnet.grinffindor.org",
			"main-seed.grin.money",
			"mainnet.fountainoffairfortune.it"
		};
	}
	else
	{
		dnsSeeds = {
			"testnet.grincoin.org",
			"test.gri.mw",
			"testnet.grinffindor.org",
			"test-seed.grin.money",
			"testnet.fountainoffairfortune.it"
		};
	}

	for (auto seed : dnsSeeds)
	{
		LOG_TRACE_F("Checking seed: {}", seed);
		const std::vector<IPAddress> ipAddresses = IPAddress::Resolve(seed);
		for (const IPAddress ipAddress : ipAddresses)
		{
			LOG_TRACE_F("IP Address: {}", ipAddress);
			addresses.emplace_back(SocketAddress(ipAddress, Global::GetConfig().GetP2PPort()));
		}
	}

	return addresses;
}

