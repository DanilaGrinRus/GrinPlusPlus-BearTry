#pragma once

#include <API/Node/NodeStatus.h>

#include <BlockChain/BlockChain.h>
#include <P2P/P2PServer.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <json/json.h>

// Grin v5.5.2 owner API: get_status.
// The shared NodeStatus builder is the single source of the Rust Status schema.
class GetStatusOwnerHandler : public RPCMethod
{
public:
	GetStatusOwnerHandler(const IBlockChain::Ptr& pBlockChain, const IP2PServerPtr& pP2PServer)
		: m_pBlockChain(pBlockChain), m_pP2PServer(pP2PServer) { }
	~GetStatusOwnerHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		const Json::Value status = NodeStatus::Build(m_pBlockChain, m_pP2PServer);
		if (status.isNull()) {
			return request.BuildError("INTERNAL_ERROR", "Failed to find tip.");
		}

		Json::Value result;
		result["Ok"] = status;
		return request.BuildResult(result);
	}

	ApiAccess::Level GetAccessLevel() const noexcept final { return ApiAccess::Level::PRIVATE; }

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IBlockChain::Ptr m_pBlockChain;
	IP2PServerPtr m_pP2PServer;
};
