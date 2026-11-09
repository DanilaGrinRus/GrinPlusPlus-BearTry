#pragma once

#include <BlockChain/BlockChain.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <GrinVersion.h>
#include <optional>

class GetVersionHandler : public RPCMethod
{
public:
	GetVersionHandler(const IBlockChain::Ptr& pBlockChain)
		: m_pBlockChain(pBlockChain) { }
	~GetVersionHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		const auto pTip = m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTip == nullptr)
		{
			return request.BuildError("INTERNAL_ERROR", "Failed to find tip.");
		}

		Json::Value versionJson;
		versionJson["node_version"] = StringUtil::Format("Grin++ {}", GRINPP_VERSION);
		versionJson["block_header_version"] = pTip->GetVersion();

		Json::Value result;
		result["Ok"] = versionJson;
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IBlockChain::Ptr m_pBlockChain;
};