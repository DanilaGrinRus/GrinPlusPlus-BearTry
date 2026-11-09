#pragma once

#include <Core/Util/JsonUtil.h>
#include <optional>

struct SlateFeatureArgs
{
    std::optional<uint64_t> lockHeightOpt;

    Json::Value ToJSON() const noexcept
    {
        Json::Value json;

        if (lockHeightOpt.has_value()) {
            json["lock_hgt"] = lockHeightOpt.value();
        }

        return json;
    }

    bool operator==(const SlateFeatureArgs& rhs) const noexcept { return lockHeightOpt == rhs.lockHeightOpt; }

    static SlateFeatureArgs FromJSON(const Json::Value& json)
    {
        SlateFeatureArgs args;
        args.lockHeightOpt = JsonUtil::GetUInt64Opt(json, "lock_hgt");
        return args;
    }
};