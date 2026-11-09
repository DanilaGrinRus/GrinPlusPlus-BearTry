#pragma once

#include <Core/Models/FullBlock.h>
#include <Core/Models/BlockSums.h>
#include <Database/BlockDb.h>
#include <Core/Traits/Lockable.h>
#include <Core/Config.h>
#include <PMMR/TxHashSet.h>
#include <memory>

class BlockValidator
{
public:
	static void VerifySelfConsistent(const FullBlock& block);

	// D2 network gate (Rust grin 5.5.2 parity): NRD kernels are valid only when
	// the configured network enables the NRD feature flag (mainnet disables it).
	// Public so the mainnet/testnet decision can be unit-tested directly without
	// switching the process-wide Global environment.
	static void VerifyNrdNetworkGate(const FullBlock& block, const Environment environment);

private:
	static void VerifyBody(const FullBlock& block);
	static void VerifyWeight(const FullBlock& block);
	static void VerifyKernelFeatures(const FullBlock& block);
	static void VerifyKernelLockHeights(const FullBlock& block);
	static void VerifyCoinbase(const FullBlock& block);
};