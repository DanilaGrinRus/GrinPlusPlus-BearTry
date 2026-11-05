#include <catch.hpp>

#include <API/Wallet/Owner/Models/SendResponse.h>

TEST_CASE("SendResponse: legacy status fields are unchanged and lifecycle fields are additive")
{
	const SendResponse response(
		SendResponse::EStatus::FINALIZED,
		Slate(),
		"BEGINSLATEPACK. test. ENDSLATEPACK.",
		"",
		"posted",
		"posted",
		true,
		"00000000-0000-0000-0000-000000000001"
	);

	const Json::Value json = response.ToJSON();

	// UI v1.2.8 contract: these two fields keep their old meaning.
	REQUIRE(json["status"].asString() == "FINALIZED");
	REQUIRE(json.isMember("slate"));
	REQUIRE(json["slatepack"].asString() == "BEGINSLATEPACK. test. ENDSLATEPACK.");

	// Additive lifecycle fields.
	REQUIRE(json["lifecycle"].asString() == "posted");
	REQUIRE(json["broadcast"].asString() == "posted");
	REQUIRE(json["finalized"].asBool());
	REQUIRE(json["tx_slate_id"].asString() == "00000000-0000-0000-0000-000000000001");

	const SendResponse parsed = SendResponse::FromJSON(json);
	REQUIRE(parsed.GetStatus() == SendResponse::EStatus::FINALIZED);
	REQUIRE(parsed.GetLifecycle() == "posted");
	REQUIRE(parsed.GetBroadcast() == "posted");
	REQUIRE(parsed.IsFinalized());
	REQUIRE(parsed.GetTxSlateId() == "00000000-0000-0000-0000-000000000001");
}

TEST_CASE("SendResponse: SENT keeps its legacy value for the offline slatepack flow")
{
	const SendResponse response(
		SendResponse::EStatus::SENT,
		Slate(),
		"BEGINSLATEPACK. test. ENDSLATEPACK.",
		"",
		"not_finalized",
		"not_requested"
	);

	const Json::Value json = response.ToJSON();
	REQUIRE(json["status"].asString() == "SENT");
	REQUIRE(json["lifecycle"].asString() == "not_finalized");
	REQUIRE(json["broadcast"].asString() == "not_requested");
	REQUIRE(!json["finalized"].asBool());
}

TEST_CASE("SendResponse: old JSON without lifecycle fields still parses")
{
	const SendResponse response(
		SendResponse::EStatus::SENT,
		Slate(),
		"BEGINSLATEPACK. test. ENDSLATEPACK."
	);
	Json::Value json = response.ToJSON();
	json.removeMember("lifecycle");
	json.removeMember("broadcast");
	json.removeMember("finalized");
	json.removeMember("tx_slate_id");

	const SendResponse parsed = SendResponse::FromJSON(json);
	REQUIRE(parsed.GetStatus() == SendResponse::EStatus::SENT);
	REQUIRE(parsed.GetLifecycle().empty());
	REQUIRE(parsed.GetBroadcast().empty());
	REQUIRE(!parsed.IsFinalized());
}
