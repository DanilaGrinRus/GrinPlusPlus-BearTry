#include "BlockChainImpl.h"
#include "BlockHydrator.h"
#include "CompactBlockFactory.h"
#include "Validators/BlockHeaderValidator.h"
#include "Validators/BlockValidator.h"
#include "Processors/BlockHeaderProcessor.h"
#include "Processors/TxHashSetProcessor.h"
#include "Processors/BlockProcessor.h"
#include "ChainResyncer.h"
#include "CoinView.h"
#include "ChainConsistency.h"
#include "HeaderStateCheckpoint.h"
#include <Core/Enums/Environment.h>
#include <PMMR/KernelIndex.h>
#include "DbCheckpoint.h"
#include <BlockChain/DbRecovery.h>
#include <PMMR/Common/MmrStructure.h>
#include "../PMMR/Common/MMRHashUtil.h"
#include <PMMR/Common/LeafIndex.h>
#include <Core/Serialization/Serializer.h>
#include <Database/DbBackup.h>
#include <Database/BlockDb.h>

#include <Database/DbBackup.h>

#include <Consensus.h>
#include <GrinVersion.h>
#include <Common/Logger.h>
#include <Common/Util/ScopedTiming.h>
#include <Core/Global.h>
#include <Core/Exceptions/BadDataException.h>
#include <Core/Config.h>

#ifdef _MSC_VER
#pragma warning(disable: 4996) // getenv for the experimental anchor env
#endif
#include <PMMR/TxHashSet.h>
#include "../PMMR/TxHashSetImpl.h"
#include <filesystem.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace
{
	class CoinViewStartupGuard
	{
	public:
		CoinViewStartupGuard() : m_previous(Global::GetCoinView()) { }
		~CoinViewStartupGuard() { if (!m_committed) Global::SetCoinView(m_previous); }
		void Commit() { m_committed = true; }
	private:
		std::shared_ptr<const ICoinView> m_previous;
		bool m_committed = false;
	};

	// Rebuild the in-memory NRD recent-kernel index from the kernel MMR plus the
	// header chain and require it to be complete. Rust grin 5.5.2 treats an
	// invalid/incomplete NRD index as an error for the whole txhashset write
	// (TxHashSet::verify_kernel_pos_index), so callers must not publish or keep
	// using a state whose index could not be fully rebuilt.
	bool RebuildNrdIndexAndRequireComplete(
		ITxHashSet* pTxHashSet,
		const std::shared_ptr<const IBlockDB>& pBlockDB,
		const BlockHeaderPtr& pTip,
		const char* context)
	{
		if (pTxHashSet == nullptr || pBlockDB == nullptr || pTip == nullptr) {
			LOG_ERROR_F("Cannot rebuild the NRD recent-kernel index ({}): missing txhashset, block DB or tip", context);
			return false;
		}

		// TxHashSet is currently the only ITxHashSet implementation. Treat an
		// unexpected implementation as fail-closed rather than skipping history.
		TxHashSet* pConcrete = dynamic_cast<TxHashSet*>(pTxHashSet);
		if (pConcrete == nullptr) {
			LOG_ERROR_F(
				"Cannot rebuild the NRD recent-kernel index ({}): unexpected ITxHashSet implementation",
				context);
			return false;
		}

		pConcrete->RebuildRecentKernelIndex(pBlockDB, *pTip);
		if (!pConcrete->IsRecentNrdIndexComplete()) {
			LOG_ERROR_F(
				"NRD recent-kernel index rebuild is incomplete at {} (tip {} height {}); refusing to treat this txhashset state as valid",
				context,
				pTip->GetHash(),
				pTip->GetHeight());
			return false;
		}

		return true;
	}
}

BlockChain::BlockChain(
	std::shared_ptr<ITransactionPool> pTransactionPool,
	std::shared_ptr<Locked<ChainState>> pChainState)
	: m_pTransactionPool(pTransactionPool), m_pChainState(pChainState)
{

}

BlockChain::StateWriteGuard::StateWriteGuard(BlockChain& chain)
	: m_chain(chain), m_lock(chain.m_stateWriteMutex)
{
	m_chain.m_stateSequence.fetch_add(1, std::memory_order_release);
}

BlockChain::StateWriteGuard::~StateWriteGuard()
{
	m_chain.m_stateSequence.fetch_add(1, std::memory_order_release);
}
void BlockChain::StartHeaderCheckpointUpdater()
{
	const HeaderCheckpoint::EMode mode = HeaderCheckpoint::ModeFromEnv();
	if (mode != HeaderCheckpoint::EMode::Write && mode != HeaderCheckpoint::EMode::Fast) {
		return;
	}

	HeaderCheckpointData checkpoint;
	std::string error;
	if (!HeaderCheckpoint::Read(HeaderCheckpoint::Path(Global::GetConfig()), checkpoint, error) ||
		(checkpoint.Trust != HeaderCheckpointData::TRUST_EXTERNAL &&
		 checkpoint.Trust != HeaderCheckpointData::TRUST_DERIVED)) {
		// Legacy/local bootstrap trust (TRUST_BOOTSTRAP) is not an external
		// anchor. Do not start the updater from it.
		return;
	}

	{
		std::lock_guard<std::mutex> lock(m_headerCheckpointMutex);
		m_headerCheckpointStop = false;
		m_headerCheckpointDirty = false;
	}

	m_headerCheckpointThread = std::thread([this]() {
		while (true) {
			std::unique_lock<std::mutex> lock(m_headerCheckpointMutex);
			m_headerCheckpointCv.wait(lock, [this]() {
				return m_headerCheckpointStop || m_headerCheckpointDirty;
			});
			if (m_headerCheckpointStop) {
				break;
			}

			// Coalesce bursts of header batches. Only a quiet period starts the
			// expensive full-state hash, so normal sync is not constantly
			// interrupted by checkpoint work.
			m_headerCheckpointDirty = false;
			auto quietDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!m_headerCheckpointStop) {
				if (m_headerCheckpointCv.wait_until(lock, quietDeadline, [this]() {
						return m_headerCheckpointStop || m_headerCheckpointDirty;
					})) {
					if (m_headerCheckpointStop) {
						break;
					}
					m_headerCheckpointDirty = false;
					quietDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
					continue;
				}
				break;
			}
			if (m_headerCheckpointStop) {
				break;
			}
			lock.unlock();

			try {
				auto pStateReader = m_pChainState->Read();
				auto pChainReader = pStateReader->GetChainStore();
				auto pDbReader = pStateReader->GetBlockDB();
				auto pHeaderReader = pStateReader->GetHeaderMMR();
				HeaderCheckpointMetrics metrics;
				std::string refreshError;
				LOG_INFO("event=diagnostic_refresh_enter");
				if (HeaderCheckpoint::Refresh(
						Global::GetConfig(),
						*pDbReader,
						*pHeaderReader,
						*pChainReader->GetCandidateChain(),
						*pChainReader->GetConfirmedChain(),
						metrics,
						refreshError)) {
					LOG_INFO_F(
						"event=header_checkpoint_refresh duration_ms={} bytes={} candidate={} trust_preserved=1",
						metrics.DurationMs,
						metrics.BytesRead,
						metrics.CandidateEntries);
				} else if (refreshError != "checkpoint is already current") {
					LOG_DEBUG_F("event=header_checkpoint_refresh_skipped reason={}", refreshError);
				}
			} catch (const std::exception& e) {
				LOG_WARNING_F("event=header_checkpoint_refresh_failed error={}", e.what());
			}
		}
	});
}

