#pragma once

#include "../NodeContext.h"

#include <Core/Context.h>
#include <Wallet/NodeClient.h>
#include <BlockChain/BlockChain.h>
#include <Common/Util/ScopedTiming.h>
#include <Database/Database.h>
#include <Database/BlockDb.h>
#include <PMMR/TxHashSetManager.h>
#include <PMMR/Common/Index.h>
#include <PMMR/Common/LeafIndex.h>
#include <TxPool/TransactionPool.h>
#include <optional>
#include "../../../P2P/PIBD/PibdActivation.h"

class DefaultNodeClient : public INodeClient
{
public:
	DefaultNodeClient(
		IDatabasePtr pDatabase,
		const TxHashSetManager::Ptr& pTxHashSetManager,
		const ITransactionPool::Ptr& pTransactionPool,
		const IBlockChain::Ptr& pBlockChain,
		IP2PServerPtr pP2PServer)
		: m_pDatabase(pDatabase),
		m_pTxHashSetManager(pTxHashSetManager),
		m_pTransactionPool(pTransactionPool),
		m_pBlockChain(pBlockChain),
		m_pP2PServer(pP2PServer)
	{

	}

	static std::shared_ptr<DefaultNodeClient> Create(const Context::Ptr& pContext)
	{
		// Settle an activation that was interrupted by a crash before the chain
		// opens the txhashset files.
		{
			ScopedTiming timing("node_pibd_activation_recover");
			PibdActivation::Recover(pContext->GetConfig().GetTxHashSetPath());
			timing.Success();
		}

		IDatabasePtr pDatabase;
		TxHashSetManager::Ptr pTxHashSetManager;
		std::shared_ptr<Locked<TxHashSetManager>> pLockedTxHashSetManager;
		ITransactionPool::Ptr pTransactionPool;
		std::shared_ptr<Locked<IHeaderMMR>> pHeaderMMR;
		IBlockChain::Ptr pBlockChainServer;
		IP2PServerPtr pP2PServer;

		{
			ScopedTiming timing("node_open_database");
			pDatabase = DatabaseAPI::OpenDatabase(pContext->GetConfig());
			timing.Success();
		}
		{
			ScopedTiming timing("node_create_txhashset_manager");
			pTxHashSetManager = std::make_shared<TxHashSetManager>(pContext->GetConfig());
			pLockedTxHashSetManager = std::make_shared<Locked<TxHashSetManager>>(pTxHashSetManager);
			timing.Success();
		}
		{
			ScopedTiming timing("node_create_txpool");
			pTransactionPool = TxPoolAPI::CreateTransactionPool(pContext->GetConfig());
			timing.Success();
		}
		{
			ScopedTiming timing("node_open_header_mmr");
			pHeaderMMR = HeaderMMRAPI::OpenHeaderMMR(pContext->GetConfig());
			timing.Success();
		}
		{
			ScopedTiming timing("node_open_blockchain");
			pBlockChainServer = BlockChainAPI::OpenBlockChain(
				pContext->GetConfig(),
				pDatabase->GetBlockDB(),
				pLockedTxHashSetManager,
				pTransactionPool,
				pHeaderMMR
			);
			timing.Success();
		}
		{
			ScopedTiming timing("node_start_p2p");
			pP2PServer = P2PAPI::StartP2PServer(
				pContext,
				pBlockChainServer,
				pLockedTxHashSetManager,
				pDatabase,
				pTransactionPool
			);
			timing.Success();
		}

		return std::make_shared<DefaultNodeClient>(
			pDatabase,
			pTxHashSetManager,
			pTransactionPool,
			pBlockChainServer,
			pP2PServer
		);
	}
    
    virtual ~DefaultNodeClient() = default;

	std::shared_ptr<NodeContext> GetNodeContext()
	{
		return std::make_shared<NodeContext>(NodeContext{
			m_pDatabase,
			m_pBlockChain,
			m_pP2PServer,
			m_pTxHashSetManager,
			m_pTransactionPool
		});
	}

	uint64_t GetChainHeight() const final
	{
		return m_pBlockChain->GetHeight(EChainType::CONFIRMED);
	}

	BlockHeaderPtr GetTipHeader() const final
	{
		return m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
	}

	BlockHeaderPtr GetBlockHeader(const uint64_t height) const final
	{
		return m_pBlockChain->GetBlockHeaderByHeight(height, EChainType::CONFIRMED);
	}

