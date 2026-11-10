#pragma once

#include "ConfigProps.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include <json/json.h>
#include <Common/Logger.h>
#include <Core/Enums/Environment.h>
#include <Net/IPAddress.h>

#include <Core/Exceptions/FileException.h>
#include <Core/Global.h>
#include <P2P/PihdParams.h>

class P2PConfig
{
public:
	int GetMaxConnections() const noexcept { return m_maxConnections; }
	void SetMaxConnections(const int max_connections) noexcept { m_maxConnections = max_connections; }

	int GetMinConnections() const noexcept { return m_minConnections; }
	void SetMinConnections(const int min_connections) noexcept { m_minConnections = min_connections; }

	uint16_t GetP2PPort() const noexcept { return m_port; }
	// Port used for outbound connections to peers. Defaults to the listening port;
	// they differ only when running behind a forward or on a shared test host.
	uint16_t GetPeerPort() const noexcept { return m_peerPort; }
	// Allows running the legacy txhashset.zip sync path on purpose (tests,
	// diagnostics, or peers that cannot serve PIBD).
	bool IsPibdEnabled() const noexcept { return m_pibdEnabled; }
	Pihd::EPihdMode GetPihdMode() const noexcept { return m_pihdMode; }
	const std::vector<uint8_t>& GetMagicBytes() const noexcept { return m_magicBytes; }

	uint8_t GetMinSyncPeers() const noexcept { return m_minSyncPeers; }

    const std::unordered_set<IPAddress>& GetPreferredPeers() const noexcept { return m_peferredPeers; }
	const std::unordered_set<IPAddress>& GetAllowedPeers() const noexcept { return m_allowedPeers; }
	const std::unordered_set<IPAddress>& GetBlockedPeers() const noexcept { return m_blockedPeers; }
	const std::unordered_set<IPAddress>& GetBlockedOutboundPeers() const noexcept { return m_blockedOutboundPeers; }

	void SetPreferredPeers(const std::unordered_set<IPAddress>& peers) noexcept { m_peferredPeers = peers; };
	void SetAllowedPeers(const std::unordered_set<IPAddress>& peers) noexcept { m_allowedPeers = peers; };
	void SetBlockedPeers(const std::unordered_set<IPAddress>& peers) noexcept { m_blockedPeers = peers; };
		