void BlockChain::StopHeaderCheckpointUpdater()
{
	{
		std::lock_guard<std::mutex> lock(m_headerCheckpointMutex);
		m_headerCheckpointStop = true;
	}
	m_headerCheckpointCv.notify_all();
	if (m_headerCheckpointThread.joinable()) {
		m_headerCheckpointThread.join();
	}
}

void BlockChain::NotifyHeaderCheckpointChanged()
{
	{
		std::lock_guard<std::mutex> lock(m_headerCheckpointMutex);
		if (m_headerCheckpointStop || !m_headerCheckpointThread.joinable()) {
			return;
		}
		m_headerCheckpointDirty = true;
	}
	m_headerCheckpointCv.notify_all();
}

BlockChain::~BlockChain()
{
	StopHeaderCheckpointUpdater();

	// Persist a checkpoint for the state reached by this process. This is done
	// here rather than on every header batch so sync is never blocked by a
	// 3M-header hash pass. A crash leaves the previous checkpoint untouched and
	// the next startup performs full validation.
	if (m_pChainState != nullptr &&
		(HeaderCheckpoint::ModeFromEnv() == HeaderCheckpoint::EMode::Write ||
		 HeaderCheckpoint::ModeFromEnv() == HeaderCheckpoint::EMode::Fast)) {
		try {
			auto pChainStateReader = m_pChainState->Read();
			auto pChainReader = pChainStateReader->GetChainStore();
			auto pDbReader = pChainStateReader->GetBlockDB();
			auto pHeaderReader = pChainStateReader->GetHeaderMMR();

			HeaderCheckpointMetrics metrics;
			std::string error;
			if (HeaderCheckpoint::Refresh(
					Global::GetConfig(),
					*pDbReader,
					*pHeaderReader,
					*pChainReader->GetCandidateChain(),
					*pChainReader->GetConfirmedChain(),
					metrics,
					error)) {
				LOG_INFO_F(
					"event=header_checkpoint_refresh duration_ms={} bytes={} candidate={} aggregate={}",
					metrics.DurationMs,
					metrics.BytesRead,
					metrics.CandidateEntries,
					"updated"
				);
			} else {
				LOG_INFO_F("event=header_checkpoint_refresh skipped reason={}", error);
			}
		} catch (const std::exception& e) {
			LOG_WARNING_F("event=header_checkpoint_refresh failed error={}", e.what());
		}
	}

	Global::SetCoinView(nullptr);
	m_pChainState.reset();
	m_pTransactionPool.reset();
}

