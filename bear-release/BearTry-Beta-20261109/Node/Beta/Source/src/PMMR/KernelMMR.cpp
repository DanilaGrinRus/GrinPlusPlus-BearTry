#include "KernelMMR.h"
#include "Common/MMRUtil.h"
#include "Common/MMRHashUtil.h"

#include <Core/Global.h>
#include <Core/Exceptions/FileException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Common/Util/StringUtil.h>
#include <Common/Logger.h>

#include <limits>
#include <algorithm>

KernelMMR::KernelMMR(
	std::shared_ptr<HashFile> pHashFile,
	std::shared_ptr<AppendOnlyFile> pDataFile,
	const bool fixedV1)
	: m_pHashFile(pHashFile),
	m_pDataFile(pDataFile),
	m_fixedV1(fixedV1)
{
}

std::vector<KernelMMR::KernelEntry> KernelMMR::ParseVariableEntries(
	const std::shared_ptr<AppendOnlyFile>& pDataFile)
{
	const uint64_t dataSize = pDataFile->GetSize();
	constexpr uint64_t WINDOW_SIZE = 4ULL * 1024 * 1024;
	std::vector<unsigned char> window;
	uint64_t windowStart = 0;
	std::vector<KernelEntry> entries;
	uint64_t offset = 0;
	while (offset < dataSize)
	{
		if (window.empty() || offset < windowStart ||
			offset - windowStart >= window.size()) {
			windowStart = offset;
			const uint64_t readSize = (std::min)(WINDOW_SIZE, dataSize - offset);
			if (!pDataFile->Read(offset, readSize, window) || window.size() != readSize) {
				throw FILE_EXCEPTION("Kernel MMR: failed to read variable-size data");
			}
		}
		const uint8_t feature = window[static_cast<size_t>(offset - windowStart)];
		uint16_t size = 0;
		switch (feature)
		{
			case 0: size = 106; break; // Plain (v2 fee fields)
			case 1: size = 98;  break; // Coinbase
			case 2: size = 114; break; // HeightLocked
			case 3: size = 108; break; // NoRecentDuplicate
			default:
				throw FILE_EXCEPTION(StringUtil::Format(
					"Kernel MMR: unknown kernel feature {} at data offset {}", feature, offset));
		}

		if (offset + size > dataSize)
		{
			throw FILE_EXCEPTION(StringUtil::Format(
				"Kernel MMR: truncated kernel record at data offset {}", offset));
		}

		entries.push_back({ offset, size });
		offset += size;
	}

	return entries;
}

std::shared_ptr<KernelMMR> KernelMMR::Load(const fs::path& txHashSetPath)
{
	auto pHashFile = HashFile::Load(txHashSetPath / "kernel" / "pmmr_hash.bin");
	auto pDataFile = std::make_shared<AppendOnlyFile>(txHashSetPath / "kernel" / "pmmr_data.bin");
	pDataFile->Load();

	const uint64_t dataSize = pDataFile->GetSize();
	// File length alone is ambiguous: a valid sequence of variable-size kernels
	// can also be divisible by 114. The hash PMMR gives the expected leaf count.
	// If both counts match, every v2 record is 114 bytes (height-locked) and has
	// the same byte layout as its v1 form, so the fixed path is safe and faster.
	const uint64_t expectedKernels = MMRUtil::GetNumLeaves(pHashFile->GetSize());
	const bool fixedCountMatches = dataSize % KERNEL_SIZE == 0 &&
		dataSize / KERNEL_SIZE == expectedKernels;
	std::vector<KernelEntry> variableEntries;
	bool variableCountMatches = false;
	if (!fixedCountMatches) {
		try {
			variableEntries = ParseVariableEntries(pDataFile);
			variableCountMatches = variableEntries.size() == expectedKernels;
		} catch (const std::exception&) {
			// A legacy fixed-width record need not be valid v2 serialization.
		}
	}
	if (!fixedCountMatches && !variableCountMatches) {
		throw FILE_EXCEPTION(StringUtil::Format(
			"Kernel MMR: data size {} does not match {} hash leaves", dataSize, expectedKernels));
	}
	const bool fixedV1 = fixedCountMatches;
	auto pKernelMMR = std::make_shared<KernelMMR>(pHashFile, pDataFile, fixedV1);

	if (dataSize > 0)
	{
		if (fixedV1)
		{
			const uint64_t count = dataSize / KERNEL_SIZE;
			pKernelMMR->m_entries.reserve(count);
			for (uint64_t i = 0; i < count; i++)
			{
				pKernelMMR->m_entries.push_back({ i * KERNEL_SIZE, KERNEL_SIZE });
			}
		}
		else
		{
			pKernelMMR->m_entries = std::move(variableEntries);
			LOG_INFO_F(
				"Kernel MMR: loaded {} variable-size Rust v2 kernels from {} bytes",
				pKernelMMR->m_entries.size(), dataSize);
		}
	}

	if (pHashFile->GetSize() == 0)
	{
		pKernelMMR->ApplyKernel(Global::GetGenesisBlock().GetKernels().front());
	}

	pKernelMMR->m_committedEntries = pKernelMMR->m_entries.size();
	return pKernelMMR;
}

