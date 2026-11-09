#include <catch.hpp>

#include <API/Node/Handlers/GetConnectedPeersOwnerHandler.h>
#include <API/Node/Handlers/GetPeersOwnerHandler.h>
#include <API/Node/Handlers/GetStatusOwnerHandler.h>
#include <API/Node/Handlers/GetConfigHandler.h>
#include <API/Node/Handlers/UpdateConfigHandler.h>

#include <Net/Servers/RPC/ApiAccess.h>
#include <Net/Servers/RPC/RPCMethod.h>

namespace
{
	class PublicProbeMethod final : public RPCMethod
	{
	public:
		RPC::Response Handle(const RPC::Request& request) const final
		{
			Json::Value result;
			result["Ok"] = "public_probe";
			return request.BuildResult(result);
		}
		bool ContainsSecrets() const noexcept final { return false; }
	};
}

// Security regression: owner read methods used to inherit the RPCMethod PUBLIC
// default and returned data without Authorization. The central RPCServer gate
// rejects PRIVATE/ADMIN methods with RPC::ErrorCode::UNAUTHORIZED.
TEST_CASE("Owner handlers require PRIVATE access; config levels unchanged")
{
	const GetStatusOwnerHandler status{ IBlockChain::Ptr(), IP2PServerPtr() };
	const GetPeersOwnerHandler peers{ IP2PServerPtr() };
	const GetConnectedPeersOwnerHandler connected{ IP2PServerPtr() };

	REQUIRE(status.GetAccessLevel() == ApiAccess::Level::PRIVATE);
	REQUIRE(peers.GetAccessLevel() == ApiAccess::Level::PRIVATE);
	REQUIRE(connected.GetAccessLevel() == ApiAccess::Level::PRIVATE);

	const GetConfigHandler config;
	const UpdateConfigHandler updateConfig;
	REQUIRE(config.GetAccessLevel() == ApiAccess::Level::PRIVATE);
	REQUIRE(updateConfig.GetAccessLevel() == ApiAccess::Level::ADMIN);

	const PublicProbeMethod publicProbe;
	REQUIRE(publicProbe.GetAccessLevel() == ApiAccess::Level::PUBLIC);
}
