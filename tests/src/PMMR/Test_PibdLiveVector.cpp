// Derived from grin 5.5.2 (Rust), Copyright The Grin Developers.
// Licensed under the Apache License, Version 2.0. Modified for the Grin++ C++ fork.
// See THIRD_PARTY_NOTICES.md and LICENSES/Apache-2.0.txt for details.
#include <catch.hpp>

#include <Common/Util/BitUtil.h>
#include <Common/Util/HexUtil.h>
#include <Common/Util/FileUtil.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Crypto/Models/Hash.h>
#include <PMMR/Common/MMRHashUtil.h>
#include <PMMR/Common/MMRUtil.h>
#include <PMMR/Models/BitmapSegment.h>
#include <PMMR/Models/Segment.h>

#include <filesystem.h>
#include <string>
#include <vector>

namespace
{
	Hash ParseHash(const std::string& hex)
	{
		return CBigInteger<32>::FromHex(hex);
	}

	uint64_t MrrSizeForLeaves(const uint64_t numLeaves)
	{
		return 2 * numLeaves - (uint64_t)BitUtil::CountBitsSet(numLeaves);
	}
}

// Offline regression vector captured from a live grin v5.5.2 peer (testnet):
// bitmap segment (height 9, idx 0) of the archive at height 3313440. The payload
// is a wire-format sample, not part of the repository; the test is skipped when
// the capture is absent (CI, fresh checkouts).
TEST_CASE("PibdLiveVector::BitmapSegmentReconstructs")
{
	const fs::path payloadPath = "fixtures/pibd/bitmap_payload_0.bin";
	if (!FileUtil::Exists(payloadPath)) {
		WARN("Captured live bitmap segment is not present; skipping");
		return;
	}

	std::vector<uint8_t> data;
	REQUIRE(FileUtil::ReadFile(payloadPath, data));

	ByteBuffer byteBuffer(data);
	const Hash blockHash = byteBuffer.ReadBigInteger<32>();
	const BitmapSegment segment = BitmapSegment::Deserialize(byteBuffer);
	const Hash outputRoot = byteBuffer.ReadBigInteger<32>();
	REQUIRE(byteBuffer.GetRemainingSize() == 0);

	REQUIRE(blockHash.GetData() ==
		ParseHash("84d7ac6763566ac67eaa8393688320c79a644321124d7ba94a1300f56af9ccea").GetData());
	REQUIRE(outputRoot.GetData() ==
		ParseHash("cb86106cdc190a37f10f644c1a84206d31e395440a54c9577f028e29ac16017e").GetData());
	REQUIRE(segment.GetIdentifier() == SegmentIdentifier(9, 0));

	const uint64_t outputLeaves = 3423687;
	const uint64_t outputMmrSize = MrrSizeForLeaves(outputLeaves);
	const uint64_t bitmapMmrSize = 6684;

	const Segment<BitmapChunk> pmmrSegment = segment.ToSegment();
	REQUIRE(pmmrSegment.GetLeafData().size() == 512);

	const std::pair<uint64_t, uint64_t> range = segment.GetIdentifier().SegmentPosRange(bitmapMmrSize);
	REQUIRE(range.first == 0);
	REQUIRE(range.second == 1022);

	const Hash segmentRoot = pmmrSegment.Root(bitmapMmrSize);
	const Hash reconstructed = pmmrSegment.GetProof().ReconstructRoot(
		bitmapMmrSize, range.first, range.second, segmentRoot, range.second + 1);
	const Hash combined = MMRHashUtil::HashParentWithIndex(outputRoot, reconstructed, outputMmrSize);
	const Hash expected = ParseHash("49fe85ec2ad085bfc3292d6993d574810c7011b7a11abc3d31e35bcbaf90c8e6");

	INFO(
		"segmentRoot=" << HexUtil::ConvertToHex(segmentRoot.GetData()) <<
		" reconstructed=" << HexUtil::ConvertToHex(reconstructed.GetData()) <<
		" combined=" << HexUtil::ConvertToHex(combined.GetData()));

	// Independent python reconstruction of the same payload gives:
	//   bitmap root a9591ab7d27629d3e26a79a43f345a7ced5c3dea069adefffb884a70fc73e37e
	//   combined   49fe85ec2ad085bfc3292d6993d574810c7011b7a11abc3d31e35bcbaf90c8e6
	REQUIRE(reconstructed.GetData() ==
		ParseHash("a9591ab7d27629d3e26a79a43f345a7ced5c3dea069adefffb884a70fc73e37e").GetData());
	REQUIRE(combined.GetData() == expected.GetData());
	REQUIRE_NOTHROW(pmmrSegment.ValidateWithOtherRoot(
		bitmapMmrSize, nullptr, expected, outputMmrSize, outputRoot, true));
}
