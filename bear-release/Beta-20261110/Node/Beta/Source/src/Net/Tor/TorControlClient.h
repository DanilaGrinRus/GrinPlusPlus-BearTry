#pragma once

#include <Net/Clients/Client.h>
#include <Net/Tor/TorException.h>
#include <Common/Logger.h>
#include <filesystem>

static const std::chrono::seconds TOR_CONTROL_TIMEOUT = std::chrono::seconds(3);
static const std::chrono::seconds TOR_STARTUP_TIMEOUT = std::chrono::seconds(10);

class TorControlClient : public Client<std::string, std::vector<std::string>>
{
public:
	using UPtr = std::unique_ptr<TorControlClient>;

	static TorControlClient::UPtr Connect(const uint16_t control_port, const std::string& password);

	//
	// T2: authenticate with Tor's CookieAuthentication file (32-byte cookie,
	// sent as hex). Avoids any hardcoded control password.
	//
	static TorControlClient::UPtr ConnectCookie(const uint16_t control_port, const std::filesystem::path& cookieFile);

	//
	// Writes the given string to the socket, and reads each line until "250 OK" is read.
	// throws TorException - If a line is read indicating a failure (ie not prefixed with "250").
	//
	std::vector<std::string> Invoke(const std::string& request) final;

private:
	bool IsBootstrapped();
	bool Connect(const SocketAddress& address);
};