#include <catch.hpp>

#include <Core/Exceptions/DeserializationException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Crypto/Models/Hash.h>
#include <P2P/Messages/MessageTypes.h>
#include <P2P/Messages/SegmentRequestMessage.h>
#include <P2P/Messages/SegmentResponseMessage.h>
#include <P2P/PIBD/PibdArchiveServer.h>
#include <PMMR/Models/SegmentIdentifier.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace
{
	std::vector<uint8_t> Bytes(const std::vector<uint8_t>& prefix, const size_t zeroCount)
	{
		std::vector<uint8_t> bytes = prefix;
		bytes.insert(bytes.end(), zeroCount, 0);
		return bytes;
	}

	std::shared_ptr<PibdArchiveServer::Snapshot> SnapshotFor(const Hash& hash, const uint64_t height)
	{
		auto pSnapshot = std::make_shared<PibdArchiveServer::Snapshot>();
		pSnapshot->key.hash = hash;
		pSnapshot->key.version = 3;
		pSnapshot->key.height = height;
		return pSnapshot;
	}
}

// B1 negative wire: malformed/truncated segment identifier must be rejected
// with a typed deserialization refusal, not accepted with zero/default values.
TEST_CASE("PibdWireNegative::TruncatedSegmentIdentifier")
{
	const std::vector<uint8_t> bytes = { 0x09, 0x00, 0x00, 0x00 }; // height + partial idx
	ByteBuffer buffer(bytes);
	REQUIRE_THROWS_AS(SegmentIdentifier::Deserialize(buffer), DeserializationException);
}

TEST_CASE("PibdWireNegative::IdentifierHeightOutOfRange")
{
	// MAX_SUPPORTED_HEIGHT is 32; 33 must be rejected before any shift/math.
	const std::vector<uint8_t> bytes = Bytes({ 33 }, 8);
	ByteBuffer buffer(bytes);
	REQUIRE_THROWS_AS(SegmentIdentifier::Deserialize(buffer), DeserializationException);
}

TEST_CASE("PibdWireNegative::IdentifierOffsetOverflow")
{
	// Height 9, idx UINT64_MAX is wire-valid but LeafOffset must overflow-fail.
	const SegmentIdentifier identifier(9, (std::numeric_limits<uint64_t>::max)());
	REQUIRE_THROWS_AS(identifier.LeafOffset(), DeserializationException);
}

// B1 negative wire: a non-segment message type must not be parsed as a
// segment response. The 32-byte block hash is present so failure is in the
// type dispatch, not just truncation.
TEST_CASE("PibdWireNegative::NonSegmentResponseType")
{
	std::vector<uint8_t> bytes(32, 0);
	ByteBuffer buffer(bytes);
	REQUIRE_THROWS_AS(
		SegmentResponseMessage::Deserialize(buffer, MessageTypes::Block),
		DeserializationException);
}

TEST_CASE("PibdWireNegative::TruncatedRequestHash")
{
	// Request body starts with a 32-byte block hash. Eight bytes cannot parse.
	std::vector<uint8_t> bytes(8, 0);
	ByteBuffer buffer(bytes);
	REQUIRE_THROWS_AS(
		SegmentRequestMessage::Deserialize(buffer, MessageTypes::GetOutputSegment),
		DeserializationException);
}

// Resource/concurrency: single-flight preparation and exact-hash acquisition.
TEST_CASE("PibdResource::SingleFlightAndExactHash")
{
	const Hash hashA = Hash::ValueOf(11);
	const Hash hashB = Hash::ValueOf(22);
	PibdArchiveServer::Registry registry(true);
	registry.SetExpected(hashA, 100);

	const auto now = std::chrono::steady_clock::now();
	REQUIRE(registry.CanPrepare(now));
	registry.BeginPreparation();
	REQUIRE_FALSE(registry.CanPrepare(now));

	REQUIRE(registry.Publish(SnapshotFor(hashA, 100)));
	REQUIRE(registry.ShouldAdvertise());
	REQUIRE(registry.Acquire(hashA) != nullptr);
	REQUIRE(registry.Acquire(hashB) == nullptr);
}

// Resource/concurrency: a horizon switch may retire the ready slot, but an
// already-held lease must stay valid; new exact-hash requests for the old
// horizon must be refused rather than serve stale data.
TEST_CASE("PibdResource::ConcurrentLeasesDuringHorizonSwitch")
{
	const Hash hashA = Hash::ValueOf(101);
	const Hash hashB = Hash::ValueOf(202);
	PibdArchiveServer::Registry registry(true);
	registry.SetExpected(hashA, 200);
	registry.BeginPreparation();
	REQUIRE(registry.Publish(SnapshotFor(hashA, 200)));

	const auto pOldLease = registry.Acquire(hashA);
	REQUIRE(pOldLease != nullptr);
	registry.SetExpected(hashB, 300);

	std::atomic<int> oldLeases{ 0 };
	std::atomic<int> newLeases{ 0 };
	std::vector<std::thread> readers;
	for (int i = 0; i < 8; ++i) {
		readers.emplace_back([&registry, hashA, hashB, &oldLeases, &newLeases]() {
			for (int j = 0; j < 50; ++j) {
				if (registry.Acquire(hashA) != nullptr) {
					++oldLeases;
				}
				if (registry.Acquire(hashB) != nullptr) {
					++newLeases;
				}
			}
		});
	}
	for (std::thread& reader : readers) {
		reader.join();
	}

	REQUIRE(pOldLease->key.hash == hashA);
	REQUIRE(oldLeases.load() == 0);
	REQUIRE(newLeases.load() == 0);
	REQUIRE(registry.Acquire(hashA) == nullptr);
	REQUIRE(registry.Acquire(hashB) == nullptr);
	REQUIRE_FALSE(registry.ShouldAdvertise());
}
