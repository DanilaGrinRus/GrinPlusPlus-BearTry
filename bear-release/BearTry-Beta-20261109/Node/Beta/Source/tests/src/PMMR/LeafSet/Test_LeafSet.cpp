#include <catch.hpp>

#include <PMMR/Common/LeafSet.h>
#include <Core/File/BitmapFile.h>
#include <filesystem.h>

TEST_CASE("LeafSet")
{
    // BitmapFile::Set(Roaring) consumes 1-based MMR positions and converts them
    // to 0-based leaf indices. PibdStateWriter therefore writes pos0 + 1.
    const fs::path path = fs::temp_directory_path() / "leafset-roaring-units.bin";
    std::error_code error;
    fs::remove(path, error);

    Roaring positions;
    positions.add(1);  // MMR position 0 -> leaf 0
    positions.add(2);  // MMR position 1 -> leaf 1
    positions.add(65); // MMR position 64 -> leaf 33
    positions.add(121); // MMR position 120 -> leaf 63
    BitmapFile::Create(path, positions);

    const std::shared_ptr<LeafSet> pLeafSet = LeafSet::Load(path);
    CHECK(pLeafSet->Contains(LeafIndex::At(0)));
    CHECK(pLeafSet->Contains(LeafIndex::At(1)));
    CHECK(pLeafSet->Contains(LeafIndex::At(33)));
    CHECK(pLeafSet->Contains(LeafIndex::At(63)));
    CHECK_FALSE(pLeafSet->Contains(LeafIndex::At(2)));
    fs::remove(path, error);
}