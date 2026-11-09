#include <catch.hpp>

#include "Test_NrdSupport.h"

#include <Core/Enums/Environment.h>
#include <TxPool/TransactionPoolImpl.h>

using NrdTest::MakeNrdKernel;
using NrdTest::MakePlainKernel;

// D2: Rust grin 5.5.2 pool verify_kernel_variants order and results.
TEST_CASE("TxPool NRD policy matches Rust verify_kernel_variants")
{
	using R = TransactionPool::ENrdPolicyResult;
	const std::vector<TransactionKernel> nrd = { MakeNrdKernel(190, 2) };
	const std::vector<TransactionKernel> plain = { MakePlainKernel(191) };

	REQUIRE(TransactionPool::CheckNrdPolicy(nrd, 4, Environment::MAINNET) == R::NRD_NOT_ENABLED);
	REQUIRE(TransactionPool::CheckNrdPolicy(nrd, 1, Environment::MAINNET) == R::NRD_NOT_ENABLED);
	REQUIRE(TransactionPool::CheckNrdPolicy(nrd, 3, Environment::FLOONET) == R::NRD_PRE_HF3);
	REQUIRE(TransactionPool::CheckNrdPolicy(nrd, 4, Environment::FLOONET) == R::OK);
	REQUIRE(TransactionPool::CheckNrdPolicy(nrd, 4, Environment::AUTOMATED_TESTING) == R::OK);
	REQUIRE(TransactionPool::CheckNrdPolicy(plain, 1, Environment::MAINNET) == R::OK);
	REQUIRE(TransactionPool::CheckNrdPolicy({}, 1, Environment::MAINNET) == R::OK);

	std::vector<TransactionKernel> mixed = { MakePlainKernel(192), MakeNrdKernel(193, 2) };
	REQUIRE(TransactionPool::CheckNrdPolicy(mixed, 4, Environment::MAINNET) == R::NRD_NOT_ENABLED);
}
