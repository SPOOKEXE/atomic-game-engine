#include "NodeExecutors.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_switch")
using namespace engine::imagegraph;
namespace {
	Document SwitchGraph(bool threshold = false) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {{"switch", threshold ? "pc.threshold_switch" : "pc.switch", "", {}, {}}};
		d.Outputs = {{"out", "switch", "result"}};
		return d;
	}
	void Pair(Document &d, size_t group, Value key, std::optional<Value> payload = std::nullopt) {
		auto &node = d.Nodes[0];
		const bool threshold = node.Type == "pc.threshold_switch";
		const std::string suffix = "_" + std::to_string(group);
		node.DynamicInputs.push_back(
			{(threshold ? "value" : "case") + suffix,
			 threshold ? ValueType::Scalar : ValueType::Text,
			 std::move(key)}
		);
		node.DynamicInputs.push_back(
			{(threshold ? "value_2" : "value") + suffix, ValueType::Any, std::move(payload)}
		);
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diagnostic;
		const auto status = Compile(d, p, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return p;
	}
	Document Restored(const Document &d) {
		Document restored;
		Diagnostic diagnostic;
		const auto status = Read(Write(d), restored, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(restored == d);
		return restored;
	}
	Value Evaluated(Document d, const EvaluationRequest &request = {}) {
		d = Restored(d);
		const auto p = Compiled(d);
		EvaluatedValue value;
		Diagnostic diagnostic;
		const auto status = EvaluateValue(d, p, "out", request, value, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return value.Data;
	}
	void DefaultValue(Document &d, double number) {
		d.Nodes.push_back({"default", "pc.number_simple", "", {}, {{"value", number}}});
		d.Links.push_back({"default", "number", "switch", "default_value"});
	}
	void LinkedKey(Document &d, std::string_view input, double number) {
		d.Nodes.push_back({"number", "pc.number_simple", "", {}, {{"value", number}}});
		d.Links.push_back({"number", "number", "switch", std::string(input)});
	}
}
TEST_CASE("Switch defaults and absent pair payloads retain source numeric zero", "[source_switch]") {
	for (bool threshold : {false, true}) {
		auto d = SwitchGraph(threshold);
		CHECK(Evaluated(d) == Value{double{0}});
		Pair(d, 0, threshold ? Value{0.} : Value{std::string{"hit"}});
		d.Nodes[0].Values = {{"index", threshold ? Value{0.} : Value{std::string{"hit"}}}};
		CHECK(Evaluated(d) == Value{double{0}});
	}
}
TEST_CASE("Switch ignores empty case and selects the last exact string match", "[source_switch]") {
	auto d = SwitchGraph();
	d.Nodes[0].Values = {{"index", std::string{"a"}}};
	DefaultValue(d, 7.);
	Pair(d, 0, std::string{}, 100.);
	Pair(d, 1, std::string{"a"}, 1.);
	Pair(d, 2, std::string{"A"}, 2.);
	Pair(d, 3, std::string{"a"}, 3.);
	CHECK(Evaluated(d) == Value{3.});
	d.Nodes[0].Values[0].Data = std::string{};
	CHECK(Evaluated(d) == Value{7.});
	d.Nodes[0].Values[0].Data = std::string{"é"};
	Pair(d, 4, std::string{"é"}, std::string{"Unicode"});
	CHECK(Evaluated(d) == Value{std::string{"Unicode"}});
}
TEST_CASE("Switch follows source slot order rather than dynamic declaration order", "[source_switch]") {
	auto d = SwitchGraph();
	d.Nodes[0].Values = {{"index", std::string{"x"}}};
	Pair(d, 4, std::string{"x"}, 4.);
	Pair(d, 0, std::string{"x"}, 0.);
	Pair(d, 2, std::string{"x"}, 2.);
	CHECK(Evaluated(d) == Value{4.});
}
TEST_CASE("Switch raw linked numbers use pinned numeric prefix and epsilon comparisons", "[source_switch]") {
	for (const auto &[key, expected] : std::array<std::pair<std::string, double>, 7>{
			 {{"1", 9},
			  {"1tail", 9},
			  {"\xc2\xa0+1", 9},
			  {"1.000005", 9},
			  {"1.00002", 0},
			  {"true", 0},
			  {"", 0}}
		 }) {
		auto d = SwitchGraph();
		Pair(d, 0, key, 9.);
		LinkedKey(d, "index", 1.);
		CHECK(Evaluated(d) == Value{expected});
	}
	auto d = SwitchGraph();
	Pair(d, 0, std::string{"01"}, 9.);
	d.Nodes[0].Values = {{"index", std::string{"1"}}};
	CHECK(Evaluated(d) == Value{0.});
}
TEST_CASE("Switch text getter forwards linked numeric cases without string conversion", "[source_switch]") {
	auto d = SwitchGraph();
	Pair(d, 0, std::string{}, int64_t{44});
	d.Nodes[0].Values = {{"index", std::string{"+12tail"}}};
	LinkedKey(d, "case_0", 12.);
	CHECK(Evaluated(d) == Value{int64_t{44}});
}
TEST_CASE("Threshold Switch is inclusive and unsorted with last qualifying pair winning", "[source_switch]") {
	auto d = SwitchGraph(true);
	d.Nodes[0].Values = {{"index", 8.}};
	DefaultValue(d, -1.);
	Pair(d, 0, 8., 80.);
	Pair(d, 1, 3., 30.);
	Pair(d, 2, 9., 90.);
	Pair(d, 3, 3., 31.);
	CHECK(Evaluated(d) == Value{31.});
	d.Nodes[0].Values[0].Data = 2.;
	CHECK(Evaluated(d) == Value{-1.});
}
TEST_CASE("Threshold Frame mode uses signed fractional frame plus one", "[source_switch]") {
	auto d = SwitchGraph(true);
	d.Nodes[0].Values = {{"type", EnumValue{1}}, {"index", 999.}};
	Pair(d, 0, -.25, -25.);
	Pair(d, 1, .5, 5.);
	Pair(d, 2, 1.25, 125.);
	Pair(d, 3, 1.75, 175.);
	const std::array requests{
		EvaluationRequest{},
		EvaluationRequest{.Subframe = .25},
		EvaluationRequest{.Subframe = .75},
		EvaluationRequest{.Tick = 1, .Subframe = .25, .NegativeFrame = true}
	};
	const std::array<double, 4> expected{5, 125, 175, -25};
	for (size_t i = 0; i < requests.size(); ++i)
		CHECK(Evaluated(d, requests[i]) == Value{expected[i]});
}
TEST_CASE("Threshold Switch numeric epsilon boundary does not sort or round thresholds", "[source_switch]") {
	auto d = SwitchGraph(true);
	d.Nodes[0].Values = {{"index", 1.}};
	Pair(d, 0, 1.000005, 5.);
	Pair(d, 1, 1.00002, 20.);
	CHECK(Evaluated(d) == Value{5.});
}
TEST_CASE("Switch routes nested heterogeneous whole values into downstream array access", "[source_switch]") {
	auto d = SwitchGraph();
	ArrayValue nested{ValueType::Any, {}};
	nested.Items = {
		{ElementValue{-1.}},
		{std::vector<SourceArrayItem>{
			{ElementValue{std::string{"nested"}}}, {ElementValue{UndefinedValue{}}}
		}}
	};
	Pair(d, 0, std::string{"pick"});
	d.Nodes[0].Values = {{"index", std::string{"pick"}}};
	d.Nodes.push_back(
		{"json", "pc.struct_json_parse", "", {}, {{"json_string", std::string{"[-1,[\"nested\",null]]"}}}}
	);
	d.Links.push_back({"json", "struct", "switch", "value_0"});
	CHECK(Evaluated(d) == Value{nested});
	d.Nodes.push_back({"get", "pc.array_get", "", {}, {{"index", int64_t{1}}}});
	d.Links.push_back({"switch", "result", "get", "array"});
	d.Outputs = {{"out", "get", "value"}};
	const auto value = std::get<ArrayValue>(Evaluated(d));
	CHECK(value.ElementType == ValueType::Any);
	REQUIRE(value.Items.size() == 2);
	CHECK(std::get<ElementValue>(value.Items[0].Data) == ElementValue{std::string{"nested"}});
	CHECK(std::holds_alternative<UndefinedValue>(std::get<ElementValue>(value.Items[1].Data)));
}
TEST_CASE(
	"Switch preserves Struct and Undefined payload types through native persistence", "[source_switch]"
) {
	for (bool threshold : {false, true}) {
		auto d = SwitchGraph(threshold);
		Pair(d, 0, threshold ? Value{0.} : Value{std::string{"x"}});
		if (!threshold) d.Nodes[0].Values = {{"index", std::string{"x"}}};
		d.Nodes.push_back(
			{"json",
			 "pc.struct_json_parse",
			 "",
			 {},
			 {{"json_string", std::string{"{\"field\":[1,\"x\",null]}"}}}}
		);
		d.Links.push_back({"json", "struct", "switch", threshold ? "value_2_0" : "value_0"});
		const auto value = std::get<StructValue>(Evaluated(d));
		REQUIRE(value.Data);
		REQUIRE(value.Data->Fields.size() == 1);
		CHECK(value.Data->Fields[0].first == "field");
		d.Nodes[1].Values[0].Data = std::string{"null"};
		CHECK(std::holds_alternative<UndefinedValue>(Evaluated(d)));
	}
}
TEST_CASE("Switch copies selected captured typed surfaces without borrowed lifetime", "[source_switch]") {
	for (bool threshold : {false, true}) {
		auto d = SwitchGraph(threshold);
		Pair(d, 0, threshold ? Value{0.} : Value{std::string{"x"}});
		if (!threshold) d.Nodes[0].Values = {{"index", std::string{"x"}}};
		d.Nodes.push_back({"capture", "image.captured", "", {}, {{"source_id", std::string{"source"}}}});
		d.Links.push_back({"capture", "image", "switch", threshold ? "value_2_0" : "value_0"});
		d = Restored(d);
		const auto p = Compiled(d);
		Image source{1, 1, {0, 0, 0, 63}};
		source.Format = SurfaceFormat::R32Float;
		source.Hash = SurfaceHash(source);
		std::vector<RequestImageSource> captures{{"source", source}};
		EvaluationRequest request;
		request.ImageSources = captures;
		Image output;
		Diagnostic diagnostic;
		const auto status = Evaluate(d, p, "out", request, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(output.Pixels == source.Pixels);
		CHECK(output.Format == source.Format);
		CHECK(output.Hash == source.Hash);
		captures[0].Data.Pixels[0] = 17;
		CHECK(output.Pixels[0] == 0);
	}
}
TEST_CASE(
	"Switch preserves selected nested image array shape and owns all image buffers", "[source_switch]"
) {
	auto d = SwitchGraph();
	Pair(d, 0, std::string{"x"});
	d.Nodes[0].Values = {{"index", std::string{"x"}}};
	d.Nodes.push_back({"capture", "image.captured", "", {}, {{"source_id", std::string{"source"}}}});
	Node inner{"inner", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	inner.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	Node outer{"outer", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	outer.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	d.Nodes.push_back(inner);
	d.Nodes.push_back(outer);
	d.Links = {
		{"capture", "image", "inner", "input_0"},
		{"capture", "image", "inner", "input_1"},
		{"inner", "array", "outer", "input_0"},
		{"capture", "image", "outer", "input_1"},
		{"outer", "array", "switch", "value_0"}
	};
	d = Restored(d);
	const auto p = Compiled(d);
	Image source{1, 1, {11, 22, 33, 44}};
	source.Hash = SurfaceHash(source);
	std::vector<RequestImageSource> captures{{"source", source}};
	EvaluationRequest request;
	request.ImageSources = captures;
	ImageArray output;
	Diagnostic diagnostic;
	const auto status = EvaluateArray(d, p, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Items.size() == 2);
	REQUIRE(std::holds_alternative<std::vector<ImageArrayItem>>(output.Items[0].Data));
	CHECK(std::get<std::vector<ImageArrayItem>>(output.Items[0].Data).size() == 2);
	REQUIRE(output.Images.size() == 3);
	for (const auto &image : output.Images)
		CHECK(image.Pixels == source.Pixels);
	captures[0].Data.Pixels[0] = 99;
	for (const auto &image : output.Images)
		CHECK(image.Pixels[0] == 11);
}
TEST_CASE("Switch refuses unobserved array selector identity atomically", "[source_switch]") {
	auto d = SwitchGraph();
	Pair(d, 0, std::string{"unused"}, 9.);
	for (const auto &id : {"selector", "case"})
		d.Nodes.push_back({id, "pc.equation", "", {}, {{"equation", std::string{"[1,2]"}}}});
	d.Links = {{"selector", "result", "switch", "index"}, {"case", "result", "switch", "case_0"}};
	const auto p = Compiled(d);
	EvaluatedValue value;
	value.Data = int64_t{77};
	Diagnostic diagnostic;
	CHECK(EvaluateValue(d, p, "out", {}, value, diagnostic) == Status::UnsupportedExecution);
	CHECK(value.Data == Value{int64_t{77}});
	CHECK(diagnostic.Message.find("identity") != std::string::npos);
}
TEST_CASE(
	"Threshold Switch refuses array comparison while Frame mode ignores the numeric Index", "[source_switch]"
) {
	auto d = SwitchGraph(true);
	Pair(d, 0, 0., 9.);
	d.Nodes.push_back({"array", "pc.equation", "", {}, {{"equation", std::string{"[1,2]"}}}});
	d.Links = {{"array", "result", "switch", "index"}};
	auto p = Compiled(d);
	EvaluatedValue value;
	value.Data = int64_t{77};
	Diagnostic diagnostic;
	CHECK(EvaluateValue(d, p, "out", {}, value, diagnostic) == Status::UnsupportedExecution);
	CHECK(value.Data == Value{int64_t{77}});
	d.Nodes[0].Values = {{"type", EnumValue{1}}};
	CHECK(Evaluated(d) == Value{9.});
	d.Links[0].ToPort = "value_0";
	p = Compiled(d);
	CHECK(EvaluateValue(d, p, "out", {}, value, diagnostic) == Status::UnsupportedExecution);
	CHECK(value.Data == Value{int64_t{77}});
}
TEST_CASE("Switch enforces pair quota malformed declarations and comparison work bounds", "[source_switch]") {
	auto d = SwitchGraph();
	d.Nodes[0].Values = {{"index", std::string{"x"}}};
	for (size_t i = 0; i < 32; ++i)
		Pair(d, i, std::string{"x"}, double(i));
	CHECK(Evaluated(d) == Value{31.});
	Pair(d, 32, std::string{"x"}, 32.);
	Plan p;
	Diagnostic diagnostic;
	CHECK(Compile(d, p, diagnostic) == Status::LimitExceeded);
	d = SwitchGraph();
	Pair(d, 0, std::string{"x"}, 9.);
	d.Nodes[0].DynamicInputs.pop_back();
	p = Compiled(d);
	EvaluatedValue value;
	value.Data = int64_t{77};
	CHECK(EvaluateValue(d, p, "out", {}, value, diagnostic) == Status::InvalidValue);
	CHECK(value.Data == Value{int64_t{77}});
	d = SwitchGraph();
	d.Nodes[0].Values = {{"index", std::string(Limits::MaximumTextBytes, 'x')}};
	for (size_t i = 0; i < 9; ++i)
		Pair(d, i, std::string(Limits::MaximumTextBytes, 'x'), double(i));
	p = Compiled(d);
	CHECK(EvaluateValue(d, p, "out", {}, value, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Message.find("comparison work") != std::string::npos);
	CHECK(value.Data == Value{int64_t{77}});
}
TEST_CASE(
	"Switch selected clone admission refuses before allocation and preserves caller output", "[source_switch]"
) {
	auto d = SwitchGraph();
	Pair(d, 0, std::string{"x"}, std::string(32768, 'v'));
	d.Nodes[0].Values = {{"index", std::string{"x"}}};
	Compiled(d);
	auto imageGraph = SwitchGraph();
	Pair(imageGraph, 0, std::string{"x"});
	imageGraph.Nodes[0].Values = {{"index", std::string{"x"}}};
	imageGraph.Nodes.push_back({"capture", "image.captured", "", {}, {{"source_id", std::string{"source"}}}});
	imageGraph.Links = {{"capture", "image", "switch", "value_0"}};
	const auto p = Compiled(imageGraph);
	Image source{128, 128, std::vector<uint8_t>(128 * 128 * 4, 19)};
	source.Hash = SurfaceHash(source);
	const std::array captures{RequestImageSource{"source", source}};
	EvaluationRequest imageRequest;
	imageRequest.ImageSources = captures;
	Image output{1, 1, {77, 77, 77, 77}};
	Diagnostic diagnostic;
	CHECK(Evaluate(imageGraph, p, "out", imageRequest, output, diagnostic, 512) == Status::LimitExceeded);
	CHECK(output.Pixels == std::vector<uint8_t>{77, 77, 77, 77});
	const auto *entry = FindCatalogueEntry("pc.switch");
	REQUIRE(entry);
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.switch");
	REQUIRE(executor);
	const EvaluationRequest request;
	engine::imagegraph::detail::NodeContext c(d.Nodes[0], *entry, request);
	c.ByteBudget = 1;
	const Value index = std::string{"x"}, payload = std::string(32768, 'v');
	c.ValueViews = {{"index", &index}, {"case_0", &index}, {"value_0", &payload}};
	CHECK_FALSE(executor(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.OutputValues.empty());
	CHECK(c.OutputImages.empty());
	CHECK(c.OutputImageArrays.empty());
}

TEST_CASE(
	"Switch and Threshold preserve large integer selectors and native opposite-extreme ordering",
	"[source_switch]"
) {
	for (bool threshold : {false, true}) {
		auto d = SwitchGraph(threshold);
		Pair(d, 0, threshold ? Value{0.} : Value{std::string{}}, 9.);
		Node array{"keys", "pc.array", "", {}, {{"type", EnumValue{0}}}};
		array.DynamicInputs = {
			{"input_0", ValueType::Integer, Value{int64_t{9007199254740993}}},
			{"input_1", ValueType::Integer, Value{int64_t{9007199254740992}}}
		};
		d.Nodes.push_back(array);
		d.Nodes.push_back({"selector", "pc.array_get", "", {}, {{"index", int64_t{0}}}});
		d.Nodes.push_back({"key", "pc.array_get", "", {}, {{"index", int64_t{1}}}});
		d.Links = {
			{"keys", "array", "selector", "array"},
			{"keys", "array", "key", "array"},
			{"selector", "value", "switch", "index"},
			{"key", "value", "switch", threshold ? "value_0" : "case_0"}
		};
		CHECK(Evaluated(d) == Value{threshold ? 9. : 0.});
		d.Nodes[1].DynamicInputs[0].Default = Value{std::numeric_limits<int64_t>::max()};
		d.Nodes[1].DynamicInputs[1].Default = Value{std::numeric_limits<int64_t>::min()};
		CHECK(Evaluated(d) == Value{threshold ? 9. : 0.});
		d.Nodes[1].DynamicInputs[1].Default = d.Nodes[1].DynamicInputs[0].Default;
		CHECK(Evaluated(d) == Value{9.});
		for (auto &input : d.Nodes[1].DynamicInputs)
			input.Type = ValueType::Boolean;
		d.Nodes[1].DynamicInputs[0].Default = Value{true};
		d.Nodes[1].DynamicInputs[1].Default = Value{false};
		CHECK(Evaluated(d) == Value{threshold ? 9. : 0.});
		d.Nodes[1].DynamicInputs[1].Default = Value{true};
		CHECK(Evaluated(d) == Value{9.});
	}
}
TEST_CASE(
	"Threshold raw Any text uses source operators and ignored Index needs no coercion", "[source_switch]"
) {
	auto d = SwitchGraph(true);
	Pair(d, 0, 0., 9.);
	d.Nodes.push_back({"key", "pc.equation", "", {}, {{"equation", std::string{"\"1tail\""}}}});
	d.Nodes[0].Values = {{"index", 1.}};
	d.Links = {{"key", "result", "switch", "value_0"}};
	CHECK(Evaluated(d) == Value{9.});
	d.Nodes[1].Values[0].Data = std::string{"\"\""};
	CHECK(Evaluated(d) == Value{0.});
	d.Nodes[1].Values[0].Data = std::string{"\"nonnumeric\""};
	auto p = Compiled(d);
	Diagnostic diagnostic;
	EvaluatedValue value;
	value.Data = int64_t{77};
	CHECK(EvaluateValue(d, p, "out", {}, value, diagnostic) == Status::UnsupportedExecution);
	CHECK(value.Data == Value{int64_t{77}});
	d.Nodes[0].DynamicInputs.clear();
	d.Links[0].ToPort = "index";
	CHECK(Evaluated(d) == Value{0.});
}

TEST_CASE(
	"Threshold raw text ordering and paired Undefined follow source comparison branches", "[source_switch]"
) {
	auto d = SwitchGraph(true);
	Pair(d, 0, 0., 9.);
	d.Nodes.push_back({"selector", "pc.equation", "", {}, {{"equation", std::string{"\"10\""}}}});
	d.Nodes.push_back({"key", "pc.equation", "", {}, {{"equation", std::string{"\"2\""}}}});
	d.Links = {{"selector", "result", "switch", "index"}, {"key", "result", "switch", "value_0"}};
	CHECK(Evaluated(d) == Value{0.});
	d.Nodes[1].Values[0].Data = std::string{"\"z\""};
	d.Nodes[2].Values[0].Data = std::string{"\"a\""};
	CHECK(Evaluated(d) == Value{9.});
	d.Nodes.resize(2);
	d.Nodes[1] = {"selector", "pc.struct_json_parse", "", {}, {{"json_string", std::string{"null"}}}};
	Node box{"box", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	box.DynamicInputs = {{"input_0", ValueType::Any, std::nullopt}};
	d.Nodes.push_back(std::move(box));
	d.Nodes.push_back({"undefined", "pc.array_get", "", {}, {{"index", int64_t{0}}}});
	d.Links = {
		{"selector", "struct", "box", "input_0"},
		{"box", "array", "undefined", "array"},
		{"undefined", "value", "switch", "index"},
		{"undefined", "value", "switch", "value_0"}
	};
	CHECK(Evaluated(d) == Value{9.});
}

TEST_CASE(
	"Threshold raw Unicode text follows UTF16 ordering and refuses malformed UTF8 atomically",
	"[source_switch]"
) {
	auto d = SwitchGraph(true);
	Pair(d, 0, 0., 9.);
	Node array{"keys", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	array.DynamicInputs = {
		{"input_0", ValueType::Text, Value{std::string{"\xf0\x90\x80\x80"}}},
		{"input_1", ValueType::Text, Value{std::string{"\xee\x80\x80"}}}
	};
	d.Nodes.push_back(array);
	d.Nodes.push_back({"selector", "pc.array_get", "", {}, {{"index", int64_t{0}}}});
	d.Nodes.push_back({"key", "pc.array_get", "", {}, {{"index", int64_t{1}}}});
	d.Links = {
		{"keys", "array", "selector", "array"},
		{"keys", "array", "key", "array"},
		{"selector", "value", "switch", "index"},
		{"key", "value", "switch", "value_0"}
	};
	CHECK(Evaluated(d) == Value{0.});
	std::swap(d.Nodes[1].DynamicInputs[0].Default, d.Nodes[1].DynamicInputs[1].Default);
	CHECK(Evaluated(d) == Value{9.});
	d.Nodes[1].DynamicInputs[1].Default = d.Nodes[1].DynamicInputs[0].Default;
	CHECK(Evaluated(d) == Value{9.});
	d.Nodes[1].DynamicInputs[0].Default = Value{std::string{"\xff"}};
	auto p = Compiled(d);
	EvaluatedValue output;
	output.Data = int64_t{77};
	Diagnostic diagnostic;
	CHECK(EvaluateValue(d, p, "out", {}, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output.Data == Value{int64_t{77}});
	CHECK(diagnostic.Message.find("Unicode") != std::string::npos);
}
