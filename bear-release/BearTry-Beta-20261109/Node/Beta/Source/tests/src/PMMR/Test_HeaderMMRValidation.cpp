#include <catch.hpp>

#include <Core/Global.h>
#include <Core/Models/FullBlock.h>
#include <Core/Serialization/Serializer.h>
#include <PMMR/HeaderMMRImpl.h>
#include <PMMR/Common/MMRHashUtil.h>

#include <filesystem.h>
#include <fstream>

TEST_CASE("HeaderMMR::GetHashAt detects a corrupted hash file")
{
	const fs::path dir = fs::temp_directory_path() / "header-mmr-corruption";
	std::error_code error;
	fs::remove_all(dir, error);
	fs::create_directories(dir);
	const fs::path path = dir / "header_mmr.bin";

	std::shared_ptr<HeaderMMR> pHeaderMMR = HeaderMMR::Load(path);
	auto pLocked = std::make_shared<Locked<IHeaderMMR>>(pHeaderMMR);
	{
		auto pWriter = pLocked->Write();
		pWriter->AddHeader(*Global::GetGenesisBlock().GetHeader());
		pWriter->Commit();
	}

	Serializer serializer;
	Global::GetGenesisBlock().GetHeader()->GetProofOfWork().SerializeCycle(serializer);
	const Hash expected = MMRHashUtil::HashLeafWithIndex(serializer.vec(), 0);
	const std::unique_ptr<Hash> actual = pLocked->Read()->GetHashAt(Index::At(0));
	REQUIRE(actual != nullptr);
	REQUIRE(*actual == expected);

	{
		std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
		file.seekg(0);
		char byte = 0;
		file.read(&byte, 1);
		byte ^= 0x01;
		file.seekp(0);
		file.write(&byte, 1);
	}

	std::shared_ptr<HeaderMMR> pCorrupted = HeaderMMR::Load(path);
	const std::unique_ptr<Hash> corruptedHash = pCorrupted->GetHashAt(Index::At(0));
	REQUIRE(corruptedHash != nullptr);
	REQUIRE(*corruptedHash != expected);

	fs::remove_all(dir, error);
}
