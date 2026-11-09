#include <catch.hpp>

#include <P2P/Seed/InboundAdmission.h>

#include <chrono>

using namespace std::chrono_literals;

TEST_CASE("InboundAdmission caps pending handshakes and per-IP")
{
	P2P::InboundAdmission admission(4, 100, 2);
	const auto now = P2P::InboundAdmission::Clock::now();

	REQUIRE(admission.TryAcquire("10.0.0.1", now));
	REQUIRE(admission.TryAcquire("10.0.0.1", now));
	REQUIRE_FALSE(admission.TryAcquire("10.0.0.1", now));

	REQUIRE(admission.TryAcquire("10.0.0.2", now));
	REQUIRE(admission.TryAcquire("10.0.0.3", now));
	REQUIRE(admission.Pending() == 4);
	REQUIRE_FALSE(admission.TryAcquire("10.0.0.4", now));

	admission.Release("10.0.0.1");
	REQUIRE(admission.Pending() == 3);
	REQUIRE(admission.TryAcquire("10.0.0.4", now));
}

TEST_CASE("InboundAdmission accept-rate is a token bucket")
{
	P2P::InboundAdmission admission(100, 2, 100);
	const auto now = P2P::InboundAdmission::Clock::now();

	REQUIRE(admission.TryAcquire("a", now));
	REQUIRE(admission.TryAcquire("b", now));
	REQUIRE_FALSE(admission.TryAcquire("c", now));

	REQUIRE(admission.TryAcquire("c", now + 500ms));
	REQUIRE_FALSE(admission.TryAcquire("d", now + 500ms));

	REQUIRE(admission.TryAcquire("d", now + 1000ms));
}

TEST_CASE("InboundAdmission release is safe and idempotent at zero")
{
	P2P::InboundAdmission admission(4, 10, 4);
	admission.Release("never-acquired");
	REQUIRE(admission.Pending() == 0);
	REQUIRE(P2P::InboundAdmission::DEFAULT_MAX_PENDING == 32);
}
