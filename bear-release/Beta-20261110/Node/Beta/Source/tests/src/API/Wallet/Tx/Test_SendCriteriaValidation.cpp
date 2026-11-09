#include <catch.hpp>

#include <Core/Exceptions/APIException.h>
#include <API/Wallet/Owner/Models/SendCriteria.h>

#include <vector>

namespace
{
	Json::Value BaseSendParams()
	{
		const SessionToken token(1234, std::vector<unsigned char>(36, 0x5a));

		Json::Value params(Json::objectValue);
		params["session_token"] = token.ToBase64();
		params["fee_base"] = Json::UInt64(1);
		params["selection_strategy"] = SelectionStrategyDTO(ESelectionStrategy::SMALLEST, {}).ToJSON();
		return params;
	}
}

TEST_CASE("SendCriteria: valid change_outputs and amount")
{
	Json::Value params = BaseSendParams();
	params["amount"] = Json::UInt64(1000);
	params["change_outputs"] = Json::UInt(1);

	const SendCriteria criteria = SendCriteria::FromJSON(params);
	REQUIRE(criteria.GetAmount().has_value());
	REQUIRE(criteria.GetAmount().value() == 1000);
	REQUIRE(criteria.GetNumOutputs() == 1);
}

TEST_CASE("SendCriteria: zero change_outputs is only allowed when sending the entire balance")
{
	Json::Value withAmount = BaseSendParams();
	withAmount["amount"] = Json::UInt64(1000);
	withAmount["change_outputs"] = Json::UInt(0);
	REQUIRE_THROWS_AS(SendCriteria::FromJSON(withAmount), APIException);

	Json::Value withoutAmount = BaseSendParams();
	withoutAmount["change_outputs"] = Json::UInt(0);
	const SendCriteria criteria = SendCriteria::FromJSON(withoutAmount);
	REQUIRE(!criteria.GetAmount().has_value());
	REQUIRE(criteria.GetNumOutputs() == 0);
}

TEST_CASE("SendCriteria: change_outputs must not silently truncate")
{
	Json::Value params = BaseSendParams();
	params["amount"] = Json::UInt64(1000);
	params["change_outputs"] = Json::UInt(256);
	REQUIRE_THROWS_AS(SendCriteria::FromJSON(params), APIException);

	Json::Value maxParams = BaseSendParams();
	maxParams["amount"] = Json::UInt64(1000);
	maxParams["change_outputs"] = Json::UInt(255);
	const SendCriteria maxCriteria = SendCriteria::FromJSON(maxParams);
	REQUIRE(maxCriteria.GetNumOutputs() == 255);
}

TEST_CASE("SendCriteria: zero explicit amount is rejected")
{
	Json::Value params = BaseSendParams();
	params["amount"] = Json::UInt64(0);
	params["change_outputs"] = Json::UInt(1);
	REQUIRE_THROWS_AS(SendCriteria::FromJSON(params), APIException);
}
