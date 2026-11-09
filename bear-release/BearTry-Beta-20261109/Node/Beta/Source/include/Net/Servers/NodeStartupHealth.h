#pragma once

#include <Net/Util/HTTPUtil.h>
#include <atomic>

// Independent of the chain: safe to serve while the node is opening its DB.
struct NodeStartupHealth
{
    std::atomic_bool ready{ false };

    static int Handle(struct mg_connection* conn, void* pCallbackData)
    {
        if (HTTPUtil::GetHTTPMethod(conn) != HTTP::EHTTPMethod::GET) {
            return HTTPUtil::BuildNotFoundResponse(conn, "Not Found");
        }
        const auto* pHealth = static_cast<NodeStartupHealth*>(pCallbackData);
        Json::Value result;
        result["status"] = pHealth->ready.load() ? "ready" : "starting";
        return HTTPUtil::BuildSuccessResponse(conn, result.toStyledString());
    }
};
