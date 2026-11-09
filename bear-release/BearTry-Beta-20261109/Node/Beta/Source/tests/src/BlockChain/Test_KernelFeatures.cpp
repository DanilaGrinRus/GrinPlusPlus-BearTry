#include <catch.hpp>

#include <Core/Models/Features.h>
#include <Core/Models/TransactionKernel.h>

TEST_CASE("TransactionKernel coinbase classification is exact")
{
	const Commitment zeroCommitment(CBigInteger<33>::ValueOf(0));
	const Signature zeroSignature(CBigInteger<64>::ValueOf(0));

	const TransactionKernel coinbase(
		EKernelFeatures::COINBASE_KERNEL, Fee(), 0, zeroCommitment, zeroSignature);
	const TransactionKernel nrd(
		EKernelFeatures::NO_RECENT_DUPLICATE, Fee(), 0, zeroCommitment, zeroSignature);
	const TransactionKernel plain(
		EKernelFeatures::DEFAULT_KERNEL, Fee(), 0, zeroCommitment, zeroSignature);

	REQUIRE(coinbase.IsCoinbase());
	REQUIRE_FALSE(nrd.IsCoinbase());
	REQUIRE_FALSE(plain.IsCoinbase());
}