	std::map<Commitment, OutputLocation> GetOutputsByCommitment(const std::vector<Commitment>& commitments) const final
	{
		if (commitments.empty()) {
			return {};
		}

		// Take a coherent chain-state lease first (same lock order as the node API
		// handlers: chain state, then BlockDB). If the txhashset is missing or the
		// chain moves under us we must report "unavailable" instead of returning a
		// partial (empty) set: callers would otherwise mark unspent outputs SPENT.
		const auto snapshot = m_pBlockChain->GetApiStateSnapshot();
		if (!snapshot.valid || snapshot.tip == nullptr || snapshot.txHashSet == nullptr) {
			throw NodeUnavailableException("Local node is not ready (no coherent chain state).");
		}

		std::map<Commitment, OutputLocation> outputs;
		{
			const auto pBlockDB = m_pDatabase->GetBlockDB()->Read();
			for (const Commitment& commitment : commitments)
			{
				std::unique_ptr<OutputLocation> pOutputPosition = pBlockDB->GetOutputPosition(commitment);
				if (pOutputPosition != nullptr)
				{
					outputs.insert(std::make_pair(commitment, *pOutputPosition));
				}
			}
		}

		if (!m_pBlockChain->IsApiStateCurrent(snapshot.sequence)) {
			throw NodeUnavailableException("Chain state changed during output lookup.");
		}

		return outputs;
	}

	std::vector<BlockWithOutputs> GetBlockOutputs(const uint64_t startHeight, const uint64_t maxHeight) const final
	{
		return m_pBlockChain->GetOutputsByHeight(startHeight, maxHeight);
	}

	//
	// Legacy leaf-ordinal scan (0-based leaf index). Kept for source compatibility;
	// the wallet uses GetOutputsByPosition().
	//
	std::unique_ptr<OutputRange> GetOutputsByLeafIndex(const uint64_t startIndex, const uint64_t maxNumOutputs) const final
	{
		auto pTxHashSet = m_pTxHashSetManager->GetTxHashSet();
		if (pTxHashSet == nullptr) {
			throw NodeUnavailableException("Local node TxHashSet is not available.");
		}

		std::shared_ptr<const IBlockDB> pBlockDB;
		{
			const auto pReader = m_pDatabase->GetBlockDB()->Read();
			pBlockDB = pReader.GetShared();
		}
		return std::make_unique<OutputRange>(
			pTxHashSet->GetOutputsByLeafIndex(pBlockDB, startIndex, maxNumOutputs)
		);
	}

	//
	// Position-based scan with the same contract as the remote get_unspent_outputs
	// API. The local TxHashSet exposes a leaf-ordinal scan, so the PMMR position
	// is converted to a leaf position/ordinal here. The returned range keeps
	// position semantics (next 0-based PMMR position and output-MMR size), which
	// is what the wallet persists as its restore cursor.
	//
	std::unique_ptr<OutputRange> GetOutputsByPosition(const uint64_t startPosition, const uint64_t maxNumOutputs) const final
	{
		auto pTxHashSet = m_pTxHashSetManager->GetTxHashSet();
		if (pTxHashSet == nullptr) {
			throw NodeUnavailableException("Local node TxHashSet is not available.");
		}

		const BlockHeaderPtr pFlushed = pTxHashSet->GetFlushedBlockHeader();
		if (pFlushed == nullptr) {
			throw NodeUnavailableException("Local node TxHashSet has no flushed header.");
		}

		const uint64_t outputMMRSize = pFlushed->GetOutputMMRSize();
		const uint64_t startPosition0 = startPosition == 0 ? 0 : startPosition - 1;

		if (maxNumOutputs == 0 || startPosition0 >= outputMMRSize) {
			return std::make_unique<OutputRange>(
				outputMMRSize,
				startPosition0,
				std::vector<OutputDTO>{}
			);
		}

		const uint64_t firstLeafPosition = NextLeafPosition(startPosition0);
		if (firstLeafPosition >= outputMMRSize) {
			return std::make_unique<OutputRange>(
				outputMMRSize,
				outputMMRSize,
				std::vector<OutputDTO>{}
			);
		}

		std::shared_ptr<const IBlockDB> pBlockDB;
		{
			const auto pReader = m_pDatabase->GetBlockDB()->Read();
			pBlockDB = pReader.GetShared();
		}

		const uint64_t firstLeafOrdinal = LeafOrdinalForPosition(firstLeafPosition);
		OutputRange leafRange = pTxHashSet->GetOutputsByLeafIndex(pBlockDB, firstLeafOrdinal, maxNumOutputs);
		std::vector<OutputDTO> outputs = leafRange.GetOutputs();

		uint64_t lastPosition;
		if (outputs.size() < maxNumOutputs) {
			// The leaf scan ran to the end of the MMR.
			lastPosition = outputMMRSize;
		} else {
			// Resume right after the last returned leaf. The restorer rounds this
			// up to the next leaf position on the following call.
			lastPosition = outputs.back().GetMMRPosition() + 1;
		}

		return std::make_unique<OutputRange>(outputMMRSize, lastPosition, std::move(outputs));
	}

	//
	// Output-PMMR size at the end of the given block height.
	//
	std::optional<uint64_t> GetOutputMMRSizeAtHeight(const uint64_t height) const final
	{
		const BlockHeaderPtr pHeader = m_pBlockChain->GetBlockHeaderByHeight(height, EChainType::CONFIRMED);
		if (pHeader == nullptr) {
			return std::nullopt;
		}

		return std::make_optional(pHeader->GetOutputMMRSize());
	}