std::shared_ptr<BlockChain> BlockChain::Create(
	const Config& config,
	std::shared_ptr<Locked<IBlockDB>> pDatabase,
	std::shared_ptr<Locked<TxHashSetManager>> pTxHashSetManager,
	std::shared_ptr<ITransactionPool> pTransactionPool,
	std::shared_ptr<Locked<IHeaderMMR>> pHeaderMMR)
{
	ScopedTiming createTiming("blockchain_create");
	const FullBlock& genesisBlock = Global::GetGenesisBlock();
	auto pGenesisIndex = std::make_shared<BlockIndex>(genesisBlock.GetHash(), 0);

	std::shared_ptr<Locked<ChainStore>> pChainStore;
	{
		ScopedTiming timing("blockchain_chain_store_load");
		pChainStore = ChainStore::Load(config, pGenesisIndex);
		timing.Success();
	}

	// header_mmr.bin and the chain files are committed independently. A crash
	// between the commits used to leave them at different heights and loading
	// such a state read past the MMR (G-23/G-48). Repair invariants without
	// throwing away headers: a lagging MMR is rebuilt from the block DB, and a
	// missing chain segment aborts startup instead of silently truncating data.
	{
		ScopedTiming timing("blockchain_chain_store_repair");
		const fs::path checkpointPath = config.GetChainPath() / "chain.checkpoint";
		DbCheckpointData checkpoint;
		const bool hasCheckpoint = DbCheckpoint::Read(checkpointPath, checkpoint);
		if (hasCheckpoint) {
			LOG_TRACE_F("DB checkpoint found ({}).", DbCheckpoint::Describe(checkpoint));
		}

		bool backedUp = false;
		const auto ensureBackup = [&]() {
			if (backedUp) {
				return;
			}

			const bool copied = DbRecovery::CopyForRecovery(
				config.GetChainPath(),
				{ "header_mmr.bin", "candidate.chain", "confirmed.chain", "chain.checkpoint" });
			if (!copied) {
				LOG_ERROR("Refusing to modify chain stores: recovery copy failed.");
				throw std::runtime_error("Chain store repair aborted: recovery copy failed");
			}

			backedUp = true;
			DbRecovery::RecordRepair();
		};

		const auto rebuildHeaderMmr = [&](const uint64_t fromHeight) {
			auto pCandidateChain = pChainStore->Read()->GetCandidateChain();
			const uint64_t targetHeight = pCandidateChain->GetHeight();
			LOG_WARNING_F(
				"Rebuilding header MMR from the block DB: leaves {} -> heights {}-{}.",
				fromHeight,
				fromHeight,
				targetHeight);

			auto pMmrWriter = pHeaderMMR->Write();
			auto pBlockDBReader = pDatabase->Read();
			for (uint64_t height = fromHeight; height <= targetHeight; height++) {
				const Hash& hash = pCandidateChain->GetHash(height);
				const BlockHeaderPtr pHeader = pBlockDBReader->GetBlockHeader(hash);
				if (pHeader == nullptr) {
					throw std::runtime_error(
						"Cannot rebuild the header MMR: a block header is missing at height " +
						std::to_string(height));
				}

				pMmrWriter->AddHeader(*pHeader);
				if (height != 0 && height % 100000 == 0) {
					LOG_INFO_F("Rebuilding header MMR: height {}", height);
				}
			}

			pMmrWriter->Commit();
		};

		// A raw node count that cannot describe any MMR means the append-only file
		// was truncated mid-write. Cut it back to the largest valid size; the
		// missing leaves are rebuilt from the block DB below.
		const uint64_t rawMmrNodes = pHeaderMMR->Read()->GetNodeCount();
		if (!MmrStructure::IsValidNodeCount(rawMmrNodes)) {
			const uint64_t validNodes = MmrStructure::LargestValidNodeCount(rawMmrNodes);
			const uint64_t validLeaves = MmrStructure::LeavesForNodes(validNodes);
			LOG_WARNING_F(
				"Header MMR has {} nodes, which is not a valid MMR size; repairing to {} nodes ({} leaves).",
				rawMmrNodes,
				validNodes,
				validLeaves);

			if (validNodes > 0 && validLeaves > 0) {
				ensureBackup();
				auto pMmrWriter = pHeaderMMR->Write();
				pMmrWriter->Rewind(validLeaves);
				pMmrWriter->Commit();
			} else if (rawMmrNodes > 0) {
				LOG_ERROR("Refusing to truncate the header MMR: could not derive a valid leaf count.");
				throw std::runtime_error("Header MMR structure is invalid");
			}
		}

		uint64_t headerMmrLeaves = pHeaderMMR->Read()->GetLeafCount();
		uint64_t candidateEntries =
			pChainStore->Read()->GetCandidateChain()->GetHeight() + 1;
		uint64_t confirmedEntries =
			pChainStore->Read()->GetConfirmedChain()->GetHeight() + 1;

		// A brand-new database has only genesis in the chain index; its header MMR
		// is populated by ChainState::Create just below. Do not rebuild it here.
		const bool genesisPending = candidateEntries == 1 && headerMmrLeaves == 0;

		if (hasCheckpoint) {
			// A chain index shorter than the checkpoint cannot be rebuilt from
			// headers alone (fork choice is lost): stop instead of truncating.
			if (candidateEntries < checkpoint.CandidateEntries) {
				throw std::runtime_error(
					"Chain index is behind the checkpoint; refusing to truncate. "
					"Restore NODE/recovery-* or resync from a peer.");
			}

			// Confirm the chain can be restored from the candidate chain when its
			// tail was lost between two commits.
			if (confirmedEntries < checkpoint.ConfirmedEntries) {
				ensureBackup();
				{
					auto pChainStoreWriter = pChainStore->Write();
					auto pConfirmedChain = pChainStoreWriter->GetConfirmedChain();
					const auto pCandidateChain = pChainStore->Read()->GetCandidateChain();
					const uint64_t target = (std::min)(checkpoint.ConfirmedEntries, candidateEntries);
					for (uint64_t height = confirmedEntries; height < target; height++) {
						pConfirmedChain->AddBlock(pCandidateChain->GetHash(height), height);
					}
					pChainStoreWriter->Commit();
				}
				confirmedEntries = pChainStore->Read()->GetConfirmedChain()->GetHeight() + 1;
				LOG_WARNING_F("Restored confirmed chain to {} entries.", confirmedEntries);
			}
		}

		if (headerMmrLeaves > candidateEntries) {
			// Extra MMR leaves are not indexed by the chain; drop them.
			ensureBackup();
			LOG_WARNING_F(
				"Header MMR is ahead of the candidate chain ({} > {}); truncating the MMR.",
				headerMmrLeaves,
				candidateEntries);
			auto pMmrWriter = pHeaderMMR->Write();
			pMmrWriter->Rewind(candidateEntries);
			pMmrWriter->Commit();
			headerMmrLeaves = pHeaderMMR->Read()->GetLeafCount();
		} else if (headerMmrLeaves < candidateEntries && !genesisPending) {
			// Never truncate the chain for a lagging MMR: the headers are in the
			// block DB, so rebuild the MMR instead (live data-loss regression).
			ensureBackup();
			rebuildHeaderMmr(headerMmrLeaves);
			headerMmrLeaves = pHeaderMMR->Read()->GetLeafCount();
		}

		if (confirmedEntries > candidateEntries) {
			ensureBackup();
			LOG_WARNING_F(
				"Confirmed chain is ahead of the candidate chain ({} > {}); truncating the confirmed chain.",
				confirmedEntries,
				candidateEntries);
			{
				auto pChainStoreWriter = pChainStore->Write();
				pChainStoreWriter->GetConfirmedChain()->Rewind(candidateEntries - 1);
				pChainStoreWriter->Commit();
			}
			confirmedEntries = pChainStore->Read()->GetConfirmedChain()->GetHeight() + 1;
		}

		if (headerMmrLeaves != candidateEntries && !genesisPending) {
			throw std::runtime_error("Header MMR could not be made consistent with the chain index");
		}

		if (genesisPending) {
			// ChainState::Create adds genesis to the MMR and commits a checkpoint.
			LOG_DEBUG("Fresh chain store: genesis header will initialise the MMR.");
		} else {
			// Publish the verified state as the new checkpoint.
			DbCheckpointData fresh;
		fresh.Sequence = (hasCheckpoint ? checkpoint.Sequence : 0) + 1;
		fresh.MmrLeaves = headerMmrLeaves;
		fresh.CandidateEntries = candidateEntries;
		fresh.ConfirmedEntries = confirmedEntries;
			if (!DbCheckpoint::Write(checkpointPath, fresh)) {
				LOG_WARNING("Failed to write the DB checkpoint.");
			}
		}
		timing.Success();
	}
	const HeaderCheckpoint::EMode checkpointMode = HeaderCheckpoint::ModeFromEnv();
	const bool forceFull = HeaderCheckpoint::ForceFullFromEnv();
	bool headerCheckpointFastAccepted = false;

	if (checkpointMode == HeaderCheckpoint::EMode::HashOnly) {
		auto pChainReader = pChainStore->Read();
		auto pDbReader = pDatabase->Read();
		auto pHeaderReader = pHeaderMMR->Read();
		HeaderCheckpointData data;
		HeaderCheckpointMetrics metrics;
		std::string checkpointError;
		if (!HeaderCheckpoint::Compute(
				config,
				*pDbReader,
				*pHeaderReader,
				*pChainReader->GetCandidateChain(),
				*pChainReader->GetConfirmedChain(),
				false,
				false,
				data,
				metrics,
				checkpointError)) {
			throw std::runtime_error("header checkpoint hash-only computation failed: " + checkpointError);
		}

		const double seconds = (std::max)((uint64_t)1, metrics.DurationMs) / 1000.0;
		LOG_INFO_F(
			"event=header_checkpoint_hash_only duration_ms={} cpu_ms={} bytes={} mb_per_s={:.3f} db_ms={} serialize_ms={} hash_ms={} threads={} candidate={} confirmed={} mmr_nodes={} aggregate={}",
			metrics.DurationMs,
			metrics.CpuMs,
			metrics.BytesRead,
			(metrics.BytesRead / (1024.0 * 1024.0)) / seconds,
			metrics.DbReadMs,
			metrics.SerializeMs,
			metrics.HashMs,
			metrics.Threads,
			data.CandidateEntries,
			data.ConfirmedEntries,
			data.MmrNodes,
			data.Aggregate.ToHex()
		);
		throw std::runtime_error("header checkpoint hash-only measurement completed");
	}

	DbCheckpointData validationCheckpoint;
	const bool hasValidationCheckpoint = DbCheckpoint::Read(
		config.GetChainPath() / "chain.checkpoint", validationCheckpoint);

	// Test-only mutation hook. It changes the DB record but deliberately leaves
	// the candidate chain and header MMR untouched, which is exactly the case
	// the old G-23 could not see for fields outside the PoW-cycle hash.
	{
		auto pMutationWriter = pDatabase->Write();
		std::string mutationError;
		if (!HeaderCheckpoint::MutateHeaderForTest(
				*pChainStore->Read()->GetCandidateChain(),
				*pMutationWriter.GetShared(),
				mutationError)) {
			throw std::runtime_error("header field mutation failed: " + mutationError);
		}
		// Writer commits on scope exit.
	}

	if (hasValidationCheckpoint &&
		checkpointMode == HeaderCheckpoint::EMode::Fast &&
		!forceFull) {
		HeaderCheckpointData storedCheckpoint;
		std::string checkpointError;
		const bool checkpointRead = HeaderCheckpoint::Read(
			HeaderCheckpoint::Path(config), storedCheckpoint, checkpointError);
		if (checkpointRead &&
			(storedCheckpoint.Trust != HeaderCheckpointData::TRUST_EXTERNAL &&
			 storedCheckpoint.Trust != HeaderCheckpointData::TRUST_DERIVED)) {
			LOG_WARNING_F(
				"event=header_checkpoint_full_fallback reason=untrusted_checkpoint trust={} trust_source={}",
				(uint32_t)storedCheckpoint.Trust,
				(uint32_t)storedCheckpoint.TrustSource);
		} else if (checkpointRead) {
			auto pChainReader = pChainStore->Read();
			auto pDbReader = pDatabase->Read();
			auto pHeaderReader = pHeaderMMR->Read();

			HeaderCheckpointData currentMetadata;
			if (HeaderCheckpoint::ReadMetadata(
					config,
					*pDbReader,
					*pHeaderReader,
					*pChainReader->GetCandidateChain(),
					*pChainReader->GetConfirmedChain(),
					currentMetadata,
					checkpointError)) {
				if (HeaderCheckpoint::MetadataEquals(storedCheckpoint, currentMetadata)) {
					HeaderCheckpointData current;
					HeaderCheckpointMetrics metrics;
					if (!HeaderCheckpoint::Compute(
							config,
							*pDbReader,
							*pHeaderReader,
							*pChainReader->GetCandidateChain(),
							*pChainReader->GetConfirmedChain(),
							false,
							false,
							current,
							metrics,
							checkpointError)) {
						throw std::runtime_error("header checkpoint fast verification failed: " + checkpointError);
					}

					if (HeaderCheckpoint::StateEquals(storedCheckpoint, current)) {
						headerCheckpointFastAccepted = true;
						LOG_INFO_F(
							"event=header_checkpoint_fast duration_ms={} cpu_ms={} bytes={} db_ms={} serialize_ms={} hash_ms={} threads={} candidate={} tip={} aggregate={}",
							metrics.DurationMs,
							metrics.CpuMs,
							metrics.BytesRead,
							metrics.DbReadMs,
							metrics.SerializeMs,
							metrics.HashMs,
							metrics.Threads,
							current.CandidateEntries,
							current.CandidateTip.ToHex(),
							current.Aggregate.ToHex()
						);
					} else {
						LOG_ERROR_F(
							"event=header_checkpoint_mismatch {}",
							HeaderCheckpoint::DescribeMismatch(storedCheckpoint, current)
						);
						throw std::runtime_error("header checkpoint mismatch; refusing to start");
					}
				} else {
					// A trusted checkpoint can be behind by a bounded, purely
					// append-only suffix. Refresh validates the old prefix and the
					// new header/MMR/confirmed links, then atomically writes the
					// extended checkpoint. Reorgs, gaps, corruption and any other
					// doubt still fall through to full validation.
					HeaderCheckpointMetrics extensionMetrics;
					std::string extensionError;
					if (HeaderCheckpoint::Refresh(
							config,
							*pDbReader,
							*pHeaderReader,
							*pChainReader->GetCandidateChain(),
							*pChainReader->GetConfirmedChain(),
							extensionMetrics,
							extensionError,
							true)) {
						headerCheckpointFastAccepted = true;
						LOG_INFO_F(
							"event=header_checkpoint_fast_extended duration_ms={} bytes={} threads={} candidate={} tip={}",
							extensionMetrics.DurationMs,
							extensionMetrics.BytesRead,
							extensionMetrics.Threads,
							currentMetadata.CandidateEntries,
							currentMetadata.CandidateTip.ToHex());
					} else {
						LOG_WARNING_F(
							"event=header_checkpoint_full_fallback reason=extension_failed expected_candidate={} actual_candidate={} error={}",
							storedCheckpoint.CandidateEntries,
							currentMetadata.CandidateEntries,
							extensionError);
					}
				}
			} else {
				LOG_WARNING_F("event=header_checkpoint_full_fallback reason=metadata_unavailable error={}", checkpointError);
			}
		} else {
			LOG_WARNING_F("event=header_checkpoint_full_fallback reason=checkpoint_unusable error={}", checkpointError);
		}
	}

	bool headerFullValidationRan = false;
	if (!headerCheckpointFastAccepted) {
		// Full validation is a durable-store check. Run it whenever the chain
		// contains real headers, not only when chain.checkpoint happened to be
		// readable: structural repair can otherwise hide a missing checkpoint.
		auto pChainReader = pChainStore->Read();
		auto pDbReader = pDatabase->Read();
		auto pHeaderReader = pHeaderMMR->Read();
		const uint64_t candidateEntries = pChainReader->GetCandidateChain()->GetHeight() + 1;
		const uint64_t mmrLeaves = pHeaderReader->GetLeafCount();
		if (candidateEntries > 1 && mmrLeaves > 0) {
			HeaderCheckpointMetrics validationMetrics;
			std::string validationError;
			if (!HeaderCheckpoint::VerifyFull(
					config,
					*pDbReader,
					*pHeaderReader,
					*pChainReader->GetCandidateChain(),
					*pChainReader->GetConfirmedChain(),
					validationMetrics,
					validationError)) {
				throw std::runtime_error("Header full validation failed: " + validationError);
			}
			headerFullValidationRan = true;

			if (checkpointMode != HeaderCheckpoint::EMode::Off) {
				const double seconds = (std::max)((uint64_t)1, validationMetrics.DurationMs) / 1000.0;
				LOG_INFO_F(
					"event=header_full_validation duration_ms={} bytes={} mb_per_s={:.3f} candidate={}",
					validationMetrics.DurationMs,
					validationMetrics.BytesRead,
					(validationMetrics.BytesRead / (1024.0 * 1024.0)) / seconds,
					candidateEntries
				);
			}
		}
	}

	if (!headerCheckpointFastAccepted &&
		(checkpointMode == HeaderCheckpoint::EMode::Write ||
		 checkpointMode == HeaderCheckpoint::EMode::Fast)) {
		HeaderCheckpointData oldCheckpoint;
		std::string readError;
		const bool hasOldCheckpoint = HeaderCheckpoint::Read(
			HeaderCheckpoint::Path(config), oldCheckpoint, readError);
		const bool oldTrusted = hasOldCheckpoint &&
			(oldCheckpoint.Trust == HeaderCheckpointData::TRUST_EXTERNAL ||
			 oldCheckpoint.Trust == HeaderCheckpointData::TRUST_DERIVED);
		const bool bootstrap = HeaderCheckpoint::BootstrapFromEnv();
		const std::string trustAnchor = HeaderCheckpoint::TrustAnchorFromEnv();
		const char* anchorManifestPath = std::getenv("GRINPP_HEADER_CKPT_TRUST_ANCHOR_MANIFEST");

		if (!headerFullValidationRan) {
			LOG_WARNING_F("event=header_checkpoint_not_written reason=no_full_validation");
		} else if (!oldTrusted && (!bootstrap || trustAnchor.empty())) {
			// A first checkpoint must have an out-of-band trust anchor. A local
			// full pass cannot prove fields that are not locally verifiable
			// (output/range/kernel roots, offset, nonce), so BOOTSTRAP=1 alone
			// must never create trust.
			LOG_WARNING_F(
				"event=header_checkpoint_not_written reason=trust_anchor_required; provide GRINPP_HEADER_CKPT_BOOTSTRAP=1 and GRINPP_HEADER_CKPT_TRUST_ANCHOR=<published aggregate>");
		} else {
			auto pChainReader = pChainStore->Read();
			auto pDbReader = pDatabase->Read();
			auto pHeaderReader = pHeaderMMR->Read();

			HeaderCheckpointData generated;
			HeaderCheckpointMetrics metrics;
			std::string checkpointError;
			if (!HeaderCheckpoint::Compute(
					config,
					*pDbReader,
					*pHeaderReader,
					*pChainReader->GetCandidateChain(),
					*pChainReader->GetConfirmedChain(),
					false,
					false,
					generated,
					metrics,
					checkpointError)) {
				throw std::runtime_error("header checkpoint generation failed: " + checkpointError);
			}

			generated.Sequence = (hasOldCheckpoint ? oldCheckpoint.Sequence : 0) + 1;
			bool writeAllowed = true;
			if (oldTrusted) {
				generated.Trust = oldCheckpoint.Trust;
				generated.TrustSource = oldCheckpoint.TrustSource;
			} else if (anchorManifestPath != nullptr && *anchorManifestPath != '\0') {
				HeaderCheckpointAnchor anchor;
				const std::string network = Env::ToString(Global::GetEnv());
				if (!HeaderCheckpoint::ParseAnchorManifest(anchorManifestPath, anchor, checkpointError)) {
					writeAllowed = false;
					LOG_WARNING_F(
						"event=header_checkpoint_not_written reason=anchor_manifest_invalid error={}",
						checkpointError);
				} else if (!HeaderCheckpoint::AnchorMatches(anchor, generated, network, checkpointError)) {
					writeAllowed = false;
					LOG_WARNING_F(
						"event=header_checkpoint_not_written reason=anchor_manifest_mismatch error={}",
						checkpointError);
				} else {
					generated.Trust = HeaderCheckpointData::TRUST_EXTERNAL;
					generated.TrustSource = HeaderCheckpointData::TRUST_EXTERNAL;
				}
			} else {
				const std::string aggregate = generated.Aggregate.ToHex();
				if (aggregate.size() != trustAnchor.size()) {
					writeAllowed = false;
				} else {
					for (size_t i = 0; i < aggregate.size(); ++i) {
						if (std::tolower(static_cast<unsigned char>(aggregate[i])) !=
							std::tolower(static_cast<unsigned char>(trustAnchor[i]))) {
							writeAllowed = false;
							break;
						}
					}
				}

				if (!writeAllowed) {
					LOG_WARNING_F(
						"event=header_checkpoint_not_written reason=trust_anchor_mismatch expected={} actual={}",
						trustAnchor,
						aggregate);
				} else {
					generated.Trust = HeaderCheckpointData::TRUST_EXTERNAL;
					generated.TrustSource = HeaderCheckpointData::TRUST_EXTERNAL;
				}
			}

			if (writeAllowed && !HeaderCheckpoint::Write(HeaderCheckpoint::Path(config), generated, checkpointError)) {
				throw std::runtime_error("header checkpoint write failed: " + checkpointError);
			}

			if (writeAllowed) {
				const double seconds = (std::max)((uint64_t)1, metrics.DurationMs) / 1000.0;
				LOG_INFO_F(
					"event=header_checkpoint_write duration_ms={} cpu_ms={} bytes={} mb_per_s={:.3f} validated=1 trust={} candidate={} confirmed={} mmr_nodes={} tip={} aggregate={}",
					metrics.DurationMs,
					metrics.CpuMs,
					metrics.BytesRead,
					(metrics.BytesRead / (1024.0 * 1024.0)) / seconds,
					(uint32_t)generated.Trust,
					generated.CandidateEntries,
					generated.ConfirmedEntries,
					generated.MmrNodes,
					generated.CandidateTip.ToHex(),
					generated.Aggregate.ToHex()
				);
			}
		}
	}
	auto pChainState = ChainState::Create(
		config,
		pChainStore,
		pDatabase,
		pHeaderMMR,
		pTransactionPool,
		pTxHashSetManager,
		genesisBlock
	);
	CoinViewStartupGuard coinViewGuard;
	Global::SetCoinView(std::make_shared<CoinView>(*pChainState));

	// G-23: if a TxHashSet is open, its three MMR roots must match the exact
	// header it was flushed at. Do not accept a state whose roots were corrupted.
	{
		ScopedTiming timing("blockchain_txhashset_root_and_nrd_check");
		auto pReader = pChainState->Read();
		auto pTxHashSet = pReader->GetTxHashSetManager()->GetTxHashSet();
		if (pTxHashSet != nullptr) {
			const BlockHeaderPtr pFlushed = pTxHashSet->GetFlushedBlockHeader();
			if (pFlushed != nullptr && !pTxHashSet->ValidateRoots(*pFlushed)) {
				throw std::runtime_error("TxHashSet MMR roots do not match its flushed header");
			}
		}

		const BlockHeaderPtr pConfirmedTip = pReader->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTxHashSet != nullptr && pConfirmedTip != nullptr) {
			if (!RebuildNrdIndexAndRequireComplete(
					const_cast<ITxHashSet*>(pTxHashSet.get()),
					pDatabase->Read().GetShared(),
					pConfirmedTip,
					"startup")) {
				// Rust grin 5.5.2 Chain::init propagates init_recent_kernel_pos_index
				// errors, so a node with a historically invalid NRD index does not
				// start. Match that instead of silently running with NRD disabled.
				throw std::runtime_error(
					"NRD recent-kernel index rebuild is incomplete; refusing to start on a txhashset state that Rust grin 5.5.2 would reject");
			}
		}
		timing.Success();
	}

	// Migrate database
	{
	ScopedTiming timing("blockchain_db_migrate");
	const uint8_t db_version = pDatabase->Read()->GetVersion();
	if (db_version > 3) {
		LOG_ERROR("Database is from a newer version of Grin++!");
		std::terminate();
	}

	if (db_version >= 1 && db_version < 3) {
		// Keep the previous database before any destructive migration. Abort if
		// the backup cannot be written: never migrate the only copy (G-22).
		// A fresh database (version 0) has nothing to preserve.
		const fs::path backupPath =
			DbBackup::BackupBeforeMigration(pDatabase, config.GetDatabasePath(), db_version);
		if (backupPath.empty()) {
			LOG_ERROR("Failed to back up the database before migration; aborting.");
			throw std::runtime_error("Database backup failed");
		}

		LOG_WARNING_F("Database v{} backed up to {}", db_version, backupPath.string());
	}

	if (db_version < 2) {
		LOG_WARNING_F("Updating chain for version {}", GRINPP_VERSION);
		pDatabase->Write()->ClearOutputPositions();

		auto pBlockDB = pDatabase->Write();
		auto pTxHashSetReader = pTxHashSetManager->Read();
		if (pTxHashSetReader->GetTxHashSet() != nullptr) {
			pTxHashSetReader->GetTxHashSet()->SaveOutputPositions(
				pChainStore->Read()->GetCandidateChain(),
				pBlockDB.GetShared()
			);
		}
		pBlockDB->SetVersion(2);
	}

	if (db_version < 3) {
		LOG_WARNING_F("Migrating blocks to v3 for Grin++ {}", GRINPP_VERSION);
		pDatabase->Write()->MigrateBlocks();
		pDatabase->Write()->SetVersion(3);
	}
	timing.Success();
	}

	// Trigger Compaction
	{
		ScopedTiming timing("blockchain_startup_block_compaction");
		auto pBatchDB = pDatabase->BatchWrite();
		pBatchDB->Compact(pChainStore->Read()->GetConfirmedChain());
		pBatchDB->Commit();

		auto pBatchTxHashSet = pTxHashSetManager->BatchWrite();
		auto pTxHashSet = pBatchTxHashSet->GetTxHashSet();
		if (pTxHashSet != nullptr) {
			const uint64_t horizon = Consensus::GetHorizonHeight(pChainState->Read()->GetHeight(EChainType::CONFIRMED));
			if (pTxHashSet->GetFlushedBlockHeader()->GetHeight() < horizon) {
				pTxHashSetManager->Write()->Close();
			} else {
				pTxHashSet->Compact();
			}

			pBatchTxHashSet->Commit();
		}
		timing.Success();
	}

	auto pBlockChain = std::shared_ptr<BlockChain>(new BlockChain(
		pTransactionPool,
		pChainState
	));
	{
		ScopedTiming timing("kernel_index_startup_ensure");
		if (!pBlockChain->EnsureKernelIndex()) {
			// The index is an optimization; get_kernel has a bounded direct-scan
			// fallback. Never prevent the node from starting because the index could
			// not be built (e.g. during first-run/genesis or a test database).
			LOG_WARNING("Kernel index is unavailable; get_kernel will use the direct scan.");
		}
		timing.Success();
	}
	coinViewGuard.Commit();
	createTiming.Success();
	pBlockChain->StartHeaderCheckpointUpdater();
	return pBlockChain;
}

