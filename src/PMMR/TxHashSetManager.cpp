#include <PMMR/TxHashSetManager.h>

#include "TxHashSetImpl.h"
#include "Zip/TxHashSetZip.h"
#include "Zip/Zipper.h"
#include <PMMR/TxHashSetStaging.h>

#include <Common/Util/FileUtil.h>
#include <Common/Util/StringUtil.h>
#include <Core/File/FileRemover.h>
#include <Common/Logger.h>
#include <Core/File/BitmapFile.h>

#include <filesystem.h>
#include <stdexcept>
#include <future>

TxHashSetManager::TxHashSetManager(const Config& config)
	: m_config(config), m_pTxHashSet(nullptr)
{
	// Recovery needs the committed chain tip, which is not available here.
}

std::shared_ptr<ITxHashSet> TxHashSetManager::Open(const BlockHeaderPtr& pConfirmedTip, bool recover)
{
	Close();
	if (recover && pConfirmedTip != nullptr) {
		TxHashSetStaging::Recover(m_config.GetTxHashSetPath(), pConfirmedTip->GetHash().ToHex());
	}

	const fs::path txHashSetPath = m_config.GetTxHashSetPath();
	auto kernelFuture = std::async(std::launch::async,
		[txHashSetPath] { return KernelMMR::Load(txHashSetPath); });
	auto outputFuture = std::async(std::launch::async,
		[txHashSetPath] { return OutputPMMR::Load(txHashSetPath); });
	auto rangeProofFuture = std::async(std::launch::async,
		[txHashSetPath] { return RangeProofPMMR::Load(txHashSetPath); });

	std::shared_ptr<KernelMMR> pKernelMMR = kernelFuture.get();
	std::shared_ptr<OutputPMMR> pOutputPMMR = outputFuture.get();
	std::shared_ptr<RangeProofPMMR> pRangeProofPMMR = rangeProofFuture.get();

	m_pTxHashSet = std::shared_ptr<TxHashSet>(new TxHashSet(pKernelMMR, pOutputPMMR, pRangeProofPMMR, pConfirmedTip));

	return m_pTxHashSet;
}

std::shared_ptr<ITxHashSet> TxHashSetManager::LoadFromZip(
	const Config& config,
	const fs::path& zipFilePath,
	BlockHeaderPtr pHeader,
	const fs::path& targetRoot)
{
	FileRemover fileRemover(zipFilePath);

	const fs::path txHashSetPath = targetRoot.empty() ? config.GetTxHashSetPath() : targetRoot;
	const TxHashSetZip zip(config);

	try
	{
		if (zip.Extract(zipFilePath, *pHeader, txHashSetPath))
		{
			LOG_INFO_F("{} extracted successfully", zipFilePath);

			LOG_INFO_F(
				"Loading extracted TxHashSet for header {} at height {}: kernels={}, outputs={}",
				pHeader->GetHash(),
				pHeader->GetHeight(),
				pHeader->GetNumKernels(),
				pHeader->GetNumOutputs());

			// Rewind Kernel MMR
			std::shared_ptr<KernelMMR> pKernelMMR = KernelMMR::Load(txHashSetPath);
			const uint64_t kernelMmrSize = pKernelMMR->GetSize();
			try {
				pKernelMMR->Rewind(pHeader->GetNumKernels());
				pKernelMMR->Commit();
			}
			catch (const std::exception& e) {
				throw std::runtime_error(StringUtil::Format(
					"Kernel MMR rewind failed: target kernels={}, actual MMR size={} ({}); header={} height={}",
					pHeader->GetNumKernels(), kernelMmrSize, e.what(),
					pHeader->GetHash(), pHeader->GetHeight()));
			}

			// Rewind Output MMR
			std::shared_ptr<OutputPMMR> pOutputPMMR = OutputPMMR::Load(txHashSetPath);
			const uint64_t outputMmrSize = pOutputPMMR->GetSize();
			try {
				pOutputPMMR->Rewind(pHeader->GetNumOutputs(), {});
				pOutputPMMR->Commit();
			}
			catch (const std::exception& e) {
				throw std::runtime_error(StringUtil::Format(
					"Output MMR rewind failed: target outputs={}, actual MMR size={} ({}); header={} height={}",
					pHeader->GetNumOutputs(), outputMmrSize, e.what(),
					pHeader->GetHash(), pHeader->GetHeight()));
			}

			// Rewind RangeProof MMR
			std::shared_ptr<RangeProofPMMR> pRangeProofPMMR = RangeProofPMMR::Load(txHashSetPath);
			const uint64_t rangeProofMmrSize = pRangeProofPMMR->GetSize();
			try {
				pRangeProofPMMR->Rewind(pHeader->GetNumOutputs(), {});
				pRangeProofPMMR->Commit();
			}
			catch (const std::exception& e) {
				throw std::runtime_error(StringUtil::Format(
					"RangeProof MMR rewind failed: target outputs={}, actual MMR size={} ({}); header={} height={}",
					pHeader->GetNumOutputs(), rangeProofMmrSize, e.what(),
					pHeader->GetHash(), pHeader->GetHeight()));
			}

			return std::shared_ptr<TxHashSet>(new TxHashSet(pKernelMMR, pOutputPMMR, pRangeProofPMMR, pHeader));
		}
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Failed to load: {}", e.what());

		// Preserve up to four failed archives without ever deleting an earlier
		// one. Bounded slots prevent repeated invalid archives from filling disk.
		std::error_code error;
		if (fs::exists(zipFilePath, error)) {
			fs::path failedPath;
			for (uint64_t suffix = 0; suffix < 4; ++suffix) {
				failedPath = fs::temp_directory_path() /
					("txhashset_failed.slot" + std::to_string(suffix) + ".zip");
				if (!fs::exists(failedPath)) {
					break;
				}
				failedPath.clear();
			}
			if (failedPath.empty()) {
				LOG_WARNING("All four failed TxHashSet archive slots are occupied; preserving existing evidence.");
			} else {
				error.clear();
				fs::rename(zipFilePath, failedPath, error);
			}
			if (!failedPath.empty() && !error) {
				LOG_ERROR_F(
					"Preserved failed TxHashSet archive {} ({} bytes) for header {} height {}.",
					failedPath,
					FileUtil::GetFileSize(failedPath),
					pHeader != nullptr ? pHeader->GetHash() : ZERO_HASH,
					pHeader != nullptr ? pHeader->GetHeight() : 0);
			} else if (!failedPath.empty()) {
				LOG_ERROR_F("Could not preserve failed TxHashSet archive: {}", error.message());
			}
		}
	}

	return nullptr;
}

