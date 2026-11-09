#include <catch.hpp>

#include "Test_NrdSupport.h"

using namespace NrdTest;

namespace
{
	std::vector<TransactionKernel> Kernels(std::initializer_list<TransactionKernel> kernels)
	{
		return std::vector<TransactionKernel>(kernels);
	}
}

// F11: a missing previous header during Rewind must fail closed with a typed
// exception, not dereference a null BlockHeaderPtr.
TEST_CASE("Rewind fails closed when the previous header is missing")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("rewind-missing-prev"));
	REQUIRE(builder.ApplyBlock(Kernels({})));

	const Hash previousHash = builder.Tip()->GetPreviousHash();
	builder.BlockDB()->RemoveBlockHeader(previousHash);

	REQUIRE_THROWS_AS(
		builder.TxHashSetRef().Rewind(builder.BlockDB(), *builder.GenesisHeader()),
		TxHashSetException);
}
