#pragma once

#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/Hash.h>
#include <Core/Models/BlockHeader.h>
#include <Core/Models/OutputLocation.h>
#include <Core/Models/Transaction.h>
#include <Core/Models/DTOs/BlockWithOutputs.h>
#include <Core/Models/DTOs/OutputRange.h>
#include <TxPool/PoolType.h>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

//
// Typed node-client exceptions.
//
// The wallet refresh logic must be able to distinguish three cases:
//   * NodeUnavailableException - node is busy / resyncing / did not answer
//     consistently. The wallet must not mutate output statuses; it can safely
//     retry on the next refresh.
//   * NodeRejectedException    - the node rejected the request (invalid params,
//     unsupported method). This is a bug in the client and must not be silently
//     swallowed.
//   * NodeNotFoundException    - the node gave a valid negative answer (e.g. a
//     kernel is not on the canonical chain). It must never be collapsed into
//     "unavailable".
//
class NodeException : public std::runtime_error
{
public:
	explicit NodeException(const std::string& message)
		: std::runtime_error(message) { }
};

class NodeUnavailableException : public NodeException
{
public:
	explicit NodeUnavailableException(const std::string& message)
		: NodeException(message) { }
};

class NodeRejectedException : public NodeException
{
public:
	explicit NodeRejectedException(const std::string& message)
		: NodeException(message) { }
};

class NodeNotFoundException : public NodeException
{
public:
	explicit NodeNotFoundException(const std::string& message)
		: NodeException(message) { }
};

//
// Current confirmed chain tip. height and hash are read together so that a
// same-height reorg can be detected by comparing two tips.
//
struct NodeTip
{
	uint64_t height = 0;
	Hash hash = HASH::ZERO;
};

enum class EKernelLookupStatus
{
	FOUND,
	NOT_FOUND,
	UNAVAILABLE
};

struct KernelLookup
{
	EKernelLookupStatus status = EKernelLookupStatus::UNAVAILABLE;
	uint64_t height = 0;
};

//
// INodeClient is an interface whose implementations communicate with nodes in various ways.
// For example, an HTTPNodeClient will connect to a node using HTTP and the node's REST APIs.
//
class INodeClient
{
public:
	using UPtr = std::unique_ptr<INodeClient>;

	virtual ~INodeClient() = default;

	//
	// Returns the current confirmed chain height.
	//
	virtual uint64_t GetChainHeight() const = 0;

	//
	// Return the current confirmed chain tip header.
	//
	virtual BlockHeaderPtr GetTipHeader() const = 0;
	
	//
	// Returns the header of the confirmed block at the given height, if it exists.
	//
	virtual BlockHeaderPtr GetBlockHeader(const uint64_t height) const = 0;

	//
	// Returns the location (block height and mmr index) of each requested output, if it is *unspent*.
	//
	virtual std::map<Commitment, OutputLocation> GetOutputsByCommitment(const std::vector<Commitment>& commitments) const = 0;

	//
	// Returns a vector containing block ids and their outputs for the given range.
	//
	virtual std::vector<BlockWithOutputs> GetBlockOutputs(const uint64_t startHeight, const uint64_t maxHeight) const = 0;

	//
	// Legacy leaf-index scan.
	// startIndex is a 0-based leaf ordinal (the historical local backend unit).
	// Prefer GetOutputsByPosition() for wallet scanning: mixing the two units in
	// the persisted restore cursor corrupts the cursor.
	//
	virtual std::unique_ptr<OutputRange> GetOutputsByLeafIndex(const uint64_t startIndex, const uint64_t maxNumOutputs) const = 0;

	//
	// Position-based unspent-output scan used by the wallet restore/refresh.
	//
	// Contract (matches the node's get_unspent_outputs JSON-RPC API):
	//   * startPosition == 0 means "start from the first node"; otherwise it is a
	//     1-based output-PMMR node position.
	//   * OutputRange::GetLastRetrievedIndex() is the 0-based PMMR position of the
	//     next node to scan. This is exactly what the wallet stores as the
	//     restore cursor.
	//   * OutputRange::GetHighestIndex() is the output-PMMR size (node count) at
	//     the time of the call.
	//
	// Implementations that cannot provide position semantics must return nullptr
	// (callers treat that as "node not ready"); returning leaf-ordinal semantics
	// here would corrupt the persisted cursor.
	//
	virtual std::unique_ptr<OutputRange> GetOutputsByPosition(const uint64_t /*startPosition*/, const uint64_t /*maxNumOutputs*/) const
	{
		return std::unique_ptr<OutputRange>(nullptr);
	}

	//
	// Returns the current confirmed tip (height + hash) from a single consistent read.
	// Default implementation derives it from GetTipHeader().
	//
	virtual std::optional<NodeTip> GetTip() const
	{
		BlockHeaderPtr pTip = GetTipHeader();
		if (pTip == nullptr) {
			return std::nullopt;
		}

		NodeTip tip;
		tip.height = pTip->GetHeight();
		tip.hash = pTip->GetHash();
		return std::make_optional(tip);
	}

	//
	// Looks up a transaction kernel by excess commitment on the canonical chain.
	// UNAVAILABLE means the node could not answer authoritatively; callers must
	// not treat it as NOT_FOUND.
	//
	virtual KernelLookup GetKernel(const Commitment& /*excess*/) const
	{
		return KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
	}

	//
	// Returns the output-PMMR size (node count) at the end of the given block
	// height, or nullopt when the height is unavailable. Used to rewind the
	// restore cursor after a reorg.
	//
	virtual std::optional<uint64_t> GetOutputMMRSizeAtHeight(const uint64_t /*height*/) const
	{
		return std::nullopt;
	}

	//
	// True when a successful PostTransaction means the node's own pool accepted
	// the transaction (local built-in node). Remote/JOIN clients return false:
	// their "Ok" only proves the service replied, not that the pool accepted it.
	//
	virtual bool IsPoolAcceptanceAuthoritative() const { return false; }

	//
	// Posts the transaction to the P2P Network.
	//
	virtual bool PostTransaction(TransactionPtr pTransaction, const EPoolType poolType) = 0;
};

typedef std::shared_ptr<INodeClient> INodeClientPtr;
typedef std::shared_ptr<const INodeClient> INodeClientConstPtr;