void BlockChain::ResyncChain()
{
	StateWriteGuard writeGuard(*this);
	ChainResyncer(m_pChainState).ResyncChain();
}

void BlockChain::UpdateSyncStatus(SyncStatus& syncStatus) const
{
	m_pChainState->Read()->UpdateSyncStatus(syncStatus);
}

uint64_t BlockChain::GetHeight(const EChainType chainType) const
{
	return m_pChainState->Read()->GetHeight(chainType);
}

uint64_t BlockChain::GetTotalDifficulty(const EChainType chainType) const
{
	return m_pChainState->Read()->GetTotalDifficulty(chainType);
}

EBlockChainStatus BlockChain::AddBlock(const FullBlock& block)
{
	StateWriteGuard writeGuard(*this);
	BlockProcessor processor(m_pChainState);
	const EBlockChainStatus status = processor.ProcessBlock(block);
	if (status == EBlockChainStatus::SUCCESS && processor.KernelIndexNeedsRebuild()) {
		ScopedTiming timing("kernel_index_rebuild_after_block");
		LOG_WARNING("Kernel index lagged behind a committed block; running the verified index rebuild once.");
		try {
			if (!EnsureKernelIndexLocked()) {
				LOG_WARNING("Verified kernel index rebuild failed; get_kernel will use the direct scan.");
			}
		} catch (const std::exception& e) {
			LOG_WARNING_F("Verified kernel index rebuild threw: {}; get_kernel will use the direct scan.", e.what());
		}
		timing.Success();
	}
	if (status == EBlockChainStatus::SUCCESS || status == EBlockChainStatus::ORPHANED) NotifyHeaderCheckpointChanged();
	return status;
}

