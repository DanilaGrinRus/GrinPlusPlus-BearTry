#pragma once

#include "NodeContext.h"
#include <Core/Config.h>
#include <API/Node/NodeServer.h>
#include <Net/Servers/Server.h>
#include <Net/Servers/NodeStartupHealth.h>

class NodeRestServer
{
public:
	using UPtr = std::unique_ptr<NodeRestServer>;

	NodeRestServer(const std::shared_ptr<NodeContext>& pNodeContext, std::shared_ptr<NodeStartupHealth> pHealth, NodeServer::UPtr&& pNodeServer)
		: m_pNodeContext(pNodeContext), m_pHealth(std::move(pHealth)), m_pNodeServer(std::move(pNodeServer)) { }
	~NodeRestServer() = default;

	static ServerPtr StartInitializing(const Config& config, const std::shared_ptr<NodeStartupHealth>& pHealth);
	static NodeRestServer::UPtr Create(const Config& config, std::shared_ptr<NodeContext> pNodeContext, const ServerPtr& pServer, const std::shared_ptr<NodeStartupHealth>& pHealth);

private:
	std::shared_ptr<NodeContext> m_pNodeContext;
	std::shared_ptr<NodeStartupHealth> m_pHealth;
	NodeServer::UPtr m_pNodeServer;
};