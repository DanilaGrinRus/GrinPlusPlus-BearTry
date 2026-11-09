#include "Test_NrdSupport.h"

#include <BlockChain/Validators/BlockValidator.h>
#include <Core/Serialization/Serializer.h>

#include <json/json.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace NrdTest;

namespace
{
	struct KernelSpec
	{
		bool IsNrd = false;
		uint8_t ExcessTag = 0;
		uint64_t RelativeHeight = 0;
	};

	struct RustOracleEntry
	{
		bool Computed = false;
		std::string Category;
		std::string Detail;
	};

	std::string GetEnvVar(const char* name)
	{
#if defined(_MSC_VER)
		char* buffer = nullptr;
		size_t size = 0;
		if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
			std::string value(buffer);
			std::free(buffer);
			return value;
		}

		return {};
#else
		const char* value = std::getenv(name);
		return (value != nullptr) ? std::string(value) : std::string();
#endif
	}

	fs::path FindCasesFile()
	{
		const std::string envPath = GetEnvVar("GRINPP_NRD_CASES");
		if (!envPath.empty()) {
			// An explicitly requested fixture is authoritative: if it is missing
			// the test must FAIL, never silently fall back to another fixture.
			// The caller REQUIREs that this path exists.
			return fs::path(envPath);
		}

		const fs::path relative = fs::path("tools") / "audit" / "nrd" / "cases" / "nrd_cases.json";
		const fs::path nested = fs::path("Gplus") / "src" / "grinplusplus" / relative;

		fs::path dir = fs::current_path();
		for (int depth = 0; depth < 10; ++depth) {
			if (fs::exists(dir / relative)) {
				return dir / relative;
			}
			if (fs::exists(dir / nested)) {
				return dir / nested;
			}

			const fs::path parent = dir.parent_path();
			if (parent == dir || parent.empty()) {
				break;
			}
			dir = parent;
		}

		return {};
	}

	KernelSpec ParseKernel(const Json::Value& node)
	{
		KernelSpec spec;
		const std::string features = node.get("features", "").asString();
		REQUIRE((features == "nrd" || features == "plain"));
		spec.IsNrd = (features == "nrd");
		spec.ExcessTag = static_cast<uint8_t>(node.get("excess", 0).asUInt());
		REQUIRE(spec.ExcessTag != 0);
		if (spec.IsNrd) {
			spec.RelativeHeight = node.get("relative_height", 0).asUInt64();
			REQUIRE(spec.RelativeHeight >= 1);
			REQUIRE(spec.RelativeHeight <= Consensus::WEEK_HEIGHT);
		}

		return spec;
	}

	std::vector<TransactionKernel> BuildKernels(const Json::Value& nodes)
	{
		std::vector<TransactionKernel> kernels;
		REQUIRE(nodes.isArray());
		for (const Json::Value& node : nodes) {
			const KernelSpec spec = ParseKernel(node);
			if (spec.IsNrd) {
				kernels.push_back(MakeNrdKernel(spec.ExcessTag, spec.RelativeHeight));
			} else {
				kernels.push_back(MakePlainKernel(spec.ExcessTag));
			}
		}

		return kernels;
	}

	Json::Value ParseJsonFile(const fs::path& path)
	{
		std::ifstream stream(path.string(), std::ios::in | std::ios::binary);
		REQUIRE(stream.good());
		std::stringstream buffer;
		buffer << stream.rdbuf();
		const std::string text = buffer.str();

		Json::CharReaderBuilder builder;
		builder["collectComments"] = false;
		std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		std::string errors;
		Json::Value root;
		REQUIRE(reader->parse(text.c_str(), text.c_str() + text.size(), &root, &errors));
		REQUIRE(errors.empty());

		return root;
	}

	std::string EvaluateTarget(
		ChainBuilder& builder,
		const std::vector<TransactionKernel>& targetKernels)
	{
		const bool applied = builder.ApplyBlock(targetKernels);
		if (applied) {
			const auto result = builder.LastNrdResult();
			if (result == TxHashSet::ENrdValidationResult::ACCEPTED) {
				return "accepted";
			}
			if (result == TxHashSet::ENrdValidationResult::NONE) {
				return "accepted_non_nrd";
			}

			return "accepted_unexpected_nrd_result:" + CategoryName(result);
		}

		return CategoryName(builder.LastNrdResult());
	}

	std::map<std::string, RustOracleEntry> LoadRustOracle(const fs::path& path)
	{
		const Json::Value oracle = ParseJsonFile(path);
		REQUIRE(oracle["format_version"].asInt() == 1);
		REQUIRE(oracle.isMember("cases"));
		REQUIRE(oracle["cases"].isArray());

		std::map<std::string, RustOracleEntry> entries;
		for (const Json::Value& item : oracle["cases"]) {
			const std::string id = item["id"].asString();
			REQUIRE(!id.empty());
			RustOracleEntry entry;
			entry.Computed = (item["status"].asString() == "computed");
			if (entry.Computed) {
				entry.Category = item["actual_rust_category"].asString();
				REQUIRE(!entry.Category.empty());
			}
			entry.Detail = item.isMember("detail") ? item["detail"].asString() : std::string();
			entries[id] = entry;
		}

		return entries;
	}

	// Returns the exception message of BlockValidator::VerifySelfConsistent, or
	// "NO-EXCEPTION" when the block was not rejected by self-consistency checks.
	std::string ValidateMessage(const FullBlock& block)
	{
		try {
			BlockValidator::VerifySelfConsistent(block);
			return "NO-EXCEPTION";
		}
		catch (const std::exception& e) {
			return e.what();
		}
	}
}