EBlockChainStatus BlockChain::AddCompactBlock(const CompactBlock& compactBlock)
{
	const Hash& hash = compactBlock.GetHash();
	const uint64_t height = compactBlock.GetHeight();

	{
		auto pReader = m_pChainState->Read();
		auto pConfirmed = pReader->GetChainStore()->GetConfirmedChain()->GetByHeight(height);
		if (pConfirmed != nullptr && pConfirmed->GetHash() == hash)
		{
			return EBlockChainStatus::ALREADY_EXISTS;
		}
	}

	try
	{
		std::unique_ptr<FullBlock> pHydratedBlock = BlockHydrator(m_pTransactionPool).Hydrate(compactBlock);
		if (pHydratedBlock != nullptr)
		{
			const EBlockChainStatus added = AddBlock(*pHydratedBlock);
			if (added == EBlockChainStatus::INVALID)
			{
				return EBlockChainStatus::TRANSACTIONS_MISSING;
			}

			return added;
		}
	}
	catch (std::exception& e)
	{
		LOG_WARNING_F("Exception thrown: {}", e.what());
	}

	return EBlockChainStatus::TRANSACTIONS_MISSING;
}

fs::path BlockChain::SnapshotTxHashSet(BlockHeaderPtr pBlockHeader)
{
	// A consistent view of the chain stores is required; this batch is read-only
	// and deliberately not committed.
	auto pBatch = m_pChainState->BatchWrite(); // DO NOT COMMIT THIS BATCH
	const uint64_t horizon = Consensus::GetHorizonHeight(pBatch->GetHeight(EChainType::CONFIRMED));
	if (pBlockHeader->GetHeight() < horizon)
	{
		throw BAD_DATA_EXCEPTION(EBanReason::Abusive, "TxHashSet snapshot requested beyond horizon.");
	}

	return pBatch->GetTxHashSetManager()->SaveSnapshot(pBatch->GetBlockDB(), pBlockHeader);
}