fs::path TxHashSetManager::SaveSnapshot(std::shared_ptr<IBlockDB> pBlockDB, BlockHeaderPtr pHeader) const
{
	if (m_pTxHashSet == nullptr)
	{
		throw std::exception();
	}

	fs::path snapshotDir = fs::temp_directory_path() / "Snapshots" / pHeader->ShortHash();
	FileRemover snapshotRemover(snapshotDir);

	const std::string fileName = StringUtil::Format("TxHashSet.{}.zip", pHeader->ShortHash());
	fs::path zipFilePath = fs::temp_directory_path() / "Snapshots" / fileName;

	try
	{
		BlockHeaderPtr pFlushedHeader = nullptr;

		{
			// Copy the flushed state into the per-height snapshot directory. A
			// caller that already has this snapshot reuses it instead of copying.
			FileUtil::CopyDirectory(m_config.GetTxHashSetPath(), snapshotDir);

			pFlushedHeader = m_pTxHashSet->GetFlushedBlockHeader();
			LOG_INFO_F(
				"TxHashSet snapshot: flushed height {}, requested height {}.",
				pFlushedHeader != nullptr ? pFlushedHeader->GetHeight() : 0,
				pHeader->GetHeight());
		}

		{
			// Load Snapshot TxHashSet
			auto pKernelMMR = KernelMMR::Load(snapshotDir);
			auto pOutputPMMR = OutputPMMR::Load(snapshotDir);
			auto pRangeProofPMMR = RangeProofPMMR::Load(snapshotDir);
			TxHashSet snapshotTxHashSet(pKernelMMR, pOutputPMMR, pRangeProofPMMR, pFlushedHeader);

			// Rewind Snapshot TxHashSet
			snapshotTxHashSet.Rewind(pBlockDB, *pHeader);

			// Flush Snapshot TxHashSet
			snapshotTxHashSet.Commit();
		}

		// PIBD activation writes the modern pmmr_leafset.bin; the legacy zip
		// format (and legacy clients) expect pmmr_leaf.bin. Materialize the
		// Roaring bitmap before renaming.
		const auto materializeLegacyLeaf = [](const fs::path& dir) {
			const fs::path leafPath = dir / "pmmr_leaf.bin";
			const fs::path leafSetPath = dir / "pmmr_leafset.bin";
			if (fs::exists(leafPath) || !fs::exists(leafSetPath)) {
				return;
			}

			auto pLeafSet = BitmapFile::Load(leafSetPath);
			Roaring bitmap = pLeafSet->ToRoaring();
			const size_t numBytes = bitmap.getSizeInBytes();
			std::vector<uint8_t> bytes(numBytes);
			const size_t written = bitmap.write((char*)bytes.data());
			if (written != numBytes) {
				throw FILE_EXCEPTION("Failed to serialize pmmr_leaf.bin");
			}

			FileUtil::SafeWriteToFile(leafPath, bytes);
		};

		materializeLegacyLeaf(snapshotDir / "output");
		materializeLegacyLeaf(snapshotDir / "rangeproof");

		// Rename pmmr_leaf files
		const std::string newFileName = StringUtil::Format("pmmr_leaf.bin.{}", pHeader->ShortHash());
		FileUtil::RenameFile(
			snapshotDir / "output" / "pmmr_leaf.bin",
			snapshotDir / "output" / newFileName
		);

		FileUtil::RenameFile(
			snapshotDir / "rangeproof" / "pmmr_leaf.bin",
			snapshotDir / "rangeproof" / newFileName
		);

		// Create Zip
		const std::vector<fs::path> pathsToZip = {
			snapshotDir / "kernel",
			snapshotDir / "output",
			snapshotDir / "rangeproof"
		};
		Zipper::CreateZipFile(zipFilePath, pathsToZip);
	}
	catch (...)
	{
		FileUtil::RemoveFile(zipFilePath);
		throw;
	}

	return zipFilePath;
}
