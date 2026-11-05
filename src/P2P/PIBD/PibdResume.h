#pragma once

#include "PibdDownload.h"

#include "../Messages/MessageTypes.h"

#include <Common/Util/FileUtil.h>
#include <Common/Util/HexUtil.h>
#include <Crypto/Models/Hash.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>

#include <filesystem.h>

#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Persists the segments a PIBD download has already validated, so a download
// interrupted by a process restart can continue instead of starting over.
//
// Layout: one append-only journal per segment kind inside the resume directory.
// Every frame is a big-endian u32 length followed by the raw p2p payload of an
// accepted segment. Replay order is fixed: bitmap segments first (the other
// trees are verified against the assembled bitmap), then outputs, rangeproofs
// and kernels; within a kind the original acceptance order is preserved.
class PibdResume
{
public:
	using ReplayFn = std::function<void(MessageTypes::EMessageType, const std::vector<uint8_t>&)>;

	explicit PibdResume(fs::path directory)
		: m_directory(std::move(directory))
	{
	}

	// Keeps an existing journal when it belongs to the same horizon, otherwise
	// clears it and starts a new one. Returns true when replay is possible.
	bool Prepare(const PibdDownload::Horizon& horizon)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const std::string manifest = ManifestFor(horizon);
		if (FileUtil::Exists(m_directory / "manifest")) {
			std::vector<uint8_t> existing;
			if (FileUtil::ReadFile(m_directory / "manifest", existing)) {
				const std::string text(existing.cbegin(), existing.cend());
				if (text == manifest && HasAnyJournal()) {
					return true;
				}
			}
		}

		ClearLocked();
		if (!FileUtil::Exists(m_directory)) {
			FileUtil::CreateDirectories(m_directory);
		}

		std::vector<uint8_t> bytes(manifest.cbegin(), manifest.cend());
		FileUtil::SafeWriteToFile(m_directory / "manifest", bytes);
		return false;
	}

	// Appends one accepted segment payload. Failures are best effort: a missing
	// journal only means the next restart re-downloads more.
	void Store(const MessageTypes::EMessageType type, const std::vector<uint8_t>& payload)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const fs::path path = JournalFor(type);
		if (path.empty()) {
			return;
		}

		Serializer frame;
		frame.Append<uint32_t>((uint32_t)payload.size());
		frame.AppendByteVector(payload);

		std::ofstream out(path, std::ios::binary | std::ios::app);
		if (!out) {
			return;
		}

		const std::vector<uint8_t>& data = frame.GetBytes();
		out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
	}

	void LoadAll(const ReplayFn& replay) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const MessageTypes::EMessageType type : ReplayOrder()) {
			const fs::path path = JournalFor(type);
			if (path.empty() || !FileUtil::Exists(path)) {
				continue;
			}

			std::vector<uint8_t> data;
			if (!FileUtil::ReadFile(path, data)) {
				continue;
			}

			ByteBuffer buffer(data);
			while (buffer.GetRemainingSize() > 0) {
				const uint32_t size = buffer.ReadU32();
				const std::vector<uint8_t> payload = buffer.ReadVector(size);
				replay(type, payload);
			}
		}
	}

	void Clear()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		ClearLocked();
	}

	static std::vector<MessageTypes::EMessageType> ReplayOrder()
	{
		return {
			MessageTypes::OutputBitmapSegment,
			MessageTypes::OutputSegment,
			MessageTypes::RangeProofSegment,
			MessageTypes::KernelSegment
		};
	}

private:
	static std::string NameFor(const MessageTypes::EMessageType type)
	{
		switch (type) {
			case MessageTypes::OutputBitmapSegment: return "bitmap.log";
			case MessageTypes::OutputSegment: return "output.log";
			case MessageTypes::RangeProofSegment: return "rangeproof.log";
			case MessageTypes::KernelSegment: return "kernel.log";
			default: return {};
		}
	}

	std::string ManifestFor(const PibdDownload::Horizon& horizon) const
	{
		const std::string hash = HexUtil::ConvertToHex(horizon.BlockHash.GetData());
		const std::string outputRoot = HexUtil::ConvertToHex(horizon.OutputRoot.GetData());
		const std::string rangeProofRoot = HexUtil::ConvertToHex(horizon.RangeProofRoot.GetData());
		const std::string kernelRoot = HexUtil::ConvertToHex(horizon.KernelRoot.GetData());
		return hash + "\n" + std::to_string(horizon.Height) + "\n" +
			std::to_string(horizon.OutputMmrSize) + "\n" +
			std::to_string(horizon.KernelMmrSize) + "\n" +
			outputRoot + "\n" + rangeProofRoot + "\n" + kernelRoot + "\n";
	}

	fs::path JournalFor(const MessageTypes::EMessageType type) const
	{
		const std::string name = NameFor(type);
		if (name.empty()) {
			return {};
		}
		return m_directory / name;
	}

	bool HasAnyJournal() const
	{
		for (const MessageTypes::EMessageType type : ReplayOrder()) {
			const fs::path path = JournalFor(type);
			if (!path.empty() && FileUtil::Exists(path)) {
				return true;
			}
		}
		return false;
	}

	void ClearLocked()
	{
		std::error_code error;
		fs::remove_all(m_directory, error);
	}

	mutable std::mutex m_mutex;
	fs::path m_directory;
};
