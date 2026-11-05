#include <catch.hpp>

#include <API/Wallet/Owner/Models/TxLifecycle.h>
#include <Wallet/SlateBuilder/PostTransactionResult.h>

#include <optional>
#include <string>

TEST_CASE("TxLifecycle: mapping from wallet status and broadcast outcome")
{
	using namespace TxLifecycle;

	REQUIRE(FromWalletTx(EWalletTxType::SENDING_STARTED, false, std::nullopt) == ETxLifecycleState::NOT_FINALIZED);
	REQUIRE(FromWalletTx(EWalletTxType::SENDING_STARTED, true, std::nullopt) == ETxLifecycleState::CONFIRMED);

	// Finalized, but this process has no broadcast outcome (e.g. after restart):
	// this is exactly the crash-between-commit-and-post state.
	REQUIRE(FromWalletTx(EWalletTxType::SENDING_FINALIZED, false, std::nullopt) == ETxLifecycleState::UNKNOWN);
	REQUIRE(
		FromWalletTx(EWalletTxType::SENDING_FINALIZED, false, EBroadcastResult::POSTED)
		== ETxLifecycleState::POSTED
	);
	REQUIRE(
		FromWalletTx(EWalletTxType::SENDING_FINALIZED, false, EBroadcastResult::ACCEPTED)
		== ETxLifecycleState::ACCEPTED
	);
	REQUIRE(
		FromWalletTx(EWalletTxType::SENDING_FINALIZED, false, EBroadcastResult::FAILED)
		== ETxLifecycleState::FINALIZED_NOT_BROADCAST
	);
	REQUIRE(
		FromWalletTx(EWalletTxType::SENDING_FINALIZED, false, EBroadcastResult::NOT_REQUESTED)
		== ETxLifecycleState::FINALIZED
	);
	REQUIRE(FromWalletTx(EWalletTxType::SENDING_FINALIZED, true, EBroadcastResult::FAILED) == ETxLifecycleState::CONFIRMED);

	REQUIRE(FromWalletTx(EWalletTxType::SENT, false, std::nullopt) == ETxLifecycleState::CONFIRMED);
	REQUIRE(FromWalletTx(EWalletTxType::RECEIVED, false, std::nullopt) == ETxLifecycleState::CONFIRMED);
	REQUIRE(FromWalletTx(EWalletTxType::SENT_CANCELED, false, std::nullopt) == ETxLifecycleState::UNKNOWN);

	REQUIRE(std::string(ToString(ETxLifecycleState::FINALIZED_NOT_BROADCAST)) == "finalized_not_broadcast");
	REQUIRE(std::string(ToString(EBroadcastResult::NOT_REQUESTED)) == "not_requested");
}

TEST_CASE("TxLifecycle: cancelability requires an unconfirmed transaction")
{
	REQUIRE(TxLifecycle::IsCancelable(EWalletTxType::SENDING_STARTED, false));
	REQUIRE(TxLifecycle::IsCancelable(EWalletTxType::SENDING_FINALIZED, false));
	REQUIRE(TxLifecycle::IsCancelable(EWalletTxType::RECEIVING_IN_PROGRESS, false));
	REQUIRE(!TxLifecycle::IsCancelable(EWalletTxType::SENDING_FINALIZED, true));
	REQUIRE(!TxLifecycle::IsCancelable(EWalletTxType::SENT, false));
	REQUIRE(!TxLifecycle::IsCancelable(EWalletTxType::RECEIVED, false));
	REQUIRE(!TxLifecycle::IsCancelable(EWalletTxType::COINBASE, false));
}

TEST_CASE("TxBroadcastRegistry: records, finds and clears per-slate outcomes")
{
	TxBroadcastRegistry::Clear();
	REQUIRE(!TxBroadcastRegistry::Find("slate-1").has_value());

	TxBroadcastRegistry::Record("slate-1", EBroadcastResult::FAILED, "DuplicateTx");
	const auto entry = TxBroadcastRegistry::Find("slate-1");
	REQUIRE(entry.has_value());
	REQUIRE(entry->result == EBroadcastResult::FAILED);
	REQUIRE(entry->error == "DuplicateTx");

	TxBroadcastRegistry::Record("slate-1", EBroadcastResult::POSTED);
	REQUIRE(TxBroadcastRegistry::Find("slate-1")->result == EBroadcastResult::POSTED);

	TxBroadcastRegistry::Clear();
	REQUIRE(!TxBroadcastRegistry::Find("slate-1").has_value());
}

TEST_CASE("ParsePostTransactionResponse: only an explicit Ok is a successful broadcast")
{
	Json::Value id = Json::Value(1);

	Json::Value okResult(Json::objectValue);
	okResult["Ok"] = Json::nullValue;
	REQUIRE(ParsePostTransactionResponse(RPC::Response::BuildResult(id, okResult)).status == EPostTransactionStatus::POSTED);

	Json::Value errResult(Json::objectValue);
	errResult["Err"] = "DuplicateTx";
	const PostTransactionOutcome rejected = ParsePostTransactionResponse(RPC::Response::BuildResult(id, errResult));
	REQUIRE(rejected.status == EPostTransactionStatus::REJECTED);
	REQUIRE(rejected.error == "DuplicateTx");

	// Grin++ node style: a JSON-RPC error, not a result wrapper.
	const PostTransactionOutcome rpcError = ParsePostTransactionResponse(
		RPC::Response::BuildError(id, RPC::ErrorCode::INTERNAL_ERROR, "TX_POOL_ERROR")
	);
	REQUIRE(rpcError.status == EPostTransactionStatus::REJECTED);
	REQUIRE(rpcError.error == "TX_POOL_ERROR");

	// A bare/unrecognised result must not be reported as posted.
	REQUIRE(
		ParsePostTransactionResponse(RPC::Response::BuildResult(id, Json::Value("ok"))).status
		== EPostTransactionStatus::UNKNOWN
	);
	REQUIRE(
		ParsePostTransactionResponse(RPC::Response::BuildResult(id, Json::nullValue)).status
		== EPostTransactionStatus::UNKNOWN
	);
}
