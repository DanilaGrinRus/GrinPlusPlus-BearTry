#pragma once

#include <Net/Servers/Server.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Util/HTTPUtil.h>
#include <Net/Servers/RPC/ApiAccess.h>
#include <Core/Exceptions/APIException.h>
#include <Core/Exceptions/ServiceUnavailableException.h>
#include <unordered_map>
#include <cassert>

#define RPC_LOG_INFO_F(logFile, message, ...) LoggerAPI::LogInfo(logFile, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__))
#define RPC_LOG_ERROR_F(logFile, message, ...) LoggerAPI::LogError(logFile, __func__, __LINE__, StringUtil::Format(message, __VA_ARGS__))

namespace RPC
{
	//
	// Error message for a request body that could not be parsed as JSON.
	//
	// This message reaches the wallet log and the RPC error reply, so it must
	// never include the body: request bodies contain session tokens and armored
	// slatepacks. Only the size is reported.
	//
	// Defined in src/Net/Util/HTTPUtil.cpp (the only producer of this error).
	//
	std::string FormatInvalidJsonBodyError(const std::string& requestBody);
}

class RPCServer
{
public:
	using Ptr = std::shared_ptr<RPCServer>;

	static std::shared_ptr<RPCServer> Create(
		const EServerType type,
		const std::optional<uint16_t>& port,
		const std::string& uri,
		const LoggerAPI::LogFile& logFile)
	{
		ServerPtr pServer = Server::Create(type, port);
		return RPCServer::Create(pServer, uri, logFile);
	}

	static RPCServer::Ptr Create(
		const ServerPtr& pServer,
		const std::string& uri,
		const LoggerAPI::LogFile& logFile)
	{
		auto pRPCServer = std::shared_ptr<RPCServer>(new RPCServer(pServer, logFile));

		pServer->AddListener(uri, APIHandler, pRPCServer.get());

		return pRPCServer;
	}

	uint16_t GetPortNumber() const noexcept { return m_pServer->GetPortNumber(); }

	void AddMethod(const std::string& method, const std::shared_ptr<RPCMethod>& pMethod) noexcept
	{
		m_methods[method] = pMethod;
	}

	const ServerPtr& GetServer() const { return m_pServer; }

	//
	// W-06/T1: reject browser requests whose Host/Origin is not loopback
	// (DNS rebinding, cross-origin pages). Intended for local wallet API servers
	// that do not use the node .api_secret.
	//
	void SetRequireLoopbackRequest(const bool require) noexcept { m_requireLoopbackRequest = require; }

private:
	RPCServer(const ServerPtr& pServer, const LoggerAPI::LogFile& logFile)
		: m_pServer(pServer), m_logFile(logFile) { }

	static int APIHandler(mg_connection* pConnection, void* pCbContext)
	{
		RPCServer* pInstance = static_cast<RPCServer*>(pCbContext);
		assert(pInstance != nullptr);

		// Reject an oversized declared body before any handler/parser can
		// allocate it. This is an HTTP-level error, not a JSON-RPC reply.
		if (!HTTPUtil::IsRequestBodySizeAllowed(HTTPUtil::GetRequestBodyLength(pConnection)))
		{
			return HTTPUtil::BuildRequestEntityTooLargeResponse(
				pConnection,
				"Request body too large");
		}

		Json::Value responseJSON = Handle(pConnection, *pInstance).ToJSON();

		return HTTPUtil::BuildSuccessResponseJSON(pConnection, responseJSON);
	}

