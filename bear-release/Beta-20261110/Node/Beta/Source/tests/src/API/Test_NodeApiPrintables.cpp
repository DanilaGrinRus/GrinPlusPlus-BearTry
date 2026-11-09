#include <catch.hpp>

#include <API/Node/Handlers/PrintableSerialization.h>
#include <Core/Models/Fee.h>
#include <Core/Models/Features.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/Signature.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
	Commitment MakeCommitment(const uint8_t seed)
	{
		return Commitment(CBigInteger<33>(std::vector<uint8_t>(33, seed)));
	}

	Signature MakeSignature(const uint8_t seed)
	{
		return Signature(CBigInteger<64>(std::vector<uint8_t>(64, seed)));
	}

	TransactionKernel MakeKernel(
		const EKernelFeatures features,
		const Fee& fee,
		const uint64_t lockHeight,
		const uint8_t seed)
	{
		return TransactionKernel(features, fee, lockHeight, MakeCommitment(seed), MakeSignature(seed));
	}
}

TEST_CASE("PrintableSerialization::ToRfc3339")
{
	REQUIRE(PrintableSerialization::ToRfc3339(0) == "1970-01-01T00:00:00+00:00");
	REQUIRE(PrintableSerialization::ToRfc3339(1700000000) == "2023-11-14T22:13:20+00:00");
}

TEST_CASE("PrintableSerialization::KernelPrintable")
{
	SECTION("Plain kernel")
	{
		const TransactionKernel kernel = MakeKernel(EKernelFeatures::DEFAULT_KERNEL, Fee(0, 0), 0, 1);
		const Json::Value json = PrintableSerialization::KernelPrintable(kernel);

		REQUIRE(json["features"].asString() == "Plain");
		REQUIRE(json["fee_shift"].asUInt() == 0);
		REQUIRE(json["fee"].asUInt64() == 0);
		REQUIRE(json["lock_height"].asUInt64() == 0);
		REQUIRE(json["excess"].asString() == MakeCommitment(1).ToHex());
		REQUIRE(json["excess_sig"].asString().size() == 128);
	}

	SECTION("Height locked kernel with a shifted fee")
	{
		const TransactionKernel kernel = MakeKernel(EKernelFeatures::HEIGHT_LOCKED, Fee(2, 150), 42, 2);
		const Json::Value json = PrintableSerialization::KernelPrintable(kernel);

		REQUIRE(json["features"].asString() == "HeightLocked");
		REQUIRE(json["fee_shift"].asUInt() == 2);
		REQUIRE(json["fee"].asUInt64() == 150);
		REQUIRE(json["lock_height"].asUInt64() == 42);
	}

	SECTION("Coinbase kernel")
	{
		const TransactionKernel kernel = MakeKernel(EKernelFeatures::COINBASE_KERNEL, Fee(0, 0), 0, 3);
		const Json::Value json = PrintableSerialization::KernelPrintable(kernel);

		REQUIRE(json["features"].asString() == "Coinbase");
		REQUIRE(json["fee"].asUInt64() == 0);
	}
}
