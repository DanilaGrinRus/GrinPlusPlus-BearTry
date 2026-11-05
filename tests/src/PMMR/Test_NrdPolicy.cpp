#include "Test_NrdSupport.h"

#include <BlockChain/Validators/BlockValidator.h>

#include <Core/Config.h>
#include <Core/Enums/Environment.h>

#include <string>
#include <utility>
#include <vector>

using namespace NrdTest;

namespace
{
	FullBlock MakeBlockWithKernels(
		const uint16_t version,
		const uint64_t height,
		const uint64_t powTag,
		std::vector<TransactionKernel> kernels)
	{
		const BlockHeaderPtr pHeader = MakeHeader(
			version,
			height,
			MakeHeaderHash(powTag + 1000),
			kernels.size() + 1,
			powTag);

		return FullBlock(
			pHeader,
			TransactionBody(
				std::vector<TransactionInput>{},
				std::vector<TransactionOutput>{},
				std::move(kernels)));
	}

	std::string GateMessage(const FullBlock& block, const Environment environment)
	{
		try {
			BlockValidator::VerifyNrdNetworkGate(block, environment);
			return "NO-EXCEPTION";
		}
		catch (const std::exception& e) {
			return e.what();
		}
	}

	std::string SelfConsistentMessage(const FullBlock& block)
	{
		try {
			BlockValidator::VerifySelfConsistent(block);
			return "NO-EXCEPTION";
		}
		catch (const std::exception& e) {
			return e.what();
		}
	}
}

// D2: Rust grin 5.5.2 disables NRD kernels on mainnet and enables them on all
// other networks (src/bin/grin.rs init_global_nrd_enabled). Config::IsNrdKernelEnabled
// is the pure policy helper; BlockValidator::VerifyNrdNetworkGate wires it into
// the block consensus path.
TEST_CASE("NrdPolicy network gate matches Rust grin 5.5.2")
{
	REQUIRE_FALSE(Config::IsNrdKernelEnabled(Environment::MAINNET));
	REQUIRE(Config::IsNrdKernelEnabled(Environment::FLOONET));
	REQUIRE(Config::IsNrdKernelEnabled(Environment::AUTOMATED_TESTING));

	// Pure function: repeated calls and call order must not change the answer.
	REQUIRE(Config::IsNrdKernelEnabled(Environment::FLOONET));
	REQUIRE_FALSE(Config::IsNrdKernelEnabled(Environment::MAINNET));
}

TEST_CASE("NrdPolicy mainnet block gate rejects NRD kernels only")
{
	const FullBlock nrdBlock = MakeBlockWithKernels(4, 50, 201, { MakeNrdKernel(170, 2) });
	const FullBlock plainBlock = MakeBlockWithKernels(4, 51, 202, { MakePlainKernel(171) });

	// Rust parity: mainnet rejects NRD kernels (NRDKernelNotEnabled); all other
	// networks accept them at this gate.
	const std::string mainnetMessage = GateMessage(nrdBlock, Environment::MAINNET);
	INFO("mainnet message: " + mainnetMessage);
	REQUIRE(mainnetMessage.find("not enabled on this network") != std::string::npos);

	REQUIRE(GateMessage(nrdBlock, Environment::FLOONET) == "NO-EXCEPTION");
	REQUIRE(GateMessage(nrdBlock, Environment::AUTOMATED_TESTING) == "NO-EXCEPTION");

	// The gate is NRD-specific: plain kernels are unaffected on mainnet.
	REQUIRE(GateMessage(plainBlock, Environment::MAINNET) == "NO-EXCEPTION");
}

TEST_CASE("NrdPolicy network gate precedes the HF3 version gate")
{
	// Rust checks the feature flag before the header-version rule, so a mainnet
	// version-3 NRD block reports NRDKernelNotEnabled rather than
	// NRDKernelPreHF3; on an enabled network the HF3 version error applies.
	const FullBlock version3Nrd =
		MakeBlockWithKernels(3, 52, 203, { MakeNrdKernel(172, 2) });

	const std::string mainnetMessage = GateMessage(version3Nrd, Environment::MAINNET);
	INFO("mainnet v3 message: " + mainnetMessage);
	REQUIRE(mainnetMessage.find("not enabled on this network") != std::string::npos);

	// Through the wired VerifySelfConsistent path (process = AUTOMATED_TESTING,
	// NRD enabled) the same version-3 block reports the HF3 version error.
	const std::string versionMessage = SelfConsistentMessage(version3Nrd);
	INFO("v3 self-consistent message: " + versionMessage);
	REQUIRE(versionMessage.find("not valid before header version 4") != std::string::npos);
}

TEST_CASE("NrdPolicy VerifySelfConsistent uses the process network")
{
	// TestMain runs in AUTOMATED_TESTING, where NRD is enabled: the wired
	// VerifyKernelFeatures path must not raise the network-gate error. The block
	// still fails later body checks (it is not a real block), so only the absence
	// of the network-gate category is asserted.
	const FullBlock nrdBlock = MakeBlockWithKernels(4, 54, 205, { MakeNrdKernel(174, 2) });
	const std::string message = SelfConsistentMessage(nrdBlock);
	INFO("self-consistent message: " + message);
	REQUIRE(message.find("not enabled on this network") == std::string::npos);
}