Hash KernelMMR::Root(const uint64_t size) const
{
	return MMRHashUtil::Root(m_pHashFile, size, nullptr);
}

std::unique_ptr<TransactionKernel> KernelMMR::GetKernelAt(const LeafIndex& leaf_idx) const
{
	if (leaf_idx.Get() >= m_entries.size())
	{
		return nullptr;
	}

	const KernelEntry& entry = m_entries[leaf_idx.Get()];
	std::vector<unsigned char> data;
	if (!m_pDataFile->Read(entry.Offset, entry.Size, data) || data.size() != entry.Size)
	{
		return nullptr;
	}

	ByteBuffer byteBuffer(
		std::move(data),
		m_fixedV1 ? EProtocolVersion::V1 : EProtocolVersion::V2);
	return std::make_unique<TransactionKernel>(TransactionKernel::Deserialize(byteBuffer));
}

std::vector<Hash> KernelMMR::GetLastLeafHashes(const uint64_t numHashes) const
{
	return MMRHashUtil::GetLastLeafHashes(m_pHashFile, nullptr, nullptr, numHashes);
}

bool KernelMMR::Rewind(const uint64_t num_kernels)
{
	if (num_kernels > m_entries.size())
	{
		throw FILE_EXCEPTION(StringUtil::Format(
			"Kernel MMR: rewind target {} exceeds {} entries", num_kernels, m_entries.size()));
	}

	const uint64_t endOffset = num_kernels == 0
		? 0
		: m_entries[num_kernels - 1].Offset + m_entries[num_kernels - 1].Size;

	const size_t retained = m_committedEntries - m_rewoundEntries.size();
	if (num_kernels < retained) {
		m_rewoundEntries.reserve(m_committedEntries - num_kernels);
		for (size_t i = retained; i > num_kernels; --i) {
			m_rewoundEntries.push_back(m_entries[i - 1]);
		}
	}
	m_pHashFile->Rewind(LeafIndex::At(num_kernels).GetPosition());
	if (!m_pDataFile->Rewind(endOffset))
	{
		throw FILE_EXCEPTION(StringUtil::Format(
			"Kernel MMR: failed to rewind data to byte {}", endOffset));
	}

	m_entries.resize(num_kernels);
	return true;
}

void KernelMMR::Commit()
{
	m_pHashFile->Commit();
	if (!m_pDataFile->Flush())
	{
		throw FILE_EXCEPTION("Kernel MMR data commit failed");
	}

	m_committedEntries = m_entries.size();
	m_rewoundEntries.clear();
}

void KernelMMR::Rollback() noexcept
{
	m_pHashFile->Rollback();
	m_pDataFile->Discard();
	// Rewind keeps the original vector capacity, so restoration cannot allocate.
	m_entries.resize(m_committedEntries - m_rewoundEntries.size());
	m_entries.insert(m_entries.end(), m_rewoundEntries.rbegin(), m_rewoundEntries.rend());
	m_rewoundEntries.clear();
}

void KernelMMR::ApplyKernel(const TransactionKernel& kernel)
{
	// Rust stores variable-size v2 kernel data but deliberately hashes every
	// kernel with the original v1 serialization ("hash mode" in
	// KernelFeatures::write). Storage format and MMR leaf hash are therefore
	// different concerns; mixing them changes the kernel root.
	const std::vector<uint8_t> serialized = kernel.Serialized(
		m_fixedV1 ? EProtocolVersion::V1 : EProtocolVersion::V2);
	const std::vector<uint8_t> hashBytes = kernel.Serialized(EProtocolVersion::V1);

	if (serialized.size() > (std::numeric_limits<uint16_t>::max)())
	{
		throw FILE_EXCEPTION("Kernel MMR: kernel serialization exceeds u16 size");
	}

	const uint64_t offset = m_pDataFile->GetSize();
	m_pDataFile->Append(serialized);
	m_entries.push_back({ offset, (uint16_t)serialized.size() });

	MMRHashUtil::AddHashes(m_pHashFile, hashBytes, nullptr);
}