bool BlockChain::AreSpentPositionsComplete(
	const Hash& blockHash,
	const std::vector<Commitment>& inputs,
	size_t& mappedCount) const
{
	const auto pBlockDB = m_pChainState->Read()->GetBlockDB();
	const std::unordered_map<Commitment, OutputLocation> spent =
		pBlockDB->GetSpentPositions(blockHash);
	mappedCount = spent.size();

	for (const Commitment& commitment : inputs) {
		if (spent.find(commitment) == spent.end()) {
			return false;
		}
	}

	return true;
}

EBlockChainStatus BlockChain::ProcessTransactionHashSet(const Hash& blockHash, const fs::path& path, SyncStatus& syncStatus)
{
	StateWriteGuard writeGuard(*this);
	TxHashSetProcessor processor(Global::GetConfig(), *this, m_pChainState);
	try
	{
		const bool success = processor.ProcessTxHashSet(blockHash, path, syncStatus);
		if (success)
		{
			{
				auto pReader = m_pChainState->Read();
				auto pTxHashSet = pReader->GetTxHashSetManager()->GetTxHashSet();
				const BlockHeaderPtr pTip = pReader->GetTipBlockHeader(EChainType::CONFIRMED);
				if (pTxHashSet != nullptr && pTip != nullptr) {
					// Defense-in-depth only: the processor already rebuilds and checks
					// the NRD index before committing. Never report SUCCESS for a state
					// whose NRD history cannot be rebuilt.
					if (!RebuildNrdIndexAndRequireComplete(
							const_cast<ITxHashSet*>(pTxHashSet.get()),
							pReader->GetBlockDB().GetShared(),
							pTip,
							"txhashset install")) {
						return EBlockChainStatus::STORE_ERROR;
					}
				}
			}
			// A PIBD install can jump the confirmed tip by millions of blocks. Build
			// the kernel index once here, while the chain and txhashset are already
			// quiescent under StateWriteGuard, so the next restart can extend it
			// instead of running the full rebuild on the startup critical path.
			ScopedTiming indexTiming("kernel_index_after_txhashset_install");
			try {
				if (!EnsureKernelIndexLocked()) {
					LOG_WARNING("Verified kernel index build after TxHashSet install failed; get_kernel will use the direct scan.");
				}
			} catch (const std::exception& e) {
				LOG_WARNING_F("Kernel index build after TxHashSet install threw: {}; index stays unavailable for this state.", e.what());
			}
			indexTiming.Success();
			return EBlockChainStatus::SUCCESS;
		}
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Failed to process TxHashSet: {}", e.what());
	}

	if (processor.GetLastFailureKind() == TxHashSetProcessor::EFailureKind::LocalUnverifiable) {
		LOG_WARNING("TxHashSet was refused because the state cannot be verified locally; not treating the peer as malicious.");
		return EBlockChainStatus::STORE_ERROR;
	}

	return EBlockChainStatus::INVALID;
}

bool BlockChain::EnsureKernelIndex()
{
	StateWriteGuard writeGuard(*this);
	return EnsureKernelIndexLocked();
}

