#include <catch.hpp>

#include <TestServer.h>

#include <BlockChain/BlockChain.h>
#include <Core/Models/SpentOutput.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Crypto/Models/Commitment.h>
#include <Database/BlockDb.h>
#include <P2P/ArchivePublication.h>
#include <PMMR/Common/LeafIndex.h>

TEST_CASE("SpentOutputs Release-mode deserialize consumes version byte")
{
	const Commitment commitment(CBigInteger<33>::ValueOf(7));
	const OutputLocation location(LeafIndex::At(3), 3317928);
	const SpentOutputs original(std::vector<SpentOutput>({ SpentOutput(commitment, location) }));

	Serializer serializer;
	original.Serialize(serializer);

	// This is the Release/NDEBUG path: the version byte must be consumed
	// explicitly, not by an assert that is compiled out.
	const std::vector<uint8_t> bytes = serializer.GetBytes();
	ByteBuffer byteBuffer(bytes);
	const SpentOutputs decoded = SpentOutputs::Deserialize(byteBuffer);
	REQUIRE(decoded.GetSpentOutputs().size() == 1);

	const std::unordered_map<Commitment, OutputLocation> map = decoded.BuildMap();
	REQUIRE(map.size() == 1);
	REQUIRE(map.count(commitment) == 1);
	REQUIRE(map.at(commitment).GetLeafIndex().GetPosition() == location.GetLeafIndex().GetPosition());
	REQUIRE(map.at(commitment).GetBlockHeight() == location.GetBlockHeight());

	// A non-zero version is rejected rather than silently decoded.
	std::vector<uint8_t> badBytes = bytes;
	badBytes[0] = 1;
	ByteBuffer badBuffer(badBytes);
	REQUIRE_THROWS_AS(SpentOutputs::Deserialize(badBuffer), DeserializationException);
}

TEST_CASE("BlockChain preflight requires every input in spent positions")
{
	TestServer::Ptr pServer = TestServer::Create();
	const Commitment mapped(CBigInteger<33>::ValueOf(11));
	const Commitment missing(CBigInteger<33>::ValueOf(12));
	const Hash blockHash = Hash::ValueOf(42);
	const OutputLocation location(LeafIndex::At(5), 100);

	{
		auto pWriter = pServer->GetBlockDB()->Write();
		pWriter->AddSpentPositions(
			blockHash,
			std::vector<SpentOutput>({ SpentOutput(mapped, location) }));
		pWriter->Commit();
	}

	size_t mappedCount = 0;
	REQUIRE(pServer->GetBlockChain()->AreSpentPositionsComplete(
		blockHash, std::vector<Commitment>({ mapped }), mappedCount));
	REQUIRE(mappedCount == 1);

	REQUIRE_FALSE(pServer->GetBlockChain()->AreSpentPositionsComplete(
		blockHash, std::vector<Commitment>({ mapped, missing }), mappedCount));
	REQUIRE(mappedCount == 1);
}

TEST_CASE("ArchivePublication preflight skips target body but fails rewound block gaps")
{
	using namespace ArchivePublication;
	const uint64_t targetHeight = 3313440;
	const uint64_t flushedHeight = 3318655;
	const std::string targetHash =
		"84d7ac6763566ac67eaa8393688320c79a644321124d7ba94a1300f56af9ccea";

	const auto baseView = [&](const uint64_t height) {
		RewindPreflightView view;
		view.headerPresent = true;
		view.headerHashHex = height == targetHeight
			? targetHash
			: ("hash-" + std::to_string(height));
		// The target body is deliberately absent; heights actually rewound have
		// bodies and complete spent records.
		view.bodyPresent = height != targetHeight;
		view.spentComplete = true;
		view.inputCount = 1;
		view.mappedCount = 1;
		return view;
	};

	std::string error;
	REQUIRE(ValidateRewindPreflight(
		targetHash, targetHeight, flushedHeight, baseView, &error));

	const auto missingTargetPlusOneBody = [&](const uint64_t height) {
		RewindPreflightView view = baseView(height);
		if (height == targetHeight + 1) {
			view.bodyPresent = false;
		}
		return view;
	};
	REQUIRE_FALSE(ValidateRewindPreflight(
		targetHash, targetHeight, flushedHeight, missingTargetPlusOneBody, &error));
	REQUIRE(error.find("missing block body at height 3313441") != std::string::npos);

	const auto missingTargetPlusOneSpent = [&](const uint64_t height) {
		RewindPreflightView view = baseView(height);
		if (height == targetHeight + 1) {
			view.spentComplete = false;
			view.mappedCount = 0;
		}
		return view;
	};
	REQUIRE_FALSE(ValidateRewindPreflight(
		targetHash, targetHeight, flushedHeight, missingTargetPlusOneSpent, &error));
	REQUIRE(error.find("has 1 input(s) but 0 mapped spent position(s)") != std::string::npos);

	const auto wrongTargetHeader = [&](const uint64_t height) {
		RewindPreflightView view = baseView(height);
		if (height == targetHeight) {
			view.headerHashHex = "00";
		}
		return view;
	};
	REQUIRE_FALSE(ValidateRewindPreflight(
		targetHash, targetHeight, flushedHeight, wrongTargetHeader, &error));
	REQUIRE(error.find("target header mismatch") != std::string::npos);

	// target == flushedTip is a no-op rewind: only the target header is needed.
	const auto noOpView = [](const uint64_t) {
		RewindPreflightView view;
		view.headerPresent = true;
		view.headerHashHex = "same";
		view.bodyPresent = false;
		view.spentComplete = false;
		return view;
	};
	error.clear();
	REQUIRE(ValidateRewindPreflight("same", 100, 100, noOpView, &error));
}
