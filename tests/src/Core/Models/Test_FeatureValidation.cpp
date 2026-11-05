#include <catch.hpp>

#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>

#include <Core/Serialization/ByteBuffer.h>

#include <cstdint>
#include <vector>

TEST_CASE("FeatureValidation::RejectsUnknownOutputFeature")
{
	std::vector<uint8_t> payload{ 0x02 };
	ByteBuffer buffer(payload);
	REQUIRE_THROWS(OutputIdentifier::Deserialize(buffer));
}

TEST_CASE("FeatureValidation::RejectsUnknownKernelFeature")
{
	std::vector<uint8_t> payload{ 0x04 };
	ByteBuffer buffer(payload);
	REQUIRE_THROWS(TransactionKernel::Deserialize(buffer));
}

TEST_CASE("FeatureValidation::RejectsTruncatedKernel")
{
	std::vector<uint8_t> payload{};
	ByteBuffer buffer(payload);
	REQUIRE_THROWS(TransactionKernel::Deserialize(buffer));
}