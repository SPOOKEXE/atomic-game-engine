#include "../src/ValuePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.node_source_text")
using namespace engine::imagegraph;
using namespace imagegraph_test;
namespace {
	const Value &NodeValue(const NodeRun &run, std::string_view port = "text") {
		INFO(run.Message);
		REQUIRE(run.Ok);
		REQUIRE(run.OutputValue(port));
		return *run.OutputValue(port);
	}
	const std::string &String(const NodeRun &run) {
		return std::get<std::string>(NodeValue(run));
	}
}
TEST_CASE("Text split keeps delimiter empties and Unicode periodic boundaries", "[imagegraph][source_text]") {
	const auto split =
		RunNode("pc.string_split", {}, {{"text", std::string{"a,,b,"}}, {"delimiter", std::string{","}}});
	CHECK(
		std::get<ArrayValue>(NodeValue(split)).Elements ==
		std::vector<ElementValue>{std::string{"a"}, std::string{}, std::string{"b"}, std::string{}}
	);
	const auto periodic = RunNode(
		"pc.string_split",
		{},
		{{"text", std::string{"aé中z"}}, {"mode", EnumValue{1}}, {"period", int64_t{2}}}
	);
	CHECK(
		std::get<ArrayValue>(NodeValue(periodic)).Elements ==
		std::vector<ElementValue>{std::string{"aé"}, std::string{"中z"}}
	);
	const auto escaped =
		RunNode("pc.string_split", {}, {{"text", std::string{"a\nb"}}, {"delimiter", std::string{"\\n"}}});
	CHECK(std::get<ArrayValue>(NodeValue(escaped)).Elements.size() == 2);
}
TEST_CASE("Text join and trim preserve source modes", "[imagegraph][source_text]") {
	const auto join = RunNode(
		"pc.string_join",
		{},
		{{"text_array", ArrayValue{ValueType::Text, {std::string{"one"}, std::string{}, std::string{"two"}}}},
		 {"divider", std::string{"|"}}}
	);
	CHECK(String(join) == "one||two");
	const auto trim = RunNode(
		"pc.string_trim", {}, {{"text", std::string{"aé中z"}}, {"head", double{1}}, {"tail", double{1}}}
	);
	CHECK(String(trim) == "é中");
	const auto white = RunNode(
		"pc.string_trim",
		{},
		{{"text", std::string{"  ***x*** \n"}}, {"trim", EnumValue{2}}, {"text_2", std::string{"*"}}}
	);
	CHECK(String(white) == "x");
	const auto words = RunNode(
		"pc.string_trim",
		{},
		{{"text", std::string{"one  three four"}},
		 {"trim", EnumValue{1}},
		 {"head", double{1}},
		 {"tail", double{1}}}
	);
	CHECK(String(words) == " three");
}
TEST_CASE("JSON and struct edits retain independent nested fields", "[imagegraph][source_text]") {
	const auto parsed = RunNode(
		"pc.struct_json_parse",
		{},
		{{"json_string", std::string{R"({"child":{"label":"\uD83D\uDE00"},"numbers":[1,2],"active":true})"}}}
	);
	const auto object = std::get<StructValue>(NodeValue(parsed, "struct"));
	const auto get = RunNode("pc.struct_get", {}, {{"struct", object}, {"key", std::string{"child.label"}}});
	CHECK(std::get<std::string>(NodeValue(get, "value")) == "😀");
	const auto missing =
		RunNode("pc.struct_get", {}, {{"struct", object}, {"key", std::string{"child.missing"}}});
	CHECK(std::get<double>(NodeValue(missing, "value")) == 0);
	const auto set = RunNode(
		"pc.struct_set", {}, {{"struct", object}, {"key", std::string{"child"}}, {"value", double{8}}}
	);
	CHECK(std::get<StructValue>(NodeValue(set, "struct")) != object);
	CHECK(
		std::get<std::string>(NodeValue(
			RunNode("pc.struct_get", {}, {{"struct", object}, {"key", std::string{"child.label"}}}), "value"
		)) == "😀"
	);
	const auto invalid = RunNode("pc.struct_json_parse", {}, {{"json_string", std::string{"{invalid"}}});
	CHECK_FALSE(std::get<StructValue>(NodeValue(invalid, "struct")).Data);
	const auto null = RunNode("pc.struct_json_parse", {}, {{"json_string", std::string{"{\"x\":null}"}}});
	REQUIRE(null.Ok);
	const auto &nullObject = std::get<StructValue>(NodeValue(null, "struct"));
	REQUIRE(nullObject.Data);
	REQUIRE(nullObject.Data->Fields.size() == 1);
	CHECK(std::holds_alternative<UndefinedValue>(nullObject.Data->Fields[0].second));
}
TEST_CASE("CSV preserves simple source parsing and typed sorted rows", "[imagegraph][source_text]") {
	const auto run = RunNode(
		"pc.array_csv_parse",
		{},
		{{"csv_string", std::string{"name,n\nb,2\na,1"}},
		 {"first_row_header", true},
		 {"number_columns", ArrayValue{ValueType::Text, {std::string{"n"}}}},
		 {"sort", ArrayValue{ValueType::Text, {std::string{"+n"}}}},
		 {"output_struct", true}}
	);
	const auto &rows = std::get<ArrayValue>(NodeValue(run, "array"));
	REQUIRE(rows.Elements.size() == 2);
	CHECK(rows.ElementType == ValueType::Struct);
	const auto get = RunNode(
		"pc.struct_get",
		{},
		{{"struct", std::get<StructValue>(rows.Elements[0])}, {"key", std::string{"name"}}}
	);
	CHECK(std::get<std::string>(NodeValue(get, "value")) == "a");
	const auto withoutHeader = RunNode("pc.array_csv_parse", {}, {{"csv_string", std::string{"a,1\nb,2"}}});
	CHECK(std::get<ArrayValue>(NodeValue(withoutHeader, "array")).Elements.empty());
}
TEST_CASE("Structured payload rejects excessive nesting across arrays", "[imagegraph][source_text]") {
	StructValue value;
	for (size_t i = 0; i < Limits::MaximumArrayDepth + 1; ++i) {
		StructValue parent;
		parent.Data.emplace().Fields.emplace_back("child", std::move(value));
		value = std::move(parent);
	}
	CHECK_FALSE(engine::imagegraph::detail::ValidStructPayload(value));
	const auto run =
		RunNode("pc.struct_set", {}, {{"struct", value}, {"key", std::string{"x"}}, {"value", double{1}}});
	CHECK_FALSE(run.Ok);
}
TEST_CASE("Dynamic struct and format inputs retain borrowed linked values", "[imagegraph][source_text]") {
	const auto *entry = FindCatalogueEntry("pc.struct");
	REQUIRE(entry);
	Node node{"struct", "pc.struct", "", {}, {}};
	node.DynamicInputs = {
		{"key_0", ValueType::Text, std::nullopt}, {"value_0", ValueType::Any, std::nullopt}
	};
	const EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	const Value key = std::string{"mixed"};
	ArrayValue array{ValueType::Any, {}};
	array.Items = {
		{ElementValue{std::string{"hello"}}}, {std::vector<SourceArrayItem>{{ElementValue{double{7}}}}}
	};
	const Value value = array;
	context.ValueViews = {{"key_0", &key}, {"value_0", &value}};
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.struct");
	REQUIRE(executor);
	REQUIRE(executor(context));
	const auto &object = std::get<StructValue>(context.OutputValues.front().Data);
	REQUIRE(object.Data);
	REQUIRE(object.Data->Fields.size() == 1);
	CHECK(object.Data->Fields.front().second == value);
	const auto get = RunNode("pc.struct_get", {}, {{"struct", object}, {"key", std::string{"mixed"}}});
	CHECK(NodeValue(get, "value") == value);
	const auto *formatEntry = FindCatalogueEntry("pc.string_format");
	REQUIRE(formatEntry);
	Node formatNode{"format", "pc.string_format", "", {}, {}};
	formatNode.DynamicInputs = {
		{"key_in_amo_0", ValueType::Text, std::nullopt}, {"value_0", ValueType::Text, std::nullopt}
	};
	engine::imagegraph::detail::NodeContext format(formatNode, *formatEntry, request);
	format.ByteBudget = Limits::MaximumEvaluationBytes;
	const Value pattern = std::string{"Hello {name}, {name}!"}, name = std::string{"name"},
				replacement = std::string{"grug"};
	format.ValueViews = {{"text", &pattern}, {"key_in_amo_0", &name}, {"value_0", &replacement}};
	REQUIRE(engine::imagegraph::detail::FindExecutor("pc.string_format")(format));
	CHECK(std::get<std::string>(format.OutputValues.front().Data) == "Hello grug, grug!");
}
TEST_CASE(
	"Regex nodes use substring matching capture lists and ECMAScript replacement",
	"[imagegraph][source_text][regex]"
) {
	const auto match = RunNode(
		"pc.string_regex_match", {}, {{"text", std::string{"xabcx"}}, {"regex", std::string{"ab(c)"}}}
	);
	CHECK(std::get<bool>(NodeValue(match, "results")));
	const auto search = RunNode(
		"pc.string_regex_search", {}, {{"text", std::string{"xabcx"}}, {"regex", std::string{"ab(c)"}}}
	);
	CHECK(
		std::get<ArrayValue>(NodeValue(search, "results")).Elements ==
		std::vector<ElementValue>{std::string{"abc"}, std::string{"c"}}
	);
	const auto replace = RunNode(
		"pc.string_regex_replace",
		{},
		{{"text", std::string{"ab ab"}},
		 {"regex", std::string{"(a)(b)"}},
		 {"replacement", std::string{"$2$1-$$-$&"}}}
	);
	CHECK(std::get<std::string>(NodeValue(replace, "results")) == "ba-$-ab ba-$-ab");
	const auto empty = RunNode(
		"pc.string_regex_replace",
		{},
		{{"text", std::string{"bbb"}}, {"regex", std::string{"a*|b"}}, {"replacement", std::string{"x"}}}
	);
	CHECK(std::get<std::string>(NodeValue(empty, "results")) == "xxxxxxx");
	const auto malformed =
		RunNode("pc.string_regex_match", {}, {{"text", std::string{"x"}}, {"regex", std::string{"("}}});
	CHECK_FALSE(malformed.Ok);
	CHECK(malformed.Code == Status::InvalidValue);
}
TEST_CASE("Struct fields retain surfaces buffers and undefined source values", "[imagegraph][source_text]") {
	Image source{1, 1, {12, 34, 56, 255}, 0};
	const auto inserted = RunNode("pc.struct_set", {{"value", &source}}, {{"key", std::string{"image"}}});
	const auto object = std::get<StructValue>(NodeValue(inserted, "struct"));
	REQUIRE(object.Data);
	REQUIRE(object.Data->Fields.size() == 1);
	CHECK(std::get<SurfaceValue>(object.Data->Fields[0].second).Data == source);
	const auto image = RunNode("pc.struct_get", {}, {{"struct", object}, {"key", std::string{"image"}}});
	INFO(image.Message);
	REQUIRE(image.Ok);
	CHECK(image.Output("value").Pixels == source.Pixels);
	const auto bytes = RunNode(
		"pc.struct_set",
		{},
		{{"struct", object}, {"key", std::string{"bytes"}}, {"value", BufferValue{{1, 2, 255}}}}
	);
	const auto buffer =
		RunNode("pc.struct_get", {}, {{"struct", NodeValue(bytes, "struct")}, {"key", std::string{"bytes"}}});
	CHECK(std::get<BufferValue>(NodeValue(buffer, "value")).Bytes == std::vector<uint8_t>{1, 2, 255});
	const auto null = RunNode("pc.struct_json_parse", {}, {{"json_string", std::string{"[{\"key\":null}]"}}});
	const auto lookup =
		RunNode("pc.struct_get", {}, {{"struct", NodeValue(null, "struct")}, {"key", std::string{"key"}}});
	const auto &array = std::get<ArrayValue>(NodeValue(lookup, "value"));
	REQUIRE(array.Elements.size() == 1);
	CHECK(std::get<double>(array.Elements[0]) == 0);
}
