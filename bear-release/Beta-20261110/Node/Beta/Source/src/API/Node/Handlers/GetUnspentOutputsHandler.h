#pragma once

#include <BlockChain/BlockChain.h>
#include <Database/Database.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <API/Wallet/Owner/Models/Errors.h>
#include <PMMR/TxHashSetManager.h>
#include <Core/Util/JsonUtil.h>
#include <json/json.h>
#include <algorithm>
#include <optional>
#include <string>

// Grin v5.5.2 foreign API: get_unspent_outputs.
//
// Params: [start_index, end_index?, max?, include_proof?]
//
// Rust semantics (api/src/handlers/chain_api.rs, chain/src/txhashset/txhashset.rs,
// core/src/core/pmmr/readonly_pmmr.rs):
//   * start_index is a 1-based PMMR position; scanning starts at its 0-based
//     position (start_index.saturating_sub(1)).
//   * end_index, when present, is a 0-based *exclusive* PMMR node bound.
//   * max counts returned unspent outputs (0 is valid and returns an empty page).
//   * last_retrieved_index is the 0-based position where scanning stopped
//     (the next node to scan), NOT the 1-based mmr_index of the last output.
//   * highest_index is end_index when supplied, otherwise the output MMR size.
class GetUnspentOutputsHandler : public RPCMethod
{
public:
	GetUnspentOutputsHandler(
		const IBlockChain::Ptr& pBlockChain,
		const TxHashSetManager::Ptr& pTxHashSetManager,
		const IDatabasePtr& pDatabase)
		: m_pBlockChain(pBlockChain),
		m_pTxHashSetManager(pTxHashSetManager),
		m_pDatabase(pDatabase) { }
	~GetUnspentOutputsHandler() = default;

	struct ScanPlan
	{
		bool hasWork = false;
		uint64_t startPosition = 0;
		uint64_t scanEndPosition = 0;
		uint64_t bound = 0;
		uint64_t highestIndex = 0;
	};

	// Pure arithmetic used by the handler and by unit tests. maxPositions bounds
	// the number of PMMR positions examined by a single call (DoS guard; the
	// Rust node scans unbounded when outputs are sparse).
	static ScanPlan PlanScan(
		const uint64_t startIndex,
		const std::optional<uint64_t>& endIndex,
		const uint64_t size,
		const uint64_t maxPositions)
	{
		ScanPlan plan;
		plan.bound = endIndex.value_or(size);
		plan.highestIndex = plan.bound;
		plan.startPosition = startIndex == 0 ? 0 : startIndex - 1;

		if (maxPositions == 0) {
			return plan;
		}

		const uint64_t scanLimit = (std::min)(plan.bound, size);
		if (plan.startPosition >= scanLimit) {
			return plan;
		}

		uint64_t count = scanLimit - plan.startPosition;
		if (count > maxPositions) {
			count = maxPositions;
		}

		plan.scanEndPosition = plan.startPosition + count - 1;
		plan.hasWork = true;
		return plan;
	}

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value()) {
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray() || params.empty() || params[0].isNull()) {
			return request.BuildError("INVALID_PARAMS", "Expected at least 1 parameter: start_index");
		}

		const uint64_t startIndex = JsonUtil::ConvertToUInt64(params[0]);

		std::optional<uint64_t> endIndex;
		if (params.size() > 1 && !params[1].isNull()) {
			endIndex = JsonUtil::ConvertToUInt64(params[1]);
		}

		uint64_t max = 1000;
		if (params.size() > 2 && !params[2].isNull()) {
			max = JsonUtil::ConvertToUInt64(params[2]);
		}
		max = (std::min)(max, (uint64_t)MaxOutputsPerCall);

		bool includeProof = false;
		if (params.size() > 3 && !params[3].isNull()) {
			includeProof = params[3].asBool();
		}

		for (int attempt = 0; attempt < 3; attempt++) {
			const auto snapshot = m_pBlockChain->GetApiStateSnapshot();
			if (!snapshot.valid || snapshot.tip == nullptr || snapshot.txHashSet == nullptr) {
				continue;
			}

			const BlockHeaderPtr pFlushed = snapshot.txHashSet->GetFlushedBlockHeader();
			if (pFlushed == nullptr) {
				return request.BuildError("INTERNAL_ERROR", "TxHashSet has no flushed header.");
			}

			const uint64_t size = pFlushed->GetOutputMMRSize();
			const ScanPlan plan = PlanScan(startIndex, endIndex, size, MaxScannedPositions);

			Json::Value outputs(Json::arrayValue);
			uint64_t lastRetrieved = plan.startPosition;

			if (plan.hasWork && max > 0) {
				std::vector<OutputDTO> found;
				{
					// Take a reference-counted DB snapshot and release the DB lock
					// before entering the TxHashSet. Holding BlockDB while waiting
					// for a PMMR scan can starve block application.
					std::shared_ptr<const IBlockDB> pBlockDB;
					{
						const auto pReader = m_pDatabase->GetBlockDB()->Read();
						pBlockDB = pReader.GetShared();
					}
					found = snapshot.txHashSet->GetOutputsByMMRIndex(
						pBlockDB, plan.startPosition, plan.scanEndPosition);
				}

				for (const OutputDTO& output : found) {
					if (outputs.size() >= max) {
						break;
					}

					Json::Value outputJSON = output.ToJSON();
					outputJSON["spent"] = false;
					if (!includeProof) {
						outputJSON["proof"] = Json::nullValue;
					}

					outputs.append(outputJSON);
					// 0-based position after the returned output.
					lastRetrieved = output.GetLocation().GetLeafIndex().GetPosition() + 1;
				}

				if (outputs.size() < max) {
					// Everything requested was scanned: Rust returns the exclusive
					// bound (which may lie beyond the actual MMR size).
					const uint64_t scanLimit = (std::min)(plan.bound, size);
					if (plan.scanEndPosition + 1 >= scanLimit) {
						lastRetrieved = plan.bound;
					} else {
						lastRetrieved = plan.scanEndPosition + 1;
					}
				}
			}

			if (!m_pBlockChain->IsApiStateCurrent(snapshot.sequence)) {
				continue;
			}

			Json::Value listing;
			listing["last_retrieved_index"] = Json::UInt64(lastRetrieved);
			listing["highest_index"] = Json::UInt64(plan.highestIndex);
			listing["outputs"] = outputs;

			Json::Value result;
			result["Ok"] = listing;
			return request.BuildResult(result);
		}

		return request.BuildError("NODE_BUSY_RETRY", "Chain state changed during the request");
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	static constexpr uint64_t MaxOutputsPerCall = 10000;
	static constexpr uint64_t MaxScannedPositions = 8192;

	IBlockChain::Ptr m_pBlockChain;
	TxHashSetManager::Ptr m_pTxHashSetManager;
	IDatabasePtr m_pDatabase;
};