TEST_CASE("NrdDifferentialCasesMatchRustOracle")
{
	const fs::path casesPath = FindCasesFile();
	INFO("differential cases path: " + casesPath.string());
	// A missing fixture must FAIL, never silently skip: a skipped run cannot
	// support a differential claim.
	REQUIRE(!casesPath.empty());
	REQUIRE(fs::exists(casesPath));

	const Json::Value root = ParseJsonFile(casesPath);
	REQUIRE(root["format_version"].asInt() == 1);
	REQUIRE(root["header_version_default"].asUInt() == kPostHf3HeaderVersion);

	const Json::Value& cases = root["cases"];
	REQUIRE(cases.isArray());
	REQUIRE(cases.size() >= 10);

	// Optional real Rust oracle: a producer (tools/audit/nrd/rust-oracle) that
	// executes real grin 5.5.2 code on these cases and emits actual categories.
	// Without it the run is expectation-metadata only and must not claim paired
	// Rust/C++ parity.
	const std::string rustOraclePath = GetEnvVar("GRINPP_NRD_RUST_ORACLE");
	std::map<std::string, RustOracleEntry> rustOracle;
	std::map<std::string, bool> oracleComputedSeen;
	bool rustOracleProvided = false;
	if (!rustOraclePath.empty()) {
		REQUIRE(fs::exists(rustOraclePath));
		rustOracle = LoadRustOracle(rustOraclePath);
		rustOracleProvided = true;
		for (const auto& entry : rustOracle) {
			if (entry.second.Computed) {
				oracleComputedSeen[entry.first] = false;
			}
		}
	}

	size_t pairedCount = 0;
	size_t pairedMismatchCount = 0;
	Json::Value report(Json::arrayValue);
	for (const Json::Value& testCase : cases) {
		const std::string id = testCase["id"].asString();
		const std::string mode = testCase["mode"].asString();
		REQUIRE(!id.empty());
		REQUIRE((mode == "incremental" || mode == "rebuild" || mode == "no_index"));

		INFO("case: " + id);
		ChainBuilder builder(ChainBuilder::MakeTempRoot("case-" + id), false, mode == "incremental");

		for (const Json::Value& blockNode : testCase["blocks"]) {
			const std::vector<TransactionKernel> kernels = BuildKernels(blockNode["kernels"]);
			if (mode == "incremental") {
				REQUIRE(builder.ApplyBlock(kernels));
			} else {
				builder.AppendWithoutBody(kernels);
			}
		}

		bool expectedComplete = true;
		if (testCase.isMember("rebuild_complete")) {
			expectedComplete = testCase["rebuild_complete"].asBool();
		}
		if (mode == "rebuild") {
			builder.Rebuild();
			REQUIRE(builder.IsIndexComplete() == expectedComplete);
		} else if (mode == "no_index") {
			REQUIRE_FALSE(builder.IsIndexComplete());
		}

		const std::string actual = EvaluateTarget(builder, BuildKernels(testCase["target"]));
		const std::string expected = testCase["expect"]["cpp_category"].asString();
		INFO("expected category: " + expected + ", actual: " + actual);
		REQUIRE(actual == expected);

		// Paired comparison: only cases where the Rust oracle actually ran real
		// Rust code on this input are compared actual-vs-actual.
		std::string rustResultSource = "expectation_metadata_not_computed";
		std::string actualRustCategory;
		std::string rustOracleDetail;
		const auto oracleIter = rustOracle.find(id);
		if (rustOracleProvided && oracleIter != rustOracle.end()) {
			rustOracleDetail = oracleIter->second.Detail;
			if (oracleIter->second.Computed) {
				actualRustCategory = oracleIter->second.Category;
				rustResultSource = "computed_by_rust_oracle";
				oracleComputedSeen[id] = true;
				++pairedCount;
				if (actual != actualRustCategory) {
					++pairedMismatchCount;
				}
				INFO("paired Rust actual: " + actualRustCategory);
				REQUIRE(actual == actualRustCategory);
			}
		}

		Json::Value caseReport;
		caseReport["id"] = id;
		caseReport["mode"] = mode;
		caseReport["expected_cpp_category"] = expected;
		caseReport["actual_cpp_category"] = actual;
		caseReport["expected_rust_category"] = testCase["expect"]["expected_rust_category"];
		caseReport["actual_rust_category"] = actualRustCategory;
		caseReport["rust_result_source"] = rustResultSource;
		caseReport["rust_oracle_detail"] = rustOracleDetail;
		caseReport["rust_oracle"] = testCase["expect"]["rust_oracle"];
		caseReport["index_complete"] = builder.IsIndexComplete();
		caseReport["index_entry_count"] = static_cast<Json::UInt64>(builder.IndexEntryCount());
		report.append(caseReport);
	}

	REQUIRE(report.size() == cases.size());

	if (rustOracleProvided) {
		// An oracle that computed nothing, or that references cases not executed
		// here, is a fixture/producer error rather than a valid paired run.
		REQUIRE(pairedCount > 0);
		REQUIRE(pairedMismatchCount == 0);
		for (const auto& entry : oracleComputedSeen) {
			REQUIRE(entry.second);
		}
	}

	const std::string reportPath = GetEnvVar("GRINPP_NRD_REPORT");
	if (!reportPath.empty()) {
		std::ofstream out(reportPath, std::ios::out | std::ios::binary | std::ios::trunc);
		REQUIRE(out.good());
		Json::StreamWriterBuilder writer;
		writer["indentation"] = "  ";
		out << Json::writeString(writer, report);
		out.close();
		REQUIRE(fs::exists(reportPath));
		REQUIRE(fs::file_size(reportPath) > 0);

		// Read the report back: a run whose report cannot be parsed must fail.
		const Json::Value readBack = ParseJsonFile(reportPath);
		REQUIRE(readBack.isArray());
		REQUIRE(readBack.size() == cases.size());
		for (const Json::Value& entry : readBack) {
			REQUIRE(entry.isMember("id"));
			REQUIRE(entry.isMember("actual_cpp_category"));
			const std::string source = entry["rust_result_source"].asString();
			REQUIRE((source == "expectation_metadata_not_computed" ||
				source == "computed_by_rust_oracle"));
		}
	}

	std::cout
		<< "NrdDifferential cases executed: " << cases.size()
		<< "; paired_rust_computed: " << pairedCount
		<< "; paired_mismatches: " << pairedMismatchCount
		<< "; oracle: " << (rustOracleProvided ? rustOraclePath : std::string("<none: expectation metadata only>"))
		<< std::endl;
}