bool BlockChain::EnsureKernelIndexLocked()
{
	std::shared_ptr<IBlockDB> pBlockDB;
	BlockHeaderPtr pTip;
	std::shared_ptr<ITxHashSet> pTxHashSet;
	std::shared_ptr<const Chain> pConfirmedChain;

	{
		auto pState = m_pChainState->Write();
		pTip = pState->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTip == nullptr) {
			return false;
		}
		pBlockDB = pState->GetBlockDB();
		pTxHashSet = pState->GetTxHashSetManager()->GetTxHashSet();
		if (pTxHashSet == nullptr) {
			return false;
		}
		pConfirmedChain = pState->GetChainStore()->GetConfirmedChain();

		auto getHeader = [&pBlockDB, &pConfirmedChain](const uint64_t height) -> BlockHeaderPtr {
			const auto pIndex = pConfirmedChain->GetByHeight(height);
			if (pIndex == nullptr) {
				return nullptr;
			}
			return pBlockDB->GetBlockHeader(pIndex->GetHash());
		};

		return KernelIndex::Ensure(pBlockDB, pTxHashSet, pTip, getHeader);
	}
}
IBlockChain::ApiStateSnapshot BlockChain::GetApiStateSnapshot() const
{
	// Hold the mutation lock for as long as the caller keeps the returned
	// lease. The snapshot therefore references a txhashset that cannot be
	// replaced or mutated while a serving/API response is being assembled.
	std::unique_lock<std::mutex> lock(m_stateWriteMutex);
	const uint64_t sequence = m_stateSequence.load(std::memory_order_acquire);
	if ((sequence & 1) != 0) {
		return {};
	}

	auto pReader = m_pChainState->Read();
	const BlockHeaderPtr pTip = pReader->GetTipBlockHeader(EChainType::CONFIRMED);
	std::shared_ptr<ITxHashSet> pTxHashSet = std::const_pointer_cast<ITxHashSet>(
		pReader->GetTxHashSetManager()->GetTxHashSet());

	if (pTip == nullptr || pTxHashSet == nullptr) {
		return {};
	}

	ApiStateSnapshot snapshot;
	snapshot.valid = true;
	snapshot.sequence = sequence;
	snapshot.tip = pTip;
	snapshot.txHashSet = pTxHashSet;
	snapshot.lease = std::make_shared<StateReadLease>(std::move(lock));
	return snapshot;
}

bool BlockChain::IsApiStateCurrent(const uint64_t sequence) const
{
	const uint64_t current = m_stateSequence.load(std::memory_order_acquire);
	return current == sequence && (current & 1) == 0;
}
std::shared_ptr<ITxHashSet> BlockChain::GetTxHashSet()
{
	return std::const_pointer_cast<ITxHashSet>(m_pChainState->Read()->GetTxHashSetManager()->GetTxHashSet());
}
void BlockChain::CloseTxHashSet()
{
	StateWriteGuard writeGuard(*this);
	m_pChainState->Write()->GetTxHashSetManager()->Close();
}

void BlockChain::OpenTxHashSet()
{
	StateWriteGuard writeGuard(*this);
	const BlockHeaderPtr pTip = GetTipBlockHeader(EChainType::CONFIRMED);
	if (pTip != nullptr) {
		m_pChainState->Write()->GetTxHashSetManager()->Open(pTip);
	}
}
EBlockChainStatus BlockChain::ProcessExistingTxHashSet(const Hash& blockHash, SyncStatus& syncStatus)
{
	StateWriteGuard writeGuard(*this);
	TxHashSetProcessor processor(Global::GetConfig(), *this, m_pChainState);
	try {
		const bool success = processor.ProcessExistingTxHashSet(blockHash, syncStatus);
		if (success) {
			{
				auto pReader = m_pChainState->Read();
				auto pTxHashSet = pReader->GetTxHashSetManager()->GetTxHashSet();
				const BlockHeaderPtr pTip = pReader->GetTipBlockHeader(EChainType::CONFIRMED);
				if (pTxHashSet != nullptr && pTip != nullptr) {
					// Defense-in-depth only; the processor checks before committing.
					if (!RebuildNrdIndexAndRequireComplete(
							const_cast<ITxHashSet*>(pTxHashSet.get()),
							pReader->GetBlockDB().GetShared(),
							pTip,
							"PIBD txhashset install")) {
						return EBlockChainStatus::STORE_ERROR;
					}
				}
			}
			ScopedTiming indexTiming("kernel_index_after_existing_txhashset");
			try {
				if (!EnsureKernelIndexLocked()) {
					LOG_WARNING("Verified kernel index build after existing TxHashSet activation failed; get_kernel will use the direct scan.");
				}
			} catch (const std::exception& e) {
				LOG_WARNING_F("Kernel index build after existing TxHashSet activation threw: {}; index stays unavailable for this state.", e.what());
			}
			indexTiming.Success();
			return EBlockChainStatus::SUCCESS;
		}
	}
	catch (std::exception& e) {
		LOG_ERROR_F("Failed to process the existing TxHashSet: {}", e.what());
	}

	if (processor.GetLastFailureKind() == TxHashSetProcessor::EFailureKind::LocalUnverifiable) {
		LOG_WARNING("Activated TxHashSet was refused because the state cannot be verified locally; not treating the peer as malicious.");
		return EBlockChainStatus::STORE_ERROR;
	}

	return EBlockChainStatus::INVALID;
}
EBlockChainStatus BlockChain::AddTransaction(TransactionPtr pTransaction, const EPoolType poolType)
{
	try
	{
		auto pReader = m_pChainState->Read();
		auto pLastConfimedHeader = pReader->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pLastConfimedHeader != nullptr)
		{
			const EAddTransactionStatus status = m_pTransactionPool->AddTransaction(
				pReader->GetBlockDB().GetShared(),
				pReader->GetTxHashSetManager()->GetTxHashSet(),
				pTransaction,
				poolType,
				*pLastConfimedHeader
			);
			if (status == EAddTransactionStatus::ADDED)
			{
				return EBlockChainStatus::SUCCESS;
			}
			else if (status == EAddTransactionStatus::TX_INVALID)
			{
				return EBlockChainStatus::INVALID;
			}
			else
			{
				return EBlockChainStatus::UNKNOWN_ERROR;
			}
		}
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Exception thrown: {}", e.what());
	}

	return EBlockChainStatus::UNKNOWN_ERROR;
}

TransactionPtr BlockChain::GetTransactionByKernelHash(const Hash& kernelHash) const
{
	return m_pTransactionPool->FindTransactionByKernelHash(kernelHash);
}

EBlockChainStatus BlockChain::AddBlockHeader(BlockHeaderPtr pBlockHeader)
{
	try
	{
		const EBlockChainStatus status = BlockHeaderProcessor(m_pChainState).ProcessSingleHeader(pBlockHeader);
		if (status == EBlockChainStatus::SUCCESS || status == EBlockChainStatus::ORPHANED) NotifyHeaderCheckpointChanged();
		return status;
	}
	catch (BadDataException& e)
	{
		throw e;
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Invalid header {} - Exception: ", *pBlockHeader, e.what());
		return EBlockChainStatus::INVALID;
	}
}

EBlockChainStatus BlockChain::AddBlockHeaders(const std::vector<BlockHeaderPtr>& blockHeaders)
{
	try
	{
		const EBlockChainStatus status = BlockHeaderProcessor(m_pChainState).ProcessSyncHeaders(blockHeaders);
		if (status == EBlockChainStatus::SUCCESS || status == EBlockChainStatus::ORPHANED) NotifyHeaderCheckpointChanged();
		return status;
	}
	catch (BadDataException& e)
	{
		throw e;
	}
	catch (std::exception&)
	{
		return EBlockChainStatus::UNKNOWN_ERROR;
	}
}

