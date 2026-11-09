#pragma once

#include <Net/IPAddress.h>
#include <Net/PeerAPIPath.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <P2P/P2PServer.h>

// C++ peers are keyed by IP; as with the REST endpoint, a supplied port is
// parsed but not part of peer identity. Do not claim per-port parity with Rust.
class PeerControlOwnerHandler : public RPCMethod
{
public:
	PeerControlOwnerHandler(IP2PServerPtr pServer, const bool ban)
		: m_pServer(std::move(pServer)), m_ban(ban) { }

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value() || !request.GetParams()->isArray() ||
			request.GetParams()->size() != 1 || !(*request.GetParams())[0].isString()) {
			return request.BuildError("INVALID_PARAMS", "Expected one peer address.");
		}
		std::optional<IPAddress> address;
		try {
			address = IPAddress::Parse(PeerAPIPath::Address((*request.GetParams())[0].asString()));
		} catch (const DeserializationException&) {
			return request.BuildError("INVALID_PARAMS", "Invalid peer address.");
		}
		if (m_ban) m_pServer->BanPeer(*address, EBanReason::ManualBan);
		else m_pServer->UnbanPeer(*address);
		Json::Value result;
		result["Ok"] = Json::nullValue;
		return request.BuildResult(result);
	}

	ApiAccess::Level GetAccessLevel() const noexcept final { return ApiAccess::Level::ADMIN; }
	bool ContainsSecrets() const noexcept final { return false; }

private:
	IP2PServerPtr m_pServer;
	bool m_ban;
};
