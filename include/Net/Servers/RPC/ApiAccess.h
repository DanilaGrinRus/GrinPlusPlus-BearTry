#pragma once

#include <filesystem.h>
#include <string>

struct mg_connection;

// Central access policy for the Node HTTP/RPC servers.
//
// Access levels:
//   PUBLIC  - read-only data that is safe for any local client.
//   PRIVATE - read-only configuration/peer data; requires the API secret.
//   ADMIN   - state-changing operations (config updates, shutdown, resync,
//             peer bans); requires the API secret and a loopback Host/Origin.
//
// The secret is a 64-hex-character token stored in <dataDir>/.api_secret.
// It is generated on first start, never logged, and never placed in URLs.
// Requests authenticate with either
//   Authorization: Bearer <secret>
// or
//   X-API-Secret: <secret>
namespace ApiAccess
{
	enum class Level
	{
		PUBLIC = 0,
		PRIVATE = 1,
		ADMIN = 2
	};

	// Loads the secret from dataDir, generating it atomically on first start.
	// Called once when the node HTTP servers are created. Failure to load the
	// secret leaves the API in a fail-closed state for PRIVATE/ADMIN methods.
	void Initialize(const fs::path& dataDir);

	// True when a secret is loaded and PRIVATE/ADMIN calls can be authenticated.
	bool IsInitialized();

	// Secret file path (safe to log; contains no secret material).
	const fs::path& GetSecretPath();

	// Checks the request credentials for the given level.
	bool Check(mg_connection* conn, Level level, uint16_t serverPort);

	// --- Testable helpers (no mg_connection dependency) ---

	// Constant-time comparison of two hex tokens (case-insensitive).
	bool ValidateToken(const std::string& provided, const std::string& expected);

	// Host header must name the loopback interface; port, when present, must match.
	bool IsAllowedHost(const std::string& hostHeader, uint16_t serverPort);

	// Origin, when present, must be a loopback origin ("null" is allowed for
	// packaged desktop apps). Empty origin is allowed.
	bool IsAllowedOrigin(const std::string& origin, uint16_t serverPort);
}
