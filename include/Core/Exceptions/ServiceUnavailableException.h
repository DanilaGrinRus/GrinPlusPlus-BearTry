#pragma once

#include <stdexcept>
#include <string>

//
// Common exception for a dependent service that is temporarily unavailable
// (for example the local/remote node behind the wallet API).
//
// The RPC boundary maps it to an existing JSON-RPC error code (INTERNAL_ERROR,
// -32603) instead of the generic INVALID_REQUEST, so wallet clients can
// distinguish "node is not ready, retry" from "bad request".
//
class ServiceUnavailableException : public std::runtime_error
{
public:
	explicit ServiceUnavailableException(const std::string& message)
		: std::runtime_error(message) { }
};
