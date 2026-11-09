#pragma once

#include <API/Wallet/Owner/Models/Errors.h>
#include <Common/Util/FileUtil.h>
#include <Core/Config.h>
#include <Core/File/FileSync.h>
#include <Core/Global.h>
#include <Core/Util/JsonUtil.h>
#include <Net/Servers/RPC/RPCMethod.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

// Validate -> persist -> apply. The runtime Config is only changed after the
// new document has been written, flushed, read back and accepted. A failure
// therefore leaves the running node on the old configuration.
class UpdateConfigHandler : public RPCMethod
{
public:
	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value()) {
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value params = request.GetParams().value();
		if (!params.isObject()) {
			return request.BuildError("INVALID_PARAMS", "Expected object parameter");
		}

		std::optional<uint32_t> minPeers;
		std::optional<uint32_t> maxPeers;
		std::optional<uint32_t> confirmations;
		std::optional<uint32_t> reuseAddress;
		try {
			minPeers = JsonUtil::GetUInt32Opt(params, "min_peers");
			maxPeers = JsonUtil::GetUInt32Opt(params, "max_peers");
			confirmations = JsonUtil::GetUInt32Opt(params, "min_confirmations");
			reuseAddress = JsonUtil::GetUInt32Opt(params, "reuse_address");
		} catch (const std::exception&) {
			return request.BuildError("INVALID_PARAMS", "Numeric parameter is out of range");
		}

		const auto parsePeers = [&params](const char* name)
			-> std::optional<std::unordered_set<IPAddress>> {
			if (!params.isMember(name)) {
				return std::nullopt;
			}
			if (!params[name].isArray()) {
				throw std::invalid_argument("Peer list must be an array");
			}
			if (params[name].size() > 256) {
				throw std::invalid_argument("Peer list exceeds 256 entries");
			}

			std::unordered_set<IPAddress> parsed;
			for (const Json::Value& peer : params[name]) {
				if (!peer.isString()) {
					throw std::invalid_argument("Peer must be an IP address string");
				}
				parsed.emplace(IPAddress::Parse(peer.asString()));
			}
			return parsed;
		};

		std::optional<std::unordered_set<IPAddress>> preferredPeers;
		std::optional<std::unordered_set<IPAddress>> allowedPeers;
		std::optional<std::unordered_set<IPAddress>> blockedPeers;
		try {
			preferredPeers = parsePeers("preferred_peers");
			allowedPeers = parsePeers("allowed_peers");
			blockedPeers = parsePeers("blocked_peers");
		} catch (const std::exception&) {
			return request.BuildError("INVALID_PARAMS", "Invalid peer list or peer address");
		}

		Config& config = Global::GetConfig();

		int resultingMin = config.GetMinPeers();
		int resultingMax = config.GetMaxPeers();
		if (minPeers.has_value()) {
			if (*minPeers > static_cast<uint32_t>((std::numeric_limits<int>::max)())) {
				return request.BuildError("INVALID_PARAMS", "min_peers is out of range");
			}
			resultingMin = static_cast<int>(*minPeers);
		}
		if (maxPeers.has_value()) {
			if (*maxPeers > static_cast<uint32_t>((std::numeric_limits<int>::max)())) {
				return request.BuildError("INVALID_PARAMS", "max_peers is out of range");
			}
			resultingMax = static_cast<int>(*maxPeers);
		}
		if (resultingMin < static_cast<int>(config.GetMinSyncPeers()) ||
			resultingMax <= 0 || resultingMin > resultingMax) {
			return request.BuildError(
				"INVALID_PARAMS",
				"min_peers must be between min_sync_peers and max_peers");
		}
		if (reuseAddress.has_value() && *reuseAddress > 1) {
			return request.BuildError("INVALID_PARAMS", "reuse_address must be 0 or 1");
		}

		Json::Value configJson = config.GetJSON();
		if (!configJson.isMember("P2P") || !configJson["P2P"].isObject()) {
			configJson["P2P"] = Json::Value(Json::objectValue);
		}
		if (!configJson.isMember("WALLET") || !configJson["WALLET"].isObject()) {
			configJson["WALLET"] = Json::Value(Json::objectValue);
		}

		Json::Value& p2pJson = configJson["P2P"];
		Json::Value& walletJson = configJson["WALLET"];
		if (minPeers.has_value()) {
			p2pJson["MIN_PEERS"] = resultingMin;
		}
		if (maxPeers.has_value()) {
			p2pJson["MAX_PEERS"] = resultingMax;
		}
		if (confirmations.has_value()) {
			walletJson["MIN_CONFIRMATIONS"] = *confirmations;
		}
		if (reuseAddress.has_value()) {
			walletJson["REUSE_ADDRESS"] = *reuseAddress;
		}

		const auto writePeers = [&p2pJson](
			const char* name,
			const std::optional<std::unordered_set<IPAddress>>& peers) {
			if (!peers.has_value()) {
				return;
			}

			Json::Value array(Json::arrayValue);
			for (const IPAddress& peer : peers.value()) {
				array.append(peer.Format());
			}
			p2pJson[name] = array;
		};
		writePeers("PREFERRED_PEERS", preferredPeers);
		writePeers("ALLOWED_PEERS", allowedPeers);
		writePeers("BLOCKED_PEERS", blockedPeers);

		fs::path configPath = Global::GetConfigFilePath();
		if (configPath.empty())
		{
			configPath = config.GetDataDirectory() / "server_config.json";
		}
		const fs::path tempPath = configPath.string() + ".tmp";
		const fs::path backupPath = configPath.string() + ".bak";
		std::error_code error;

		try {
			fs::create_directories(configPath.parent_path(), error);

			{
				std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
				if (!file.is_open()) {
					return request.BuildError("INTERNAL_ERROR", "Failed to open temporary config file");
				}

				file << configJson.toStyledString();
				file.flush();
				if (!file.good()) {
					file.close();
					fs::remove(tempPath, error);
					return request.BuildError("INTERNAL_ERROR", "Failed to write temporary config file");
				}
			}

			FileSync::Flush(tempPath);

			std::vector<uint8_t> persistedBytes;
			if (!FileUtil::ReadFile(tempPath, persistedBytes)) {
				fs::remove(tempPath, error);
				return request.BuildError("INTERNAL_ERROR", "Persisted config is not readable");
			}

			Json::Value persisted;
			if (!JsonUtil::Parse(
					std::string(persistedBytes.begin(), persistedBytes.end()), persisted) ||
				persisted.toStyledString() != configJson.toStyledString()) {
				fs::remove(tempPath, error);
				return request.BuildError(
					"INTERNAL_ERROR", "Persisted config does not match the prepared state");
			}

			const bool hadTarget = fs::exists(configPath);
			if (hadTarget) {
				fs::copy_file(configPath, backupPath, fs::copy_options::overwrite_existing, error);
				if (error) {
					fs::remove(tempPath, error);
					return request.BuildError("INTERNAL_ERROR", "Failed to back up the current config");
				}
			}

			fs::rename(tempPath, configPath, error);
			if (error) {
				error.clear();
				fs::remove(configPath, error);
				error.clear();
				fs::rename(tempPath, configPath, error);
			}
			if (error) {
				if (hadTarget && fs::exists(backupPath)) {
					std::error_code restoreError;
					fs::copy_file(
						backupPath, configPath, fs::copy_options::overwrite_existing, restoreError);
				}
				fs::remove(tempPath, error);
				return request.BuildError("INTERNAL_ERROR", "Failed to replace the config file");
			}

			fs::remove(backupPath, error);
		} catch (const std::exception& e) {
			fs::remove(tempPath, error);
			return request.BuildError(
				"INTERNAL_ERROR", std::string("Config persistence failed: ") + e.what());
		}

		// Persistence succeeded; publish the runtime values.
		if (minPeers.has_value()) {
			config.SetMinPeers(resultingMin);
		}
		if (maxPeers.has_value()) {
			config.SetMaxPeers(resultingMax);
		}
		if (confirmations.has_value()) {
			config.SetMinConfirmations(*confirmations);
		}
		if (reuseAddress.has_value()) {
			config.ShouldReuseAddresses(*reuseAddress == 1);
		}
		if (preferredPeers.has_value()) {
			config.UpdatePreferredPeers(*preferredPeers);
		}
		if (allowedPeers.has_value()) {
			config.UpdateAllowedPeers(*allowedPeers);
		}
		if (blockedPeers.has_value()) {
			config.UpdateBlockedPeers(*blockedPeers);
		}

		Json::Value result(Json::objectValue);
		result["restart_required"] = false;
		return request.BuildResult(result);
	}

	ApiAccess::Level GetAccessLevel() const noexcept final { return ApiAccess::Level::ADMIN; }

	bool ContainsSecrets() const noexcept final { return false; }
};
