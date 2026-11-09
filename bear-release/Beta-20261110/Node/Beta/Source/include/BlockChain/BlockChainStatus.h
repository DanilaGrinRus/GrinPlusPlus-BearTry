#pragma once

enum class EBlockChainStatus
{
	SUCCESS,
	UNKNOWN_ERROR,
	// Local rewind data (body or spent-position entry) is missing. This is not
	// evidence that the peer's block is invalid.
	LOCAL_REWIND_DATA_MISSING,
	STORE_ERROR,
	TRANSACTIONS_MISSING,
	ALREADY_EXISTS,
	NOT_FOUND,
	INVALID,
	ORPHANED
};