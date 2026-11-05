#pragma once

#include <string>

// Forward Declarations
struct mg_connection;

class PeersAPI
{
public:
	static int GetAllPeers_Handler(struct mg_connection* conn, void* pNodeContext);
	static int GetConnectedPeers_Handler(struct mg_connection* conn, void* pNodeContext);
	// Explicit diagnostics endpoint: banned peers, current download source,
	// approximate PIBD speed and peer software versions. Not part of the
	// compatibility protocol; local node operators only.
	static int GetDiagnostics_Handler(struct mg_connection* conn, void* pNodeContext);
	static int Peer_Handler(struct mg_connection* conn, void* pNodeContext);

private:
	static std::string ParseIPAddress(const std::string& request);
	static std::string ParseCommand(const std::string& request);
};