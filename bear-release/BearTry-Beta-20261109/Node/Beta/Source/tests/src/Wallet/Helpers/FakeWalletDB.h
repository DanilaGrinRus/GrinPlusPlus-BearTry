#pragma once

#include <Wallet/WalletDB/WalletDB.h>
#include <Common/Secure.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

//
// In-memory IWalletDB for wallet refresh / restore tests.
//
// Batched writes are staged on OnInitWrite(true) and restored by Rollback(),
// which mirrors the production writer: an exception thrown in the middle of a
// refresh must leave no partial output/cursor update behind.
//
class FakeWalletDB : public IWalletDB
{
public:
	std::vector<OutputDataEntity> outputs;
	std::vector<WalletTx> transactions;
	uint32_t nextTxId = 1;
	uint64_t refreshHeight = 0;
	uint64_t restoreLeafIndex = 0;

	KeyChainPath GetNextChildPath(const KeyChainPath& parentPath) override
	{
		return parentPath.GetChild(0);
	}

	int GetCurrentAddressIndex(const KeyChainPath&) const override { return 0; }
	int IncreaseAddressIndex(const KeyChainPath&) override { return 0; }

	std::unique_ptr<Slate> LoadSlate(const SecureVector&, const uuids::uuid&, const SlateStage&) const override
	{
		return nullptr;
	}

	std::pair<std::unique_ptr<Slate>, std::string> LoadLatestSlate(const SecureVector&, const uuids::uuid&) const override
	{
		return { nullptr, std::string() };
	}

	std::string LoadArmoredSlatepack(const uuids::uuid&) const override { return std::string(); }

	void SaveSlate(const SecureVector&, const Slate&, const std::string&) override { }

	std::unique_ptr<SlateContextEntity> LoadSlateContext(const SecureVector&, const uuids::uuid&) const override
	{
		return nullptr;
	}

	void SaveSlateContext(const SecureVector&, const uuids::uuid&, const SlateContextEntity&) override { }

	void AddOutputs(const SecureVector&, const std::vector<OutputDataEntity>& newOutputs) override
	{
		for (const OutputDataEntity& output : newOutputs)
		{
			bool replaced = false;
			for (OutputDataEntity& existing : outputs)
			{
				if (existing.GetCommitment() == output.GetCommitment())
				{
					existing = output;
					replaced = true;
					break;
				}
			}

			if (!replaced) {
				outputs.push_back(output);
			}
		}
	}

	std::vector<OutputDataEntity> GetOutputs(const SecureVector&) const override { return outputs; }

	void AddTransaction(const SecureVector&, const WalletTx& walletTx) override
	{
		for (WalletTx& existing : transactions)
		{
			if (existing.GetId() == walletTx.GetId())
			{
				existing = walletTx;
				return;
			}
		}

		transactions.push_back(walletTx);
	}

	std::vector<WalletTx> GetTransactions(const SecureVector&) const override { return transactions; }

	std::unique_ptr<WalletTx> GetTransactionById(const SecureVector&, const uint32_t walletTxId) const override
	{
		for (const WalletTx& walletTx : transactions)
		{
			if (walletTx.GetId() == walletTxId) {
				return std::make_unique<WalletTx>(walletTx);
			}
		}

		return nullptr;
	}

	uint32_t GetNextTransactionId() override { return nextTxId++; }

	uint64_t GetRefreshBlockHeight() const override { return refreshHeight; }
	void UpdateRefreshBlockHeight(const uint64_t height) override { refreshHeight = height; }

	uint64_t GetRestoreLeafIndex() const override { return restoreLeafIndex; }
	void UpdateRestoreLeafIndex(const uint64_t index) override { restoreLeafIndex = index; }

	void Commit() override
	{
		// WalletRefresher commits through Writer::operator-> (IWalletDB*), so
		// the Writer cannot clear its dirty flag for us. Clear it here so the
		// Writer destructor does not roll the committed batch back.
		SetDirty(false);
	}

	void Rollback() noexcept override
	{
		outputs = m_snapshotOutputs;
		transactions = m_snapshotTransactions;
		nextTxId = m_snapshotNextTxId;
		refreshHeight = m_snapshotRefreshHeight;
		restoreLeafIndex = m_snapshotRestoreLeafIndex;
	}

	void OnInitWrite(const bool batch) override
	{
		if (!batch) {
			return;
		}

		m_snapshotOutputs = outputs;
		m_snapshotTransactions = transactions;
		m_snapshotNextTxId = nextTxId;
		m_snapshotRefreshHeight = refreshHeight;
		m_snapshotRestoreLeafIndex = restoreLeafIndex;
	}

private:
	std::vector<OutputDataEntity> m_snapshotOutputs;
	std::vector<WalletTx> m_snapshotTransactions;
	uint32_t m_snapshotNextTxId = 1;
	uint64_t m_snapshotRefreshHeight = 0;
	uint64_t m_snapshotRestoreLeafIndex = 0;
};