TEST_CASE("NrdHeaderVersionGateMatchesRustHf3Rule")
{
	// Rust grin 5.5.2 core/src/core/block.rs verify_nrd_kernels_for_header_version:
	// any NRD kernel with header.version < HeaderVersion(4) => NRDKernelPreHF3.
	// core/tests/block.rs block_with_nrd_kernel_pre_post_hf3 pins version 3 as
	// invalid and version 4 as valid.
	const TransactionKernel nrdKernel = MakeNrdKernel(130, 2);
	const TransactionBody body(
		std::vector<TransactionInput>(),
		std::vector<TransactionOutput>(),
		std::vector<TransactionKernel>({ nrdKernel }));

	const BlockHeaderPtr pVersion3 = MakeHeader(3, 10, MakeHash(210), 2, 210);
	const BlockHeaderPtr pVersion4 = MakeHeader(4, 10, MakeHash(211), 2, 211);
	const FullBlock blockVersion3(pVersion3, TransactionBody(body));
	FullBlock blockVersion4(pVersion4, TransactionBody(body));

	const std::string version3Message = ValidateMessage(blockVersion3);
	INFO("version 3 message: " + version3Message);
	REQUIRE(version3Message.find("header version 4") != std::string::npos);

	// The version-4 fixture fails later body checks (it is not a real block), but
	// it must not be rejected by the NRD header-version gate.
	const std::string version4Message = ValidateMessage(blockVersion4);
	INFO("version 4 message: " + version4Message);
	REQUIRE(version4Message.find("header version 4") == std::string::npos);
}

TEST_CASE("NrdMalformedRelativeHeightIsRejectedAtDeserialization")
{
	// Rust grin 5.5.2 NRDRelativeHeight::try_from: 1..=WEEK_HEIGHT inclusive;
	// anything else is CorruptedData on read (malformed input, not consensus
	// rejection). Verify the C++ V2 kernel reader applies the same bound.
	const auto roundTrip = [](const uint64_t relativeHeight) {
		const TransactionKernel kernel(
			EKernelFeatures::NO_RECENT_DUPLICATE,
			Fee(0, 0),
			relativeHeight,
			MakeExcess(131),
			MakeSignature(131));

		Serializer serializer{ EProtocolVersion::V2 };
		kernel.Serialize(serializer);
		ByteBuffer buffer(serializer.GetBytes(), EProtocolVersion::V2);
		return TransactionKernel::Deserialize(buffer);
	};

	REQUIRE_NOTHROW(roundTrip(1));
	REQUIRE_NOTHROW(roundTrip(Consensus::WEEK_HEIGHT));
	REQUIRE_THROWS(roundTrip(0));
	REQUIRE_THROWS(roundTrip(Consensus::WEEK_HEIGHT + 1));
}
