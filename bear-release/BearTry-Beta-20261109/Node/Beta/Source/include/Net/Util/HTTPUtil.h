#pragma once

#include <Net/Clients/HTTP/HTTP.h>
#include <json/json.h>
#include <cassert>
#include <string>
#include <optional>

// Forward Declarations
struct mg_connection;

class HTTPUtil
{
public:
	// Strips away the base URI and any query strings.
	// Ex: Given "/v1/blocks/<hash>?compact" and baseURI "/v1/blocks/", this would return <hash>.
	static std::string GetURIParam(mg_connection* conn, const std::string& baseURI);
	static std::string GetQueryString(mg_connection* conn) noexcept;
	static bool HasQueryParam(mg_connection* conn, const std::string& parameterName);
	static std::optional<std::string> GetQueryParam(mg_connection* conn, const std::string& parameterName);
	static std::optional<std::string> GetHeaderValue(mg_connection* conn, const std::string& headerName);
	static HTTP::EHTTPMethod GetHTTPMethod(mg_connection* conn);
	static std::optional<Json::Value> GetRequestBody(mg_connection* conn);

	// Upper bound for a single JSON-RPC request body. The node API is local,
	// but Content-Length is attacker-controlled and a declared 256 MiB used to
	// allocate 256 MiB before any bytes were read. 8 MiB is larger than any
	// node/wallet request (the JSON-encoded max block transaction is < 2 MiB)
	// and keeps a malicious body from driving unbounded allocation.
	static constexpr long long MAX_REQUEST_BODY_BYTES = 8LL * 1024 * 1024;

	static bool IsRequestBodySizeAllowed(const long long contentLength) noexcept
	{
		return contentLength >= 0 && contentLength <= MAX_REQUEST_BODY_BYTES;
	}

	// Returns the declared Content-Length, or -1 for chunked/unknown bodies.
	static long long GetRequestBodyLength(mg_connection* conn) noexcept;

	static int BuildSuccessResponseJSON(mg_connection* conn, const Json::Value& json);
	static int BuildSuccessResponse(mg_connection* conn, const std::string& response);
	static int BuildBadRequestResponse(mg_connection* conn, const std::string& response);
	static int BuildConflictResponse(mg_connection* conn, const std::string& response);
	static int BuildUnauthorizedResponse(mg_connection* conn, const std::string& response);
	static int BuildNotFoundResponse(mg_connection* conn, const std::string& response);
	static int BuildRequestEntityTooLargeResponse(mg_connection* conn, const std::string& response);
	static int BuildInternalErrorResponse(mg_connection* conn, const std::string& response);
};