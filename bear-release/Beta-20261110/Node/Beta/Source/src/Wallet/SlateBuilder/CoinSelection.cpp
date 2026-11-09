#include "CoinSelection.h"

#include <Core/Util/FeeUtil.h>
#include <Wallet/Exceptions/InsufficientFundsException.h>
#include <Common/Logger.h>

#include <limits>

namespace
{
	//
	// Adds two uint64 amounts, failing closed on overflow. Selection compares
	// (amount + fee) against the selected total, so an overflowing sum must be
	// treated as "not enough funds" rather than wrapping around.
	//
	uint64_t AddChecked(const uint64_t lhs, const uint64_t rhs)
	{
		if (lhs > (std::numeric_limits<uint64_t>::max)() - rhs)
		{
			throw InsufficientFundsException();
		}

		return lhs + rhs;
	}
}

// If strategy is "ALL", spend all available coins to reduce the fee.
std::vector<OutputDataEntity> CoinSelection::SelectCoinsToSpend(
	const std::vector<OutputDataEntity>& availableCoins, 
	const uint64_t amount,
	const uint64_t feeBase,
	const ESelectionStrategy& strategy,
	const std::set<Commitment>& inputs,
	const int64_t numOutputs,
	const int64_t numKernels)
{
	if (strategy == ESelectionStrategy::CUSTOM)
	{
		return SelectUsingCustomInputs(availableCoins, amount, feeBase, inputs, numOutputs, numKernels);
	}
	else if (strategy == ESelectionStrategy::SMALLEST)
	{
		return SelectUsingSmallestInputs(availableCoins, amount, feeBase, numOutputs, numKernels);
	}
	else if (strategy == ESelectionStrategy::FEWEST_INPUTS)
	{
		return SelectUsingFewestInputs(availableCoins, amount, feeBase, numOutputs, numKernels);
	}
	else if (strategy == ESelectionStrategy::ALL)
	{
		return SelectUsingAllInputs(availableCoins, amount, feeBase, numOutputs, numKernels);
	}

	WALLET_ERROR("Unsupported selection strategy used.");
	throw InsufficientFundsException();
}

std::vector<OutputDataEntity> CoinSelection::SelectUsingSmallestInputs(
	const std::vector<OutputDataEntity>& availableCoins,
	const uint64_t amount,
	const uint64_t feeBase,
	const int64_t numOutputs,
	const int64_t numKernels)
{
	std::vector<OutputDataEntity> sortedCoins = availableCoins;
	std::sort(sortedCoins.begin(), sortedCoins.end());

	uint64_t amountFound = 0;
	std::vector<OutputDataEntity> selectedCoins;
	for (const OutputDataEntity& coin : sortedCoins)
	{
		amountFound = AddChecked(amountFound, coin.GetAmount());
		selectedCoins.push_back(coin);

		const uint64_t fee = FeeUtil::CalculateFee(feeBase, (int64_t)selectedCoins.size(), numOutputs, numKernels);
		const uint64_t required = AddChecked(amount, fee);
		if (amountFound >= required)
		{
			return selectedCoins;
		}
	}

	// Not enough coins found.
	WALLET_ERROR("Not enough funds.");
	throw InsufficientFundsException();
}

std::vector<OutputDataEntity> CoinSelection::SelectUsingFewestInputs(
	const std::vector<OutputDataEntity>& availableCoins,
	const uint64_t amount,
	const uint64_t feeBase,
	const int64_t numOutputs,
	const int64_t numKernels)
{
	std::vector<OutputDataEntity> sortedCoins = availableCoins;
	std::sort(sortedCoins.rbegin(), sortedCoins.rend());

	uint64_t amountFound = 0;
	std::vector<OutputDataEntity> selectedCoins;
	for (const OutputDataEntity& coin : sortedCoins) {
		amountFound = AddChecked(amountFound, coin.GetAmount());
		selectedCoins.push_back(coin);

		const uint64_t fee = FeeUtil::CalculateFee(feeBase, (int64_t)selectedCoins.size(), numOutputs, numKernels);
		const uint64_t required = AddChecked(amount, fee);
		if (amountFound >= required) {
			return selectedCoins;
		}
	}

	// Not enough coins found.
	WALLET_ERROR("Not enough funds.");
	throw InsufficientFundsException();
}

std::vector<OutputDataEntity> CoinSelection::SelectUsingAllInputs(
	const std::vector<OutputDataEntity>& availableCoins,
	const uint64_t amount,
	const uint64_t feeBase,
	const int64_t numOutputs,
	const int64_t numKernels)
{
	uint64_t amountFound = 0;
	for (const OutputDataEntity& coin : availableCoins)
	{
		amountFound = AddChecked(amountFound, coin.GetAmount());
	}

	const uint64_t fee = FeeUtil::CalculateFee(feeBase, (int64_t)availableCoins.size(), numOutputs, numKernels);
	const uint64_t required = AddChecked(amount, fee);
	if (amountFound >= required)
	{
		return availableCoins;
	}

	// Not enough coins found.
	WALLET_ERROR("Not enough funds.");
	throw InsufficientFundsException();
}

std::vector<OutputDataEntity> CoinSelection::SelectUsingCustomInputs(
	const std::vector<OutputDataEntity>& availableCoins,
	const uint64_t amount,
	const uint64_t feeBase,
	const std::set<Commitment>& inputs,
	const int64_t numOutputs,
	const int64_t numKernels)
{
	uint64_t amountFound = 0;
	std::vector<OutputDataEntity> selectedCoins;
	for (const OutputDataEntity& coin : availableCoins)
	{
		if (inputs.find(coin.GetOutput().GetCommitment()) != inputs.end())
		{
			amountFound = AddChecked(amountFound, coin.GetAmount());
			selectedCoins.push_back(coin);
		}
	}

	const uint64_t fee = FeeUtil::CalculateFee(feeBase, (int64_t)selectedCoins.size(), numOutputs, numKernels);
	const uint64_t required = AddChecked(amount, fee);
	if (amountFound >= required)
	{
		return selectedCoins;
	}

	// Not enough coins selected.
	WALLET_ERROR("Not enough funds.");
	throw InsufficientFundsException();
}