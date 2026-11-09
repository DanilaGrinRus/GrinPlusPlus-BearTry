#pragma once

#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/ApiAccess.h>

class RPCMethod
{
public:
	virtual ~RPCMethod() = default;

	virtual RPC::Response Handle(const RPC::Request& request) const = 0;

	// Access level enforced centrally by RPCServer before Handle() is called.
	virtual ApiAccess::Level GetAccessLevel() const noexcept { return ApiAccess::Level::PUBLIC; }

	//
	// If true, requests and replies will not be logged.
	// NOTE: Errors could still be logged, so never store secrets in exceptions or RPC errors.
	//
	virtual bool ContainsSecrets() const noexcept = 0;
};