	static RPC::Response Handle(mg_connection* pConnection, RPCServer& instance)
	{
		Json::Value id(Json::nullValue);
		try
		{
			RPC::Request request = RPC::Request::Parse(pConnection);
			id = request.GetId();
			try
			{
				if (instance.m_requireLoopbackRequest &&
				!ApiAccess::CheckLocalRequest(pConnection, instance.m_pServer->GetPortNumber()))
			{
				RPC_LOG_ERROR_F(instance.m_logFile, "Rejected non-loopback Host/Origin request");
				return request.BuildError(
					RPC::ErrorCode::INVALID_REQUEST,
					"Invalid Host/Origin"
				);
			}

			const std::string& method = request.GetMethod();
				auto iter = instance.m_methods.find(method);
				if (iter != instance.m_methods.end())
				{
					if (iter->second->GetAccessLevel() != ApiAccess::Level::PUBLIC &&
						!ApiAccess::Check(pConnection, iter->second->GetAccessLevel(), instance.m_pServer->GetPortNumber()))
					{
						RPC_LOG_ERROR_F(instance.m_logFile, "Unauthorized access to method: {}", method);
						return request.BuildError(
							RPC::ErrorCode::UNAUTHORIZED,
							"Unauthorized: valid API secret required"
						);
					}

					if (instance.m_logFile != LoggerAPI::LogFile::WALLET && !iter->second->ContainsSecrets())
					{
						RPC_LOG_INFO_F(instance.m_logFile, "REQUEST: {}", request.ToString());
					}
					else
					{
						RPC_LOG_INFO_F(
							instance.m_logFile,
							"Request made with method: {}",
							method
						);
					}
					
					auto response = iter->second->Handle(request);

					if (instance.m_logFile != LoggerAPI::LogFile::WALLET && !iter->second->ContainsSecrets())
					{
						RPC_LOG_INFO_F(instance.m_logFile, "REPLY: {}", response.ToString());
					}
					else if (response.GetError().has_value())
					{
						RPC_LOG_INFO_F(
							instance.m_logFile,
							"Method: {}, Error: {}",
							method,
							response.GetError().value().GetCode()
						);
					}

					return response;
				}
				else
				{
					RPC_LOG_ERROR_F(instance.m_logFile, "Method not supported: {}", method);
					return request.BuildError(
						RPC::ErrorCode::METHOD_NOT_FOUND,
						"Method not supported"
					);
				}
			}
			catch (const DeserializationException& e)
			{
				RPC_LOG_ERROR_F(instance.m_logFile, "Failed to deserialize request: {}", e.what());
				return request.BuildError(
					RPC::ErrorCode::INVALID_PARAMS,
					StringUtil::Format("Failed to deserialize request: {}", e.what())
				);
			}
		}
		catch (const RPCException& e)
		{
			RPC_LOG_ERROR_F(instance.m_logFile, "RPCException occurred: {}", e.what());
			return RPC::Response::BuildError(
				e.GetId().value_or(Json::nullValue),
				RPC::ErrorCode::INVALID_REQUEST,
				e.what()
			);
		}
		catch (const APIException& e)
		{
			RPC_LOG_ERROR_F(instance.m_logFile, "APIException occurred: {}", e.what());
			return RPC::Response::BuildError(
				id,
				e.GetErrorCode(),
				e.GetMsg()
			);
		}
		catch (const ServiceUnavailableException& e)
		{
			// Wallet node-client errors (NodeUnavailable/Rejected/NotFound) are
			// transient service failures, not malformed requests. Map them to the
			// existing INTERNAL_ERROR (-32603) code; the structured NODE_* code is
			// part of the final API decision package, not this stabilization stage.
			RPC_LOG_ERROR_F(instance.m_logFile, "Service unavailable: {}", e.what());
			return RPC::Response::BuildError(
				id,
				RPC::ErrorCode::INTERNAL_ERROR,
				e.what()
			);
		}
		catch (const Json::Exception& e)
		{
			// JsonCpp throws Json::LogicError from asString()/asBool() when the
			// parameter type is wrong (e.g. a string where a boolean is
			// required). That is an invalid parameter, not an invalid request.
			RPC_LOG_ERROR_F(instance.m_logFile, "JSON parameter error: {}", e.what());
			return RPC::Response::BuildError(
				id,
				RPC::ErrorCode::INVALID_PARAMS,
				e.what()
			);
		}
		catch (const std::exception& e)
		{
			RPC_LOG_ERROR_F(instance.m_logFile, "Exception occurred: {}", e.what());
			return RPC::Response::BuildError(
				id,
				RPC::ErrorCode::INVALID_REQUEST,
				e.what()
			);
		}
	}

	ServerPtr m_pServer;
	bool m_requireLoopbackRequest = false;
	std::unordered_map<std::string, std::shared_ptr<RPCMethod>> m_methods;
	LoggerAPI::LogFile m_logFile;
};

typedef std::shared_ptr<RPCServer> RPCServerPtr;