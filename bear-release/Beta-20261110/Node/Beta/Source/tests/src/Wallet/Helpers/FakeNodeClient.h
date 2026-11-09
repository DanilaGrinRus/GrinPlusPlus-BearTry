#pragma once

#include <Wallet/NodeClient.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

//
// Programmable INodeClient for wallet scan/refresh tests.
//
class FakeNodeClient : public INodeClient
{
public:
	NodeTip tip;
	bool tipUnavailable = false;
	std::function<NodeTip()> tipProvider;

	std::map<Commitment, OutputLocation> unspentOutputs;
	std::map<Commitment, KernelLookup> kernels;
	std::optional<uint64_t> mmrSizeAtHeight;

	// Pages returned by consecutive GetOutputsByPosition calls. When exhausted an
	// empty range with highest/last == 0 is returned.
	std::vector<OutputRange> pages;
	bool returnNullPage = false;

	mutable uint64_t outputScanCalls = 0;
	mutable uint64_t lastScanStart = 0;

	uint64_t GetChainHeight() const final { return tip.height; }

	BlockHeaderPtr GetTipHeader() const final { return nullptr; }
	BlockHeaderPtr GetBlockHeader(const uint64_t) const final { return nullptr; }

	std::map<Commitment, OutputLocation> GetOutputsByCommitment(const std::vector<Commitment>& commitments) const final
	{
		std::map<Commitment, OutputLocation> result;
		for (const Commitment& commitment : commitments)
		{
			const auto iter = unspentOutputs.find(commitment);
			if (iter != unspentOutputs.cend()) {
				result.insert(*iter);
			}
		}

		return result;
	}

	std::vector<BlockWithOutputs> GetBlockOutputs(const uint64_t, const uint64_t) const final
	{
		return {};
	}

	std::unique_ptr<OutputRange> GetOutputsByLeafIndex(const uint64_t, const uint64_t) const final
	{
		throw NodeUnavailableException("Legacy leaf-index scan is not supported by FakeNodeClient.");
	}

	std::unique_ptr<OutputRange> GetOutputsByPosition(const uint64_t startPosition, const uint64_t) const final
	{
		++outputScanCalls;
		lastScanStart = startPosition;

		if (returnNullPage) {
			return nullptr;
		}

		if (m_nextPage < pages.size()) {
			return std::make_unique<OutputRange>(pages[m_nextPage++]);
		}

		return std::make_unique<OutputRange>(0, 0, std::vector<OutputDTO>{});
	}

	std::optional<NodeTip> GetTip() const final
	{
		if (tipUnavailable) {
			return std::nullopt;
		}

		if (tipProvider) {
			return tipProvider();
		}

		return tip;
	}

	KernelLookup GetKernel(const Commitment& excess) const final
	{
		const auto iter = kernels.find(excess);
		if (iter != kernels.cend()) {
			return iter->second;
		}

		return KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
	}

	std::optional<uint64_t> GetOutputMMRSizeAtHeight(const uint64_t) const final
	{
		return mmrSizeAtHeight;
	}

	bool PostTransaction(TransactionPtr, const EPoolType) final
	{
		return false;
	}

private:
	mutable size_t m_nextPage = 0;
};
