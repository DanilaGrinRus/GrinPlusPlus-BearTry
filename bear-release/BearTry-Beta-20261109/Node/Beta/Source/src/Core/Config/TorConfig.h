#pragma once

#include "ConfigProps.h"

#include <json/json.h>

#include <filesystem.h>

#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
#include <chrono>
#include <fstream>
#include <iostream>

class TorConfig
{
public:
	bool IsEnabled() const noexcept { return true; }

	// The "SocksPort" https://2019.www.torproject.org/docs/tor-manual.html.en#SocksPort
	uint16_t GetSocksPort() const noexcept { return m_socksPort; }

	// The "ControlPort" https://2019.www.torproject.org/docs/tor-manual.html.en#ControlPort
	uint16_t GetControlPort() const noexcept { return m_controlPort; }

	// The pre-hashed "ControlPassword" https://2019.www.torproject.org/docs/tor-manual.html.en#HashedControlPassword
	const std::string& GetControlPassword() const noexcept { return m_password; }

	// The "HashedControlPassword" https://2019.www.torproject.org/docs/tor-manual.html.en#HashedControlPassword
	const std::string& GetHashedControlPassword() const noexcept { return m_hashedPassword; }

	const fs::path& GetTorDataPath() const noexcept { return m_torDataPath; }

	const fs::path& GetTorrcPath() const noexcept { return m_torrcPath; }

	const std::string ReadTorrcFile() const noexcept
	{
		std::ifstream torrc(m_torrcPath);
		std::stringstream buffer;
		buffer << torrc.rdbuf();
		return buffer.str();
	}
	
	bool WriteTorrcContent(const std::string& content) const noexcept
	{
		const fs::path tempTorrc = CreateTempTorrcPath();
		std::ofstream out(tempTorrc, std::ofstream::out | std::ofstream::trunc | std::ofstream::binary);
		if (!out) {
			return false;
		}
		out.write(content.data(), (std::streamsize)content.size());
		out.close();
		if (out.fail()) {
			std::error_code removeEc;
			fs::remove(tempTorrc, removeEc);
			return false;
		}
		std::string error;
		return ReplaceTorrc(tempTorrc, error);
	}

	bool AddObfs4TorBridge(std::string bridge) const noexcept
	{
		if (bridge.empty()) {
			return true;
		}

		std::string error;
		if (!IsObfs4ConfigPresent() && !AddObfs4Config(error)) {
			return false;
		}

		std::vector<std::string> lines;
		if (!ReadTorrcLines(lines, error)) {
			return false;
		}
		lines.push_back("Bridge " + bridge);
		return WriteTorrcLines(lines, error);
	}

	void ClearTorrcFile() const noexcept
	{
		WriteTorrcContent("");
	}

	bool DisableObfsBridges() const noexcept
	{
		if (!IsObfs4ConfigPresent()) {
			return true;
		}
		std::string error;
		return RemoveObfs4Config(error);
	}

	bool EnableSnowflake() const noexcept
	{
		if (IsSnowflakeConfigPresent()) {
			return true;
		}
		std::string error;
		return AddSnowflakeConfig(error);
	}

	bool DisableSnowflake() const noexcept
	{
		if (!IsSnowflakeConfigPresent()) {
			return true;
		}
		std::string error;
		return RemoveSnowflakeConfig(error);
	}

	bool IsTorBridgesEnabled() const noexcept
	{
		bool enabled = false;
		std::ifstream configFile(m_torrcPath);
		std::string line;
		while (std::getline(configFile, line))
		{
			if (line.find("UseBridges 1") != std::string::npos)
			{
				enabled = true;
				break;
			}
		}

		return enabled;
	}

	bool IsObfs4ConfigPresent() const noexcept
	{
		bool enabled = false;
		std::ifstream configFile(m_torrcPath);
		std::string line;
		while (std::getline(configFile, line))
		{
			if (line.find("ClientTransportPlugin obfs4 exec") != std::string::npos)
			{
				enabled = true;
				break;
			}
		}

		return enabled;
	}

	bool IsSnowflakeConfigPresent() const noexcept
	{
		bool enabled = false;
		std::ifstream configFile(m_torrcPath);
		std::string line;
		while (std::getline(configFile, line))
		{
			if (line.find("ClientTransportPlugin snowflake exec") != std::string::npos)
			{
				enabled = true;
				break;
			}
		}

		return enabled;
	}

