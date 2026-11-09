#include <catch.hpp>

#include <Core/Config/TorConfig.h>
#include <json/json.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
	fs::path MakeTorConfigDir()
	{
		fs::path dir = fs::temp_directory_path()
			/ ("grinpp-torcfg-" + std::to_string((unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::create_directories(dir);
		return dir;
	}

	std::string ReadAll(const fs::path& path)
	{
		std::ifstream in(path);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	void WriteAll(const fs::path& path, const std::string& content)
	{
		std::ofstream out(path, std::ios::out | std::ios::trunc);
		out << content;
	}
}

TEST_CASE("TorConfig preserves unrelated torrc lines when enabling bridges")
{
	const fs::path dir = MakeTorConfigDir();
	TorConfig config(Json::Value(), dir);
	WriteAll(config.GetTorrcPath(),
		"SocksPort 9050\n"
		"Nickname my-node\n"
		"# operator comment: do not lose\n"
		"\n");

	REQUIRE(config.AddObfs4TorBridge("obfs4 192.0.2.10:443 ABCDEF cert=xyz iat-mode=2"));

	const std::string torrc = ReadAll(config.GetTorrcPath());
	REQUIRE(torrc.find("SocksPort 9050") != std::string::npos);
	REQUIRE(torrc.find("Nickname my-node") != std::string::npos);
	REQUIRE(torrc.find("# operator comment: do not lose") != std::string::npos);
	REQUIRE(torrc.find("UseBridges 1") != std::string::npos);
	REQUIRE(torrc.find("ClientTransportPlugin obfs4") != std::string::npos);
	REQUIRE(torrc.find("Bridge obfs4 192.0.2.10:443") != std::string::npos);
	REQUIRE(config.IsObfs4ConfigPresent());
	REQUIRE(config.IsTorBridgesEnabled());
}

TEST_CASE("TorConfig transitions Snowflake <-> obfs4 <-> disabled preserve unrelated lines")
{
	const fs::path dir = MakeTorConfigDir();
	TorConfig config(Json::Value(), dir);
	WriteAll(config.GetTorrcPath(), "SocksPort 9050\nNickname my-node\n# operator comment\n");

	REQUIRE(config.EnableSnowflake());
	std::string torrc = ReadAll(config.GetTorrcPath());
	REQUIRE(torrc.find("ClientTransportPlugin snowflake") != std::string::npos);
	REQUIRE(torrc.find("Bridge snowflake") != std::string::npos);
	REQUIRE(torrc.find("ClientTransportPlugin obfs4") == std::string::npos);
	REQUIRE(torrc.find("SocksPort 9050") != std::string::npos);
	REQUIRE(torrc.find("Nickname my-node") != std::string::npos);
	REQUIRE(torrc.find("# operator comment") != std::string::npos);

	REQUIRE(config.AddObfs4TorBridge("obfs4 192.0.2.20:443 DEF cert=xyz"));
	torrc = ReadAll(config.GetTorrcPath());
	REQUIRE(torrc.find("ClientTransportPlugin obfs4") != std::string::npos);
	REQUIRE(torrc.find("Bridge obfs4 192.0.2.20:443") != std::string::npos);
	REQUIRE(torrc.find("ClientTransportPlugin snowflake") == std::string::npos);
	REQUIRE(torrc.find("Bridge snowflake") == std::string::npos);
	REQUIRE(torrc.find("SocksPort 9050") != std::string::npos);

	REQUIRE(config.DisableObfsBridges());
	torrc = ReadAll(config.GetTorrcPath());
	REQUIRE(torrc.find("ClientTransportPlugin obfs4") == std::string::npos);
	REQUIRE(torrc.find("Bridge obfs4") == std::string::npos);
	REQUIRE(torrc.find("UseBridges 1") == std::string::npos);
	REQUIRE(torrc.find("SocksPort 9050") != std::string::npos);
	REQUIRE(torrc.find("Nickname my-node") != std::string::npos);
	REQUIRE(torrc.find("# operator comment") != std::string::npos);

	REQUIRE(config.EnableSnowflake());
	REQUIRE(config.DisableSnowflake());
	torrc = ReadAll(config.GetTorrcPath());
	REQUIRE(torrc.find("ClientTransportPlugin snowflake") == std::string::npos);
	REQUIRE(torrc.find("UseBridges 1") == std::string::npos);
	REQUIRE(torrc.find("SocksPort 9050") != std::string::npos);
}

#ifdef _WIN32
TEST_CASE("TorConfig reports write failure and leaves the original torrc unchanged")
{
	const fs::path dir = MakeTorConfigDir();
	TorConfig config(Json::Value(), dir);
	const std::string original = "SocksPort 9050\nNickname my-node\n# keep\n";
	WriteAll(config.GetTorrcPath(), original);

	std::error_code ec;
	fs::permissions(config.GetTorrcPath(), fs::perms::owner_read, fs::perm_options::replace, ec);
	REQUIRE_FALSE(ec);

	const bool written = config.AddObfs4TorBridge("obfs4 192.0.2.30:443 GHI cert=xyz");
	fs::permissions(config.GetTorrcPath(), fs::perms::owner_all, fs::perm_options::replace, ec);
	REQUIRE_FALSE(written);
	REQUIRE(ReadAll(config.GetTorrcPath()) == original);
}
#endif
