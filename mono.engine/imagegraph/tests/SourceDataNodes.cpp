#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_data_nodes")
using namespace engine::imagegraph;
namespace {
	imagegraph_test::NodeRun Dynamic(Node node) {
		imagegraph_test::NodeRun run;
		const auto *entry = FindCatalogueEntry(node.Type);
		REQUIRE(entry);
		EvaluationRequest request;
		engine::imagegraph::detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &input : entry->Inputs) {
			const auto found = std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
				return value.Port == input.Id;
			});
			if (found != node.Values.end())
				context.Values.emplace_back(input.Id, found->Data);
			else if (auto fallback = CatalogueDefault(input))
				context.Values.emplace_back(input.Id, std::move(*fallback));
		}
		for (const auto &input : node.DynamicInputs)
			if (input.Default) context.Values.emplace_back(input.Id, *input.Default);
		run.Ok = engine::imagegraph::detail::RunProcessorBatch(
			context, engine::imagegraph::detail::FindExecutor(node.Type)
		);
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Values = std::move(context.OutputValues);
		return run;
	}
}
TEST_CASE("Area preserves center spans and computes two-point extents", "[imagegraph][source_data]") {
	const auto centered =
		imagegraph_test::RunNode("pc.area", {}, {{"position", Vector2{3, 4}}, {"span", Vector2{-2, 8}}});
	REQUIRE(centered.Ok);
	CHECK(std::get<Area>(*centered.OutputValue("area")) == Area{3, 4, -2, 8, 0, 0});
	const auto points = imagegraph_test::RunNode(
		"pc.area",
		{},
		{{"type", EnumValue{1}},
		 {"position", Vector2{4, 8}},
		 {"span", Vector2{-2, 2}},
		 {"shape", EnumValue{1}}}
	);
	REQUIRE(points.Ok);
	CHECK(std::get<Area>(*points.OutputValue("area")) == Area{1, 5, 3, 3, 1, 0});
}
TEST_CASE(
	"Logic supports source scalar modes and native periodic nested array projection",
	"[imagegraph][source_data]"
) {
	const std::array<bool, 6> expected{false, true, false, true, false, true};
	for (int64_t mode = 0; mode < 6; ++mode) {
		Node node{
			"logic",
			"pc.logic",
			"",
			{},
			{{"type", EnumValue{mode}}},
			{{"jname_0", ValueType::Boolean, Value{true}}, {"jname_1", ValueType::Boolean, Value{false}}}
		};
		const auto run = Dynamic(node);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(std::get<bool>(*run.OutputValue("result")) == expected[size_t(mode)]);
	}
	Node one{"logic", "pc.logic", "", {}, {}, {{"jname_0", ValueType::Boolean, Value{true}}}};
	const auto identity = Dynamic(one);
	REQUIRE(identity.Ok);
	CHECK(std::get<bool>(*identity.OutputValue("result")));
	Node arrays{
		"logic",
		"pc.logic",
		"",
		{},
		{{"type", EnumValue{5}}},
		{{"jname_0", ValueType::Array, Value{ArrayValue{ValueType::Boolean, {true, false, true}}}},
		 {"jname_1", ValueType::Array, Value{ArrayValue{ValueType::Boolean, {false, true}}}}}
	};
	const auto run = Dynamic(arrays);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &array = std::get<ArrayValue>(*run.OutputValue("result"));
	REQUIRE(array.Items.size() == 3);
	for (const auto &item : array.Items)
		CHECK(std::get<bool>(std::get<ElementValue>(item.Data)));
}
TEST_CASE(
	"Statistic spreads flat source input arrays once and covers all reductions", "[imagegraph][source_data]"
) {
	const std::array<double, 5> expected{13, 3.25, 2.5, 9, -1};
	for (int64_t mode = 0; mode < 5; ++mode) {
		Node node{
			"stat",
			"pc.statistic",
			"",
			{},
			{{"type", EnumValue{mode}}},
			{{"input_0", ValueType::Array, Value{ArrayValue{ValueType::Scalar, {9.0, -1.0, 2.0}}}},
			 {"input_1", ValueType::Scalar, Value{3.0}}}
		};
		const auto run = Dynamic(node);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(std::get<double>(*run.OutputValue("statistic")) == expected[size_t(mode)]);
	}
}
TEST_CASE(
	"Seconds conversion preserves negative days and replaces only first token occurrences",
	"[imagegraph][source_data]"
) {
	const auto run = imagegraph_test::RunNode(
		"pc.sec_convert", {}, {{"seconds", 90061.25}, {"format", std::string{"%d %h:%n:%s %s"}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(std::get<std::string>(*run.OutputValue("format_string")) == "01 01:01:01 %s.25");
	CHECK(std::get<double>(*run.OutputValue("seconds")) == 1.25);
	CHECK(std::get<double>(*run.OutputValue("days")) == 1);
	const auto negative = imagegraph_test::RunNode("pc.sec_convert", {}, {{"seconds", -.5}});
	REQUIRE(negative.Ok);
	CHECK(std::get<std::string>(*negative.OutputValue("format_string")) == "23:59:59.50");
	CHECK(std::get<double>(*negative.OutputValue("days")) == -1);
}
TEST_CASE(
	"Buffer text formats unsigned bytes and stops ASCII at its terminator", "[imagegraph][source_data]"
) {
	const BufferValue bytes{{0xFF, 0x00, 0x41}};
	const std::array<std::string, 4> expected{
		"111111110000000001000001", "FF0041", std::string(1, char(0xFF)), "/wBB"
	};
	for (int64_t mode = 0; mode < 4; ++mode) {
		const auto run = imagegraph_test::RunNode(
			"pc.buffer_to_string", {}, {{"input_0", bytes}, {"format", EnumValue{mode}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(std::get<std::string>(*run.OutputValue("string_out")) == expected[size_t(mode)]);
	}
	for (const auto &[bytes, expected] :
		 std::array<std::pair<BufferValue, std::string>, 2>{{{{{'M'}}, "TQ=="}, {{{'M', 'a'}}, "TWE="}}}) {
		const auto run = imagegraph_test::RunNode(
			"pc.buffer_to_string", {}, {{"input_0", bytes}, {"format", EnumValue{3}}}
		);
		REQUIRE(run.Ok);
		CHECK(std::get<std::string>(*run.OutputValue("string_out")) == expected);
	}
	const auto empty = imagegraph_test::RunNode("pc.buffer_to_string", {});
	REQUIRE(empty.Ok);
	CHECK(std::get<std::string>(*empty.OutputValue("string_out")).empty());
}
TEST_CASE(
	"Number formatting follows exact decimal ties and source padding order", "[imagegraph][source_data]"
) {
	for (const auto &[number, expected] : std::array<std::pair<double, std::string>, 6>{
			 {{.125, "0.13"}, {-.125, "-0.13"}, {1.005, "1"}, {.375, "0.38"}, {9.5, "9.5"}, {-0.0, "0"}}
		 }) {
		const auto run = imagegraph_test::RunNode("pc.number_text_format", {}, {{"number", number}});
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(std::get<std::string>(*run.OutputValue("text")) == expected);
	}
	const auto padded = imagegraph_test::RunNode(
		"pc.number_text_format",
		{},
		{{"number", -12.5},
		 {"pad_integer", true},
		 {"minimum_digit", int64_t{6}},
		 {"pad_letter", std::string{"界"}},
		 {"thousands_sep", std::string{","}},
		 {"pad_decimal", true},
		 {"minimum_digit_2", int64_t{3}},
		 {"pad_letter_2", std::string{"x"}},
		 {"decimal_sep", std::string{":"}},
		 {"prefix", std::string{"["}},
		 {"suffix", std::string{"]"}}}
	);
	INFO(padded.Message);
	REQUIRE(padded.Ok);
	CHECK(std::get<std::string>(*padded.OutputValue("text")) == "[界界界,-12:5xx]");
	const auto invalid =
		imagegraph_test::RunNode("pc.number_text_format", {}, {{"minimum_digit_2", int64_t{101}}});
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Code == Status::InvalidValue);
	const auto bounded = imagegraph_test::RunNode(
		"pc.number_text_format",
		{},
		{{"pad_integer", true},
		 {"minimum_digit", int64_t(Limits::MaximumTextBytes)},
		 {"pad_letter", std::string{"xxxx"}}}
	);
	CHECK_FALSE(bounded.Ok);
	CHECK(bounded.Code == Status::LimitExceeded);
}
TEST_CASE(
	"File path separation preserves inspected HTML5 separator and extension behavior",
	"[imagegraph][source_data]"
) {
	for (const auto &[path, directory, name] : std::array<std::array<std::string, 3>, 5>{
			 {{"folder/file.txt", "folder", "file.txt"},
			  {"C:\\folder\\file.txt", "C:\\folder", "file.txt"},
			  {"/file.txt", "/file.txt", "/file.txt"},
			  {"plain", "plain", "plain"},
			  {"folder/", "folder", ""}}
		 }) {
		const auto run = imagegraph_test::RunNode("pc.path_separate_folder", {}, {{"path", path}});
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(std::get<std::string>(*run.OutputValue("directory")) == directory);
		CHECK(std::get<std::string>(*run.OutputValue("file_name")) == name);
	}
	for (const auto &[path, name] : std::array<std::pair<std::string, std::string>, 3>{
			 {{"folder/file.txt", "file"}, {"plain", "plai"}, {"a.txta.txt", "aa.txt"}}
		 }) {
		const auto run = imagegraph_test::RunNode(
			"pc.path_separate_folder", {}, {{"path", path}, {"keep_extension", false}}
		);
		REQUIRE(run.Ok);
		CHECK(std::get<std::string>(*run.OutputValue("file_name")) == name);
	}
}
