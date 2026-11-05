#pragma once

#include "Common/MMR.h"
#include "Common/HashFile.h"

#include <Core/File/AppendOnlyFile.h>
#include <Core/Models/TransactionKernel.h>
#include <Core/Models/FullBlock.h>
#include <Core/Traits/Lockable.h>
#include <Crypto/Models/Hash.h>
#include <PMMR/Common/LeafIndex.h>
#include <filesystem.h>
#include <cstdint>
#include <vector>

#define KERNEL_SIZE 114

class KernelMMR : public MMR
{
public:
	KernelMMR(
		std::shared_ptr<HashFile> pHashFile,
		std::shared_ptr<AppendOnlyFile> pDataFile,
		const bool fixedV1);
	virtual ~KernelMMR() = default;

	static std::shared_ptr<KernelMMR> Load(const fs::path & txHashSetPath);

	std::unique_ptr<TransactionKernel> GetKernelAt(const LeafIndex& leaf_idx) const;
	bool Rewind(const uint64_t size);

	Hash Root(const uint64_t size) const final;
	uint64_t GetSize() const final { return m_pHashFile->GetSize(); }
	uint64_t GetNumKernels() const { return m_entries.size(); }

	std::unique_ptr<Hash> GetHashAt(const Index& mmrIndex) const final
	{
		return std::make_unique<Hash>(m_pHashFile->GetDataAt(mmrIndex.GetPosition()));
	}

	std::vector<Hash> GetLastLeafHashes(const uint64_t numHashes) const final;

	void Commit() final;
	void Rollback() noexcept final;

	void ApplyKernel(const TransactionKernel& kernel);

private:
	struct KernelEntry
	{
		uint64_t Offset;
		uint16_t Size;
	};

	static std::vector<KernelEntry> ParseVariableEntries(
		const std::shared_ptr<AppendOnlyFile>& pDataFile);

	mutable std::shared_ptr<HashFile> m_pHashFile;
	mutable std::shared_ptr<AppendOnlyFile> m_pDataFile;
	bool m_fixedV1;
	std::vector<KernelEntry> m_entries;
	// Original committed entries removed by rewind, in reverse order.
	std::vector<KernelEntry> m_rewoundEntries;
	size_t m_committedEntries = 0;
};
