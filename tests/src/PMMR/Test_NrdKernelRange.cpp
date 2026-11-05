#include "Test_NrdSupport.h"

#include <optional>
#include <utility>
#include <vector>

using namespace NrdTest;

namespace
{
	std::vector<TransactionKernel> Kernels(std::initializer_list<TransactionKernel> kernels)
	{
		return std::vector<TransactionKernel>(kernels);
	}
}

// Duplicate-excess selection semantics used by get_kernel: the direct scan must
// return the LATEST matching kernel within [min_index, max_index] (Rust
// find_kernel scans backwards). This exercises the real TxHashSet/KernelMMR with
// two occurrences of the same excess at different heights.
TEST_CASE("TxHashSet duplicate-excess selection preserves latest-in-range")
{
	ChainBuilder builder(ChainBuilder::MakeTempRoot("kernel-range"));

	// h1: first occurrence; h2: empty; h3: same excess again (NRD rel=1 allows a
	// two-block gap, so the real ApplyBlock accepts both).
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(190, 1) }))); // h1, leaf 1
	REQUIRE(builder.ApplyBlock(Kernels({})));                        // h2, no kernel
	const BlockHeaderPtr pHeader2 = builder.Tip();
	REQUIRE(builder.ApplyBlock(Kernels({ MakeNrdKernel(190, 1) }))); // h3, leaf 2

	const Commitment excess = MakeExcess(190);
	// The ChainBuilder KernelMMR::Load seeds the genesis kernel as leaf 0, so the
	// h1 kernel is leaf 1 and the h3 kernel is leaf 2. FindKernelByExcess returns
	// the 1-based PMMR node position (leaf_position + 1), not the header
	// kernel_mmr_size (node count).
	const uint64_t firstIndex = LeafIndex::At(1).GetPosition() + 1;   // 2
	const uint64_t h2MaxIndex = pHeader2->GetKernelMMRSize();          // 3
	const uint64_t latestIndex = LeafIndex::At(2).GetPosition() + 1;   // 4

	// No range: latest occurrence.
	{
		const auto found = builder.TxHashSetRef().FindKernelByExcess(excess, std::nullopt, std::nullopt);
		REQUIRE(found.has_value());
		REQUIRE(found->second == latestIndex);
	}

	// Range ending at h2: the earlier occurrence is the latest one in range.
	{
		const auto found = builder.TxHashSetRef().FindKernelByExcess(excess, std::nullopt, h2MaxIndex);
		REQUIRE(found.has_value());
		REQUIRE(found->second == firstIndex);
	}

	// Range starting at h3 (min_index = h2.kernel_mmr_size + 1): latest occurrence.
	{
		const auto found = builder.TxHashSetRef().FindKernelByExcess(excess, h2MaxIndex + 1, std::nullopt);
		REQUIRE(found.has_value());
		REQUIRE(found->second == latestIndex);
	}

	// Empty range (min > max): no kernel.
	{
		const auto found = builder.TxHashSetRef().FindKernelByExcess(excess, latestIndex, h2MaxIndex);
		REQUIRE_FALSE(found.has_value());
	}
}
