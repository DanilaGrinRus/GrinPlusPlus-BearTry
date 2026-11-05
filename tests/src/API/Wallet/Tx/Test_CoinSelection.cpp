#include <catch.hpp>

#include <Wallet/Exceptions/InsufficientFundsException.h>
#include <Wallet/SlateBuilder/CoinSelection.h>

#include <cstdint>
#include <limits>
#include <set>
#include <vector>

TEST_CASE("CoinSelection: ALL strategy rejects an amount the wallet cannot cover")
{
	// Regression (A-02): the old ALL check was `amount < amount + fee`, which is
	// always true, so an insufficient balance returned every coin instead of
	// failing. Selection must now fail closed.
	const std::vector<OutputDataEntity> noCoins;

	REQUIRE_THROWS_AS(
		CoinSelection::SelectCoinsToSpend(
			noCoins,
			(uint64_t)1'000'000,
			(uint64_t)1'000'000,
			ESelectionStrategy::ALL,
			std::set<Commitment>(),
			2,
			1
		),
		InsufficientFundsException
	);
}

TEST_CASE("CoinSelection: SMALLEST/FEWEST/CUSTOM reject insufficient balances")
{
	const std::vector<OutputDataEntity> noCoins;

	REQUIRE_THROWS_AS(
		CoinSelection::SelectCoinsToSpend(
			noCoins, (uint64_t)1, (uint64_t)1, ESelectionStrategy::SMALLEST, std::set<Commitment>(), 2, 1
		),
		InsufficientFundsException
	);
	REQUIRE_THROWS_AS(
		CoinSelection::SelectCoinsToSpend(
			noCoins, (uint64_t)1, (uint64_t)1, ESelectionStrategy::FEWEST_INPUTS, std::set<Commitment>(), 2, 1
		),
		InsufficientFundsException
	);
	REQUIRE_THROWS_AS(
		CoinSelection::SelectCoinsToSpend(
			noCoins, (uint64_t)1, (uint64_t)1, ESelectionStrategy::CUSTOM, std::set<Commitment>(), 2, 1
		),
		InsufficientFundsException
	);
}

TEST_CASE("CoinSelection: amount + fee overflow is treated as insufficient funds")
{
	const std::vector<OutputDataEntity> noCoins;
	const uint64_t maxAmount = (std::numeric_limits<uint64_t>::max)();

	REQUIRE_THROWS_AS(
		CoinSelection::SelectCoinsToSpend(
			noCoins, maxAmount, (uint64_t)1'000'000, ESelectionStrategy::ALL, std::set<Commitment>(), 2, 1
		),
		InsufficientFundsException
	);
	REQUIRE_THROWS_AS(
		CoinSelection::SelectCoinsToSpend(
			noCoins, maxAmount, (uint64_t)1'000'000, ESelectionStrategy::SMALLEST, std::set<Commitment>(), 2, 1
		),
		InsufficientFundsException
	);
}

TEST_CASE("CoinSelection: degenerate empty selection with zero amount and zero fee is a no-op")
{
	const std::vector<OutputDataEntity> noCoins;
	const std::vector<OutputDataEntity> selected = CoinSelection::SelectCoinsToSpend(
		noCoins, (uint64_t)0, (uint64_t)0, ESelectionStrategy::ALL, std::set<Commitment>(), 2, 1
	);
	REQUIRE(selected.empty());
}
