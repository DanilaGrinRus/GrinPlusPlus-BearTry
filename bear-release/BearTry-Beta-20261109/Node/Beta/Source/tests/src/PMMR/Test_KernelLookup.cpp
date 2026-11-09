#include <catch.hpp>

#include <Core/Global.h>
#include <Core/Models/Fee.h>
#include <Core/Models/Features.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/Signature.h>
#include <PMMR/KernelMMR.h>
#include <PMMR/TxHashSetImpl.h>

#include <filesystem.h>

namespace
{
	Commitment MakeCommitment(const uint8_t seed)
	{
		std::vector<uint8_t> bytes(33, seed);
		return Commitment(CBigInteger<33>(std::move(bytes)));
	}

	Signature MakeSignature(const uint8_t seed)
	{
		std::vector<uint8_t> bytes(64, seed);
		return Signature(CBigInteger<64>(std::move(bytes)));
	}

	TransactionKernel MakeKernel(const uint8_t seed)
	{
		return TransactionKernel(
			EKernelFeatures::DEFAULT_KERNEL, Fee(0, 0), 0, MakeCommitment(seed), MakeSignature(seed));
	}
}

TEST_CASE("TxHashSet::FindKernelByExcess uses 1-based PMMR positions")
{
	const fs::path dir = fs::temp_directory_path() / "kernel-lookup-positions";
	std::error_code error;
	fs::remove_all(dir, error);
	fs::create_directories(dir / "kernel");

	// The empty kernel MMR Load() adds the genesis kernel, then add three more.
	std::shared_ptr<KernelMMR> pKernelMMR = KernelMMR::Load(dir);
	REQUIRE(pKernelMMR->GetNumKernels() == 1);
	pKernelMMR->ApplyKernel(MakeKernel(1));
	pKernelMMR->ApplyKernel(MakeKernel(2));
	pKernelMMR->ApplyKernel(MakeKernel(3));

	TxHashSet txHashSet(pKernelMMR, nullptr, nullptr, nullptr);
	const Commitment excess3 = MakeCommitment(3);

	// Four leaves occupy positions 0,1,3,4. The fourth kernel is at PMMR
	// position 4, i.e. mmr_index 5. The old leaf+1 arithmetic returned 4.
	const auto found = txHashSet.FindKernelByExcess(excess3, std::nullopt, std::nullopt);
	REQUIRE(found.has_value());
	REQUIRE(found->second == 5);
	REQUIRE(found->first.GetExcessCommitment() == excess3);

	// maxIndex is inclusive 1-based position 4 -> leaf 3 (position 4) excluded.
	REQUIRE_FALSE(txHashSet.FindKernelByExcess(excess3, std::nullopt, 4).has_value());
	// minIndex=5 starts at the fourth kernel.
	const auto bounded = txHashSet.FindKernelByExcess(excess3, 5, 5);
	REQUIRE(bounded.has_value());
	REQUIRE(bounded->second == 5);

	fs::remove_all(dir, error);
}
