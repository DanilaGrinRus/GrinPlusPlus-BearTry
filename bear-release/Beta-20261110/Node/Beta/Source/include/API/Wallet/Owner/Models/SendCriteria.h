#pragma once

#include <Core/Util/JsonUtil.h>
#include <Core/Exceptions/APIException.h>
#include <Wallet/SessionToken.h>
#include <Wallet/Models/DTOs/PostMethodDTO.h>
#include <Wallet/Models/DTOs/SelectionStrategyDTO.h>
#include <Wallet/Models/Slatepack/SlatepackAddress.h>
#include <API/Wallet/Owner/Models/Errors.h>
#include <optional>

class SendCriteria
{
public:
	SendCriteria(
		const SessionToken& token,
		const std::optional<uint64_t>& amount,
		const uint64_t feeBase,
		const uint8_t numOutputs,
		const SelectionStrategyDTO& selectionStrategy,
		const std::optional<std::string>& addressOpt,
		const std::optional<PostMethodDTO>& postMethodOpt
	)
		: m_token(token),
		m_amount(amount),
		m_feeBase(feeBase),
		m_numOutputs(numOutputs),
		m_selectionStrategy(selectionStrategy),
		m_addressOpt(addressOpt),
		m_postMethodOpt(postMethodOpt),
		m_slateVersion(4) { }

	Json::Value ToJSON() const noexcept
	{
		Json::Value json;
		json["session_token"] = m_token.ToBase64();

		if (m_amount.has_value()) {
			json["amount"] = m_amount.value();
		}

		json["fee_base"] = m_feeBase;
		json["change_outputs"] = m_numOutputs;
		json["selection_strategy"] = m_selectionStrategy.ToJSON();

		if (m_addressOpt.has_value()) {
			json["address"] = m_addressOpt.value();
		}

		if (m_postMethodOpt.has_value()) {
			json["post_tx"] = m_postMethodOpt.value().ToJSON();
		}

		return json;
	}

	static SendCriteria FromJSON(const Json::Value& json)
	{
		const SessionToken token = SessionToken::FromBase64(JsonUtil::GetRequiredString(json, "session_token"));
		std::optional<uint64_t> amountOpt = JsonUtil::GetUInt64Opt(json, "amount");
		const uint64_t feeBase = JsonUtil::GetRequiredUInt64(json, "fee_base");

		// change_outputs is a uint8 in the slate/tx weight calculation. Reject
		// values that would silently truncate (256 -> 0) instead of building a
		// transaction with an unintended fee or output count.
		const uint64_t numOutputsRaw = json.isMember("change_outputs")
			? JsonUtil::ConvertToUInt64(json["change_outputs"])
			: 1;
		if (numOutputsRaw > 255)
		{
			throw API_EXCEPTION(
				RPC::Errors::INVALID_CHANGE_OUTPUTS.GetCode(),
				"change_outputs must be between 0 and 255"
			);
		}

		if (amountOpt.has_value() && numOutputsRaw == 0)
		{
			// With an explicit amount, "0 change outputs" would turn the whole
			// remainder into the fee. Require an explicit choice to send all.
			throw API_EXCEPTION(
				RPC::Errors::INVALID_CHANGE_OUTPUTS.GetCode(),
				"change_outputs must be at least 1 when amount is specified"
			);
		}

		if (amountOpt.has_value() && amountOpt.value() == 0)
		{
			throw API_EXCEPTION(
				RPC::Errors::INVALID_AMOUNT.GetCode(),
				"amount must be greater than 0"
			);
		}

		const uint8_t numOutputs = (uint8_t)numOutputsRaw;
		const Json::Value selectionStrategyJSON = JsonUtil::GetRequiredField(json, "selection_strategy");
		const SelectionStrategyDTO selectionStrategy = SelectionStrategyDTO::FromJSON(selectionStrategyJSON);
		const std::optional<std::string> addressStrOpt = JsonUtil::GetStringOpt(json, "address");

		std::optional<PostMethodDTO> postMethodOpt = std::nullopt;
		std::optional<Json::Value> postJSON = JsonUtil::GetOptionalField(json, "post_tx");
		if (postJSON.has_value())
		{
			postMethodOpt = std::make_optional<PostMethodDTO>(PostMethodDTO::FromJSON(postJSON.value()));
		}

		return SendCriteria(
			token,
			amountOpt,
			feeBase,
			numOutputs,
			selectionStrategy,
			addressStrOpt,
			postMethodOpt
		);
	}

	//
	// Getters
	//
	const SessionToken& GetToken() const { return m_token; }
	std::optional<uint64_t> GetAmount() const { return m_amount; }
	uint64_t GetFeeBase() const { return m_feeBase; }
	uint8_t GetNumOutputs() const { return m_numOutputs; }
	const SelectionStrategyDTO& GetSelectionStrategy() const { return m_selectionStrategy; }
	const std::optional<std::string>& GetAddress() const { return m_addressOpt; }
	const std::optional<PostMethodDTO>& GetPostMethod() const { return m_postMethodOpt; }
	uint16_t GetSlateVersion() const { return m_slateVersion; }

	//
	// Setters
	//
	void SetSlateVersion(const uint16_t slateVersion) { m_slateVersion = slateVersion; }

private:
	SessionToken m_token;
	std::optional<uint64_t> m_amount;
	uint64_t m_feeBase;
	uint8_t m_numOutputs;
	SelectionStrategyDTO m_selectionStrategy;
	std::optional<std::string> m_addressOpt;
	std::optional<PostMethodDTO> m_postMethodOpt;
	uint16_t m_slateVersion;
};