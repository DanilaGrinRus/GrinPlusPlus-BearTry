#pragma once

#include <Core/Traits/Jsonable.h>
#include <Core/Util/JsonUtil.h>
#include <Wallet/Models/Slate/Slate.h>
#include <string>

class SendResponse : public Traits::IJsonable
{
public:
    enum class EStatus
    {
        SENT,
        FINALIZED
    };

    SendResponse(
        const EStatus status,
        const Slate& slate,
        const std::string& armoredSlate,
        const std::string& error = "",
        const std::string& lifecycle = "",
        const std::string& broadcast = "",
        const bool finalized = false,
        const std::string& txSlateId = ""
    ) : m_status(status),
        m_slate(slate),
        m_armoredSlate(armoredSlate),
        m_error(error),
        m_lifecycle(lifecycle),
        m_broadcast(broadcast),
        m_finalized(finalized),
        m_txSlateId(txSlateId) { }

    EStatus GetStatus() const noexcept { return m_status; }
    const Slate& GetSlate() const noexcept { return m_slate; }
    const std::string& GetArmoredSlate() const noexcept { return m_armoredSlate; }
    const std::string& GetError() const noexcept { return m_error; }

    // Additive lifecycle information (see API/Wallet/Owner/Models/TxLifecycle.h).
    // The legacy "status" field keeps its old values for UI v1.2.8 compatibility.
    const std::string& GetLifecycle() const noexcept { return m_lifecycle; }
    const std::string& GetBroadcast() const noexcept { return m_broadcast; }
    bool IsFinalized() const noexcept { return m_finalized; }
    const std::string& GetTxSlateId() const noexcept { return m_txSlateId; }

    Json::Value ToJSON() const noexcept final
    {
        Json::Value result;
        result["status"] = m_status == EStatus::SENT ? "SENT" : "FINALIZED";
        result["slate"] = m_slate.ToJSON();
        result["slatepack"] = m_armoredSlate;

        if (!m_error.empty())
        {
            result["error"] = m_error;
        }

        if (!m_lifecycle.empty())
        {
            result["lifecycle"] = m_lifecycle;
        }

        if (!m_broadcast.empty())
        {
            result["broadcast"] = m_broadcast;
        }

        if (!m_txSlateId.empty())
        {
            result["tx_slate_id"] = m_txSlateId;
        }

        result["finalized"] = m_finalized;

        return result;
    }

    static SendResponse FromJSON(const Json::Value& json)
    {
        std::string statusStr = JsonUtil::GetRequiredString(json, "status");
        EStatus status = EStatus::SENT;
        if (statusStr == "SENT") {
            status = EStatus::SENT;
        } else if (statusStr == "FINALIZED") {
            status = EStatus::FINALIZED;
        } else {
            throw std::exception();
        }

        Slate slate = Slate::FromJSON(JsonUtil::GetRequiredField(json, "slate"));
        std::string armoredSlate = JsonUtil::GetRequiredString(json, "slatepack");
        std::string error = JsonUtil::GetStringOpt(json, "error").value_or("");
        std::string lifecycle = JsonUtil::GetStringOpt(json, "lifecycle").value_or("");
        std::string broadcast = JsonUtil::GetStringOpt(json, "broadcast").value_or("");
        std::string txSlateId = JsonUtil::GetStringOpt(json, "tx_slate_id").value_or("");

        bool finalized = false;
        if (json.isMember("finalized") && json["finalized"].isBool())
        {
            finalized = json["finalized"].asBool();
        }

        return SendResponse(status, slate, armoredSlate, error, lifecycle, broadcast, finalized, txSlateId);
    }

private:
    EStatus m_status;
    Slate m_slate;
    std::string m_armoredSlate;
    std::string m_error;
    std::string m_lifecycle;
    std::string m_broadcast;
    bool m_finalized;
    std::string m_txSlateId;
};