	bool DisableTorBridges() const noexcept
	{
		if (IsSnowflakeConfigPresent()) {
			return DisableSnowflake();
		}
		return DisableObfsBridges();
	}
	std::vector<std::string> ListTorBridges() const noexcept
	{
		std::vector<std::string> bridges;
		std::ifstream configFile(m_torrcPath);
		std::string line;
		while (std::getline(configFile, line))
		{
			if (line.rfind("Bridge", 0) == 0)
			{
				bridges.push_back(line.substr(7)); // Bridges length = 6
			}
		}

		return bridges;
	}

	//
	// Constructor
	//
	TorConfig(const Json::Value& json, const fs::path& torDataPath)
	{
		m_socksPort = 3422;
		m_controlPort = 3423;
		m_password = "";
		m_hashedPassword = "";
		m_torDataPath = torDataPath;
		m_torrcPath = torDataPath / ".torrc";
		fs::create_directories(torDataPath);
		if (!fs::exists(m_torrcPath)) std::ofstream file(m_torrcPath);

		if (json.isMember(ConfigProps::Tor::TOR))
		{
			const Json::Value& torJSON = json[ConfigProps::Tor::TOR];

			m_socksPort = (uint16_t)torJSON.get(ConfigProps::Tor::SOCKS_PORT, m_socksPort).asUInt();
			m_controlPort = (uint16_t)torJSON.get(ConfigProps::Tor::CONTROL_PORT, m_controlPort).asUInt();

			if (torJSON.isMember(ConfigProps::Tor::PASSWORD) && torJSON.isMember(ConfigProps::Tor::HASHED_PASSWORD))
			{
				m_password = torJSON[ConfigProps::Tor::PASSWORD].asCString();
				m_hashedPassword = torJSON[ConfigProps::Tor::HASHED_PASSWORD].asCString();
			}
		}
	}

	TorConfig(const uint16_t socksPort, const uint16_t controlPort, const fs::path& torDataPath)
		: m_socksPort(socksPort),
		m_controlPort(controlPort),
		m_password(""),
		m_hashedPassword(""),
		m_torDataPath(torDataPath)
	{

	}

private:
	uint16_t m_socksPort;
	uint16_t m_controlPort;
	std::string m_password;
	std::string m_hashedPassword;
	fs::path m_torDataPath;
	fs::path m_torrcPath;

	bool ReadTorrcLines(std::vector<std::string>& lines, std::string& error) const noexcept
	{
		lines.clear();
		std::ifstream configFile(m_torrcPath);
		if (!configFile)
		{
			if (!fs::exists(m_torrcPath)) {
				return true;
			}
			error = "failed to open torrc for reading";
			return false;
		}

		std::string line;
		while (std::getline(configFile, line)) {
			lines.push_back(line);
		}
		if (configFile.bad()) {
			error = "failed while reading torrc";
			return false;
		}
		return true;
	}

	static std::string TrimCopy(const std::string& value) noexcept
	{
		const size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) {
			return "";
		}
		const size_t last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	static bool StartsWith(const std::string& value, const char* prefix) noexcept
	{
		return value.rfind(prefix, 0) == 0;
	}

	static bool IsObfs4ManagedLine(const std::string& trimmed) noexcept
	{
		return StartsWith(trimmed, "ClientTransportPlugin obfs4")
			|| StartsWith(trimmed, "Bridge obfs4");
	}

	static bool IsSnowflakeManagedLine(const std::string& trimmed) noexcept
	{
		return StartsWith(trimmed, "ClientTransportPlugin snowflake")
			|| StartsWith(trimmed, "Bridge snowflake");
	}

	bool ReplaceTorrc(const fs::path& temp, std::string& error) const noexcept
	{
		std::error_code ec;
#ifdef _WIN32
		if (!MoveFileExW(temp.wstring().c_str(), m_torrcPath.wstring().c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			error = "atomic torrc replace failed";
			std::error_code removeEc;
			fs::remove(temp, removeEc);
			return false;
		}
#else
		fs::rename(temp, m_torrcPath, ec);
		if (ec) {
			error = "atomic torrc replace failed: " + ec.message();
			fs::remove(temp, ec);
			return false;
		}
#endif
		return true;
	}

	fs::path CreateTempTorrcPath() const noexcept
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		for (int attempt = 0; attempt < 32; ++attempt) {
			fs::path candidate = m_torrcPath;
			candidate += ".tmp." + std::to_string(stamp) + "." + std::to_string(attempt);
			std::error_code ec;
			if (!fs::exists(candidate, ec) && !fs::is_symlink(candidate, ec)) {
				return candidate;
			}
		}
		return m_torrcPath.string() + ".tmp";
	}

