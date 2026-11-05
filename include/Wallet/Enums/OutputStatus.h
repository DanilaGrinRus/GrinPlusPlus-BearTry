#pragma once

#include <Core/Exceptions/DeserializationException.h>
#include <string>

enum EOutputStatus
{
	SPENDABLE = 0,
	IMMATURE = 1, // DEPRECATED: Outputs should be marked as spendable.
	NO_CONFIRMATIONS = 2,
	SPENT = 3,
	LOCKED = 4,
	CANCELED = 5,
	// The output was seen on chain but its confirming block was reorged out and
	// the output is no longer in the canonical UTXO set. Unlike SPENT, this is
	// not a final state: the output may reappear (and become SPENDABLE again).
	REVERTED = 6
};

namespace OutputStatus
{
	static EOutputStatus FromString(const std::string& value)
	{
		if (value == "Spendable")
		{
			return EOutputStatus::SPENDABLE;
		}
		else if (value == "Immature")
		{
			return EOutputStatus::IMMATURE;
		}
		else if (value == "No Confirmations")
		{
			return EOutputStatus::NO_CONFIRMATIONS;
		}
		else if (value == "Spent")
		{
			return EOutputStatus::SPENT;
		}
		else if (value == "Locked")
		{
			return EOutputStatus::LOCKED;
		}
		else if (value == "Canceled")
		{
			return EOutputStatus::CANCELED;
		}
		else if (value == "Reverted")
		{
			return EOutputStatus::REVERTED;
		}

		throw DESERIALIZATION_EXCEPTION_F("Invalid output status: {}", value);
	}

	static std::string ToString(const EOutputStatus status)
	{
		switch (status)
		{
			case SPENDABLE:
			{
				return "Spendable";
			}
			case IMMATURE:
			{
				return "Immature";
			}
			case NO_CONFIRMATIONS:
			{
				return "No Confirmations";
			}
			case SPENT:
			{
				return "Spent";
			}
			case LOCKED:
			{
				return "Locked";
			}
			case CANCELED:
			{
				return "Canceled";
			}
			case REVERTED:
			{
				return "Reverted";
			}
		}

		return "";
	}
}