std::vector<BlockHeaderPtr> BlockChain::GetBlockHeadersByHash(const std::vector<CBigInteger<32>>& hashes) const
{
	auto pReader = m_pChainState->Read();

	std::vector<BlockHeaderPtr> headers;
	for (const CBigInteger<32>& hash : hashes)
	{
		BlockHeaderPtr pHeader = pReader->GetBlockHeaderByHash(hash);
		if (pHeader != nullptr)
		{
			headers.push_back(pHeader);
		}
	}

	return headers;
}

BlockHeaderPtr BlockChain::GetBlockHeaderByHeight(const uint64_t height, const EChainType chainType) const
{
	return m_pChainState->Read()->GetBlockHeaderByHeight(height, chainType);
}

BlockHeaderPtr BlockChain::GetBlockHeaderByHash(const CBigInteger<32>& hash) const
{
	return m_pChainState->Read()->GetBlockHeaderByHash(hash);
}

BlockHeaderPtr BlockChain::GetBlockHeaderByCommitment(const Commitment& outputCommitment) const
{
	return m_pChainState->Read()->GetBlockHeaderByCommitment(outputCommitment);
}

BlockHeaderPtr BlockChain::GetTipBlockHeader(const EChainType chainType) const
{
	return m_pChainState->Read()->GetTipBlockHeader(chainType);
}

std::unique_ptr<CompactBlock> BlockChain::GetCompactBlockByHash(const Hash& hash) const
{
	std::unique_ptr<FullBlock> pBlock = m_pChainState->Read()->GetBlockByHash(hash);
	if (pBlock != nullptr)
	{
		return std::make_unique<CompactBlock>(CompactBlockFactory::CreateCompactBlock(*pBlock));
	}

	return std::unique_ptr<CompactBlock>(nullptr);
}

std::unique_ptr<FullBlock> BlockChain::GetBlockByCommitment(const Commitment& outputCommitment) const
{
	auto pHeader = m_pChainState->Read()->GetBlockHeaderByCommitment(outputCommitment);
	if (pHeader != nullptr)
	{
		return GetBlockByHash(pHeader->GetHash());
	}

	return std::unique_ptr<FullBlock>(nullptr);
}

std::unique_ptr<FullBlock> BlockChain::GetBlockByHash(const Hash& hash) const
{
	return m_pChainState->Read()->GetBlockByHash(hash);
}

std::unique_ptr<FullBlock> BlockChain::GetBlockByHeight(const uint64_t height) const
{
	return m_pChainState->Read()->GetBlockByHeight(height);
}

std::vector<BlockWithOutputs> BlockChain::GetOutputsByHeight(const uint64_t startHeight, const uint64_t maxHeight) const
{
	auto pChainStateReader = m_pChainState->Read();
	const uint64_t highestHeight = (std::min)(pChainStateReader->GetHeight(EChainType::CONFIRMED), maxHeight);

	std::vector<BlockWithOutputs> blocksWithOutputs;
	if (startHeight > highestHeight) return blocksWithOutputs;
	blocksWithOutputs.reserve(highestHeight - startHeight + 1);

	uint64_t height = startHeight;
	while (height <= highestHeight)
	{
		std::unique_ptr<BlockWithOutputs> pBlockWithOutputs = pChainStateReader->GetBlockWithOutputs(height);
		if (pBlockWithOutputs != nullptr)
		{
			blocksWithOutputs.push_back(*pBlockWithOutputs);
		}

		++height;
	}

	return blocksWithOutputs;
}

bool BlockChain::HasBlock(const uint64_t height, const Hash& hash) const
{
	auto pChainStateReader = m_pChainState->Read();
	
	auto pIndex = pChainStateReader->GetChainStore()->GetConfirmedChain()->GetByHeight(height);

	return pIndex != nullptr && pIndex->GetHash() == hash;
}

std::vector<std::pair<uint64_t, Hash>> BlockChain::GetBlocksNeeded(const uint64_t maxNumBlocks) const
{
	return m_pChainState->Read()->GetBlocksNeeded(maxNumBlocks);
}

bool BlockChain::ProcessNextOrphanBlock()
{
	// This is a chain-state mutation like AddBlock: serialize it with other
	// mutations and repair the optimization index before returning. Orphan-block
	// processing previously called BlockProcessor directly, so a rejected
	// AppendBlock/RewindToAncestor could commit the block and leave the index
	// stale until the next restart.
	StateWriteGuard writeGuard(*this);

	BlockHeaderPtr pNextHeader = nullptr;
	std::shared_ptr<const FullBlock> pOrphanBlock = nullptr;

	{
		auto pReader = m_pChainState->Read();
		const uint64_t height = pReader->GetHeight(EChainType::CONFIRMED) + 1;
		pNextHeader = pReader->GetBlockHeaderByHeight(height, EChainType::CANDIDATE);
		if (pNextHeader == nullptr)
		{
			return false;
		}

		pOrphanBlock = pReader->GetOrphanBlock(height, pNextHeader->GetHash());
		if (pOrphanBlock == nullptr)
		{
			return false;
		}
	}

	try
	{
		BlockProcessor processor(m_pChainState);
		const EBlockChainStatus status = processor.ProcessBlock(*pOrphanBlock);
		if (status == EBlockChainStatus::SUCCESS && processor.KernelIndexNeedsRebuild()) {
			ScopedTiming timing("kernel_index_rebuild_after_orphan");
			LOG_WARNING("Kernel index lagged behind a committed orphan block; running the verified index rebuild once.");
			try {
				if (!EnsureKernelIndexLocked()) {
					LOG_WARNING("Verified kernel index rebuild after orphan block failed; get_kernel will use the direct scan.");
				}
			} catch (const std::exception& e) {
				LOG_WARNING_F("Verified kernel index rebuild after orphan block threw: {}; get_kernel will use the direct scan.", e.what());
			}
			timing.Success();
		}
		return status == EBlockChainStatus::SUCCESS;
	}
	catch (std::exception&)
	{
		m_pChainState->
			Write()->
			GetOrphanPool()->
			RemoveOrphan(pNextHeader->GetHeight(), pNextHeader->GetHash());
		return false;
	}
}

bool BlockChain::HasOrphan(const Hash& blockHash) const
{
	auto pReader = m_pChainState->Read();
	auto pHeader = pReader->GetBlockHeaderByHash(blockHash);
	if (pHeader == nullptr) {
		return false;
	}

	return pReader->GetOrphanBlock(pHeader->GetHeight(), blockHash) != nullptr;
}

namespace BlockChainAPI
{
	BLOCK_CHAIN_API std::shared_ptr<IBlockChain> OpenBlockChain(
		const Config& config,
		std::shared_ptr<Locked<IBlockDB>> pDatabase,
		std::shared_ptr<Locked<TxHashSetManager>> pTxHashSetManager,
		std::shared_ptr<ITransactionPool> pTransactionPool,
		std::shared_ptr<Locked<IHeaderMMR>> pHeaderMMR)
	{
		// Startup recovery validates and repairs the chain stores before this point
		// (E48); unrecoverable corruption is fatal by design instead of silently
		// deleting user data.
		return BlockChain::Create(config, pDatabase, pTxHashSetManager, pTransactionPool, pHeaderMMR);
	}
}