	//
	// Kernel lookup at the confirmed tip. The persistent excess -> position index
	// is used when it is bound to the exact tip; otherwise the in-memory kernel
	// MMR is scanned. A miss is reported as NOT_FOUND only when the view is
	// authoritative; if the chain state is unavailable the result is UNAVAILABLE.
	//
	KernelLookup GetKernel(const Commitment& excess) const final
	{
		const auto snapshot = m_pBlockChain->GetApiStateSnapshot();
		if (!snapshot.valid || snapshot.tip == nullptr || snapshot.txHashSet == nullptr) {
			return KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
		}

		try
		{
			const auto pBlockDB = m_pDatabase->GetBlockDB()->Read();
			const auto pIndexState = pBlockDB->GetKernelIndexState();
			if (pIndexState != nullptr &&
				pIndexState->GetTipHash() == snapshot.tip->GetHash() &&
				pIndexState->GetTipHeight() == snapshot.tip->GetHeight() &&
				pIndexState->GetKernelCount() == snapshot.tip->GetNumKernels() &&
				pIndexState->GetEntryCount() == snapshot.tip->GetNumKernels() &&
				snapshot.txHashSet->GetNumKernels() == snapshot.tip->GetNumKernels())
			{
				const auto pPosition = pBlockDB->GetKernelPosition(pIndexState->GetGeneration(), excess);
				if (pPosition != nullptr)
				{
					KernelLookup lookup;
					lookup.status = EKernelLookupStatus::FOUND;
					lookup.height = pPosition->GetBlockHeight();
					return lookup;
				}

				// The index is complete and bound to this exact tip: absence is final.
				return KernelLookup{ EKernelLookupStatus::NOT_FOUND, 0 };
			}
		}
		catch (const std::exception&)
		{
			// Fall through to the authoritative in-memory scan.
		}

		const auto kernelOpt = snapshot.txHashSet->FindKernelByExcess(excess, std::nullopt, std::nullopt);
		if (kernelOpt.has_value())
		{
			KernelLookup lookup;
			lookup.status = EKernelLookupStatus::FOUND;
			return lookup;
		}

		if (!m_pBlockChain->IsApiStateCurrent(snapshot.sequence)) {
			return KernelLookup{ EKernelLookupStatus::UNAVAILABLE, 0 };
		}

		return KernelLookup{ EKernelLookupStatus::NOT_FOUND, 0 };
	}

	//
	// Pure cursor-unit conversion helpers (0-based leaf ordinal <-> 0-based node
	// position). Public so regression tests can pin the mapping.
	//
	static uint64_t LeafPositionForOrdinal(const uint64_t leafOrdinal)
	{
		return LeafIndex::At(leafOrdinal).GetPosition();
	}

	static uint64_t LeafOrdinalForPosition(const uint64_t leafPosition0)
	{
		return Index::At(leafPosition0).GetLeafIndex();
	}

	static uint64_t NextLeafPosition(const uint64_t startPosition0)
	{
		Index index = Index::At(startPosition0);
		uint64_t guard = 0;
		while (!index.IsLeaf() && guard < 64) {
			index = index.GetNext();
			++guard;
		}

		return index.GetPosition();
	}

	bool IsPoolAcceptanceAuthoritative() const final { return true; }

	bool PostTransaction(TransactionPtr pTransaction, const EPoolType poolType) final
	{
		auto pTipHeader = m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTipHeader != nullptr) {
			std::shared_ptr<const IBlockDB> pBlockDB;
			{
				const auto pReader = m_pDatabase->GetBlockDB()->Read();
				pBlockDB = pReader.GetShared();
			}
			auto pTxHashSet = m_pTxHashSetManager->GetTxHashSet();
			if (pTxHashSet != nullptr) {
				try
				{
					auto result = m_pTransactionPool->AddTransaction(
						pBlockDB,
						pTxHashSet,
						pTransaction,
						poolType,
						*pTipHeader
					);
					
					if (result == EAddTransactionStatus::ADDED) {
						if (poolType == EPoolType::MEMPOOL) {
							m_pP2PServer->BroadcastTransaction(pTransaction);
						}

						return true;
					} else {
						LOG_ERROR_F("Failed to post {} for reason '{}'", pTransaction, (uint8_t)result);
					}
				}
				catch (std::exception& e)
				{
					LOG_ERROR_F("Failed to post {}. Exception: ", pTransaction, e.what());
					return false;
				}
			}
		}

		return false;
	}

	IP2PServerPtr GetP2PServer() { return m_pP2PServer; }

private:
	IDatabasePtr m_pDatabase;
	TxHashSetManager::Ptr m_pTxHashSetManager;
	ITransactionPool::Ptr m_pTransactionPool;
	IBlockChain::Ptr m_pBlockChain;
	IP2PServerPtr m_pP2PServer;
};