	//
	// Constructor
	//
	P2PConfig(const Environment env, const Json::Value& json)
	{
		m_port = 13414;
		m_magicBytes = { 83, 59 };

		m_maxConnections = 60;
		m_minConnections = 10;

		m_minSyncPeers = 3;

		if (env == Environment::MAINNET) {
			m_port = 3414;
			m_magicBytes = { 97, 61 };
		}
		if (env == Environment::AUTOMATED_TESTING) {
			m_minSyncPeers = 1;
		}

		if (!json.isMember(ConfigProps::P2P::P2P)) {
			m_peerPort = m_port;
			return;
		}

		const Json::Value& p2pJSON = json[ConfigProps::P2P::P2P];

		if (p2pJSON.isMember(ConfigProps::P2P::MAX_PEERS)) {
			m_maxConnections = p2pJSON.get(ConfigProps::P2P::MAX_PEERS, 60).asInt();
		}

		if (p2pJSON.isMember(ConfigProps::P2P::MIN_PEERS)) {
			m_minConnections = p2pJSON.get(ConfigProps::P2P::MIN_PEERS, 10).asInt();
		}

		auto parsePort = [](const Json::Value& value, const uint16_t fallback) -> uint16_t {
			if (!value.isInt() && !value.isUInt()) {
				LOG_WARNING("Ignoring non-integer P2P port; keeping default.");
				return fallback;
			}
			const Json::Int64 port = value.asInt64();
			if (port < 1 || port > 65535) {
				LOG_WARNING("Ignoring out-of-range P2P port; keeping default.");
				return fallback;
			}
			return static_cast<uint16_t>(port);
		};

		if (p2pJSON.isMember(ConfigProps::P2P::P2P_PORT)) {
			m_port = parsePort(p2pJSON[ConfigProps::P2P::P2P_PORT], m_port);
		}

		m_peerPort = m_port;
		if (p2pJSON.isMember(ConfigProps::P2P::P2P_PEER_PORT)) {
			m_peerPort = parsePort(p2pJSON[ConfigProps::P2P::P2P_PEER_PORT], m_port);
		}

		if (p2pJSON.isMember(ConfigProps::P2P::PIBD_ENABLED)) {
			m_pibdEnabled = p2pJSON.get(ConfigProps::P2P::PIBD_ENABLED, true).asBool();
		}

		if (p2pJSON.isMember(ConfigProps::P2P::PIHD_MODE)) {
			const std::string mode = p2pJSON.get(ConfigProps::P2P::PIHD_MODE, "off").asString();
			if (mode == "client" || mode == "full") {
				// Serving is not implemented yet; "full" must not advertise
				// PIHD_HIST or promise segments it cannot return.
				m_pihdMode = Pihd::EPihdMode::CLIENT;
			} else {
				m_pihdMode = Pihd::EPihdMode::OFF;
			}
		}

		if (p2pJSON.isMember(ConfigProps::P2P::MIN_SYNC_PEERS)) {
			m_minSyncPeers = (uint8_t)p2pJSON.get(ConfigProps::P2P::MIN_SYNC_PEERS, 3).asInt();
		}

		if (p2pJSON.isMember(ConfigProps::P2P::PREFERRED_PEERS)) {
			Json::Value peers = p2pJSON.get(ConfigProps::P2P::PREFERRED_PEERS, Json::Value(Json::nullValue));
			for (auto& peer : peers) { 
				const std::string& addressStr = peer.asCString();
				const std::vector<IPAddress>& iPAddresses = GetValidIPAddress(addressStr);
				m_peferredPeers.insert(iPAddresses.begin(), iPAddresses.end());
			}
		}

		if (p2pJSON.isMember(ConfigProps::P2P::ALLOWED_PEERS)) {
			Json::Value peers = p2pJSON.get(ConfigProps::P2P::ALLOWED_PEERS, Json::Value(Json::nullValue));
			for (auto& peer : peers) {
				const std::string& addressStr = peer.asCString();
				const std::vector<IPAddress>& iPAddresses = GetValidIPAddress(addressStr);
				m_allowedPeers.insert(iPAddresses.begin(), iPAddresses.end());
			}
		}

		if (p2pJSON.isMember(ConfigProps::P2P::BLOCKED_OUTBOUND_PEERS)) {
			Json::Value peers = p2pJSON.get(ConfigProps::P2P::BLOCKED_OUTBOUND_PEERS, Json::Value(Json::nullValue));
			for (auto& peer : peers) {
				const std::string& addressStr = peer.asCString();
				const std::vector<IPAddress>& iPAddresses = GetValidIPAddress(addressStr);
				m_blockedOutboundPeers.insert(iPAddresses.begin(), iPAddresses.end());
			}
		}

		if (p2pJSON.isMember(ConfigProps::P2P::BLOCKED_PEERS)) {
			Json::Value peers = p2pJSON.get(ConfigProps::P2P::BLOCKED_PEERS, Json::Value(Json::nullValue));
			for (auto& peer : peers) {
				const std::string& addressStr = peer.asCString();
				const std::vector<IPAddress>& iPAddresses = GetValidIPAddress(addressStr);
				m_blockedPeers.insert(iPAddresses.begin(), iPAddresses.end());
			}
		}
	}

private:
	int m_maxConnections;
	int m_minConnections;
	uint16_t m_port;
	uint16_t m_peerPort;
	bool m_pibdEnabled = true;
	Pihd::EPihdMode m_pihdMode = Pihd::EPihdMode::OFF;
	std::vector<uint8_t> m_magicBytes;
	uint8_t m_minSyncPeers;
	std::unordered_set<IPAddress> m_peferredPeers;
	std::unordered_set<IPAddress> m_allowedPeers;
	std::unordered_set<IPAddress> m_blockedPeers;
	std::unordered_set<IPAddress> m_blockedOutboundPeers;

	static std::vector<IPAddress> GetValidIPAddress(const std::string& addressStr)
    {
		std::vector<IPAddress> ipList;

        if (IPAddress::IsValidIPAddress(addressStr)) {
			try {
				ipList.push_back(IPAddress::Parse(addressStr));
			} catch (std::exception& e) {
				LOG_DEBUG_F("Error parsing IP Address: {} {}", addressStr, e.what());
			}
		} else {
			LOG_INFO_F("Resolving domain: {}", addressStr);
			try {
				for (const IPAddress ipAddress : IPAddress::Resolve(addressStr))
				{
					LOG_INFO_F("Resolved domain: {}", ipAddress);
					ipList.push_back(ipAddress);
				}
			} catch (std::exception& e) {
				LOG_DEBUG_F("Failed to resolve domain: {} {}", addressStr, e.what());
			}
		}

		return ipList;
    };
};