	bool WriteTorrcLines(const std::vector<std::string>& lines, std::string& error) const noexcept
	{
		const fs::path tempTorrc = CreateTempTorrcPath();
		std::ofstream newConfigFile(tempTorrc, std::ofstream::out | std::ofstream::trunc);
		if (!newConfigFile) {
			error = "failed to open temporary torrc for writing";
			return false;
		}

		for (const std::string& line : lines) {
			newConfigFile << line << "\n";
		}
		newConfigFile.close();
		if (newConfigFile.fail()) {
			error = "failed to write temporary torrc";
			std::error_code removeEc;
			fs::remove(tempTorrc, removeEc);
			return false;
		}

		return ReplaceTorrc(tempTorrc, error);
	}

	bool RewriteTorrc(bool snowflake, bool enable, std::string& error) const noexcept
	{
		std::vector<std::string> lines;
		if (!ReadTorrcLines(lines, error)) {
			return false;
		}

		std::vector<std::string> rewritten;
		rewritten.reserve(lines.size() + 4);
		for (const std::string& line : lines)
		{
			const std::string trimmed = TrimCopy(line);
			if (StartsWith(trimmed, "UseBridges")) {
				continue;
			}

			if (enable) {
				if (IsObfs4ManagedLine(trimmed) || IsSnowflakeManagedLine(trimmed)) {
					continue;
				}
			} else if (snowflake) {
				if (IsSnowflakeManagedLine(trimmed)) {
					continue;
				}
			} else {
				if (IsObfs4ManagedLine(trimmed)) {
					continue;
				}
			}

			rewritten.push_back(line);
		}

		if (enable) {
			rewritten.push_back("UseBridges 1");
			if (snowflake) {
#ifdef _WIN32
				const fs::path snowflakeDir = (m_torDataPath / "PluggableTransports" / "snowflake-client.exe");
#else
				const fs::path snowflakeDir = (m_torDataPath / "PluggableTransports" / "snowflake-client");
#endif
				rewritten.push_back("ClientTransportPlugin snowflake exec " + snowflakeDir.u8string()
					+ " -url https://snowflake-broker.torproject.net.global.prod.fastly.net/ -front cdn.sstatic.net -ice stun:stun.l.google.com:19302,stun:stun.voip.blackberry.com:3478,stun:stun.altar.com.pl:3478,stun:stun.antisip.com:3478,stun:stun.bluesip.net:3478,stun:stun.dus.net:3478,stun:stun.epygi.com:3478,stun:stun.sonetel.com:3478,stun:stun.sonetel.net:3478,stun:stun.stunprotocol.org:3478,stun:stun.uls.co.za:3478,stun:stun.voipgate.com:3478,stun:stun.voys.nl:3478");
				rewritten.push_back("Bridge snowflake 192.0.2.3:1 2B280B23E1107BB62ABFC40DDCC8824814F80A72");
			}
			else {
#ifdef _WIN32
				const fs::path obfs4Dir = (m_torDataPath / "PluggableTransports" / "obfs4proxy.exe");
#else
				const fs::path obfs4Dir = (m_torDataPath / "PluggableTransports" / "obfs4proxy");
#endif
				rewritten.push_back("ClientTransportPlugin obfs4 exec " + obfs4Dir.u8string());
			}
		}
		else
		{
			bool needsUseBridges = false;
			for (const std::string& line : rewritten) {
				const std::string trimmed = TrimCopy(line);
				if (StartsWith(trimmed, "ClientTransportPlugin") || StartsWith(trimmed, "Bridge ")) {
					needsUseBridges = true;
					break;
				}
			}
			if (needsUseBridges) {
				rewritten.push_back("UseBridges 1");
			}
		}

		return WriteTorrcLines(rewritten, error);
	}

	bool AddObfs4Config(std::string& error) const noexcept
	{
		return RewriteTorrc(false, true, error);
	}

	bool AddSnowflakeConfig(std::string& error) const noexcept
	{
		return RewriteTorrc(true, true, error);
	}

	bool RemoveObfs4Config(std::string& error) const noexcept
	{
		return RewriteTorrc(false, false, error);
	}

	bool RemoveSnowflakeConfig(std::string& error) const noexcept
	{
		return RewriteTorrc(true, false, error);
	}
};
