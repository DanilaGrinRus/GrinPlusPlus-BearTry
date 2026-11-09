#include <catch.hpp>

#include <Core/Exceptions/APIException.h>
#include <Wallet/SlateBuilder/ReceiveSlateBuilder.h>

TEST_CASE("ReceiveSlateBuilder: accepts a standard send slate within its TTL")
{
	Slate slate;
	slate.amount = 1'000'000;
	slate.ttl = 0;
	REQUIRE_NOTHROW(ReceiveSlateBuilder::ValidateIncomingSlate(slate, 1000));

	slate.ttl = 2000;
	REQUIRE_NOTHROW(ReceiveSlateBuilder::ValidateIncomingSlate(slate, 1000));
}

TEST_CASE("ReceiveSlateBuilder: rejects non-S1 slates")
{
	Slate slate;
	slate.amount = 1'000'000;
	slate.stage = ESlateStage::STANDARD_RECEIVED;
	REQUIRE_THROWS_AS(ReceiveSlateBuilder::ValidateIncomingSlate(slate, 1000), APIException);

	slate.stage = ESlateStage::STANDARD_FINALIZED;
	REQUIRE_THROWS_AS(ReceiveSlateBuilder::ValidateIncomingSlate(slate, 1000), APIException);
}

TEST_CASE("ReceiveSlateBuilder: rejects zero-amount slates")
{
	Slate slate;
	slate.amount = 0;
	REQUIRE_THROWS_AS(ReceiveSlateBuilder::ValidateIncomingSlate(slate, 1000), APIException);
}

TEST_CASE("ReceiveSlateBuilder: rejects slates whose TTL already expired")
{
	Slate slate;
	slate.amount = 1'000'000;
	slate.ttl = 1000;

	// Exclusive cutoff: at/after the TTL height the slate is no longer valid.
	REQUIRE_THROWS_AS(ReceiveSlateBuilder::ValidateIncomingSlate(slate, 1000), APIException);
	REQUIRE_THROWS_AS(ReceiveSlateBuilder::ValidateIncomingSlate(slate, 1001), APIException);
	REQUIRE_NOTHROW(ReceiveSlateBuilder::ValidateIncomingSlate(slate, 999));
}
