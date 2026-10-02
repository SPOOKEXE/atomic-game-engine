#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.dynamic_outputs")

using namespace engine::imagegraph;

TEST_CASE(
	"Array Split dynamic output declarations persist and feed downstream graph inputs",
	"[imagegraph][dynamic_outputs]"
) {
	Document document;
	document.FormatVersion = 9;
	const ArrayValue array{ValueType::Scalar, {2.0, 5.0, 8.0}};
	Node source{
		"source",
		"pc.array",
		"",
		{},
		{{"type", EnumValue{0}}, {"spread_array", true}},
		{{"value_0", ValueType::Array, Value{array}}}
	};
	Node split{"split", "pc.array_split", "", {}, {}};
	split.DynamicOutputs = {{"val_1", ValueType::Any}, {"val_2", ValueType::Any}};
	document.Nodes = {source, split, Node{"capture", "pc.array_copy", "", {}, {}}};
	document.Links = {{"source", "array", "split", "array"}, {"split", "val_1", "capture", "array"}};
	document.Outputs = {{"out", "capture", "array"}};
	Plan plan;
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Document parsed;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(parsed, plan, "capture", {}, snapshot, diagnostic) == Status::Ok);
	bool found = false;
	for (const auto &value : snapshot.Values())
		if (value.Port == "array") {
			CHECK(std::get<double>(value.Data) == 5);
			found = true;
		}
	CHECK(found);
	document.Nodes[1].DynamicOutputs.push_back({"val_1", ValueType::Any});
	CHECK(Compile(document, plan, diagnostic) == Status::DuplicateId);
}

TEST_CASE(
	"Array Split derives additional output values and pads only to source minimum",
	"[imagegraph][dynamic_outputs]"
) {
	const ArrayValue array{ValueType::Scalar, {2.0, 5.0, 8.0}};
	const auto run =
		imagegraph_test::RunNode("pc.array_split", {}, {{"array", array}, {"minimum_outputs", int64_t{5}}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	REQUIRE(run.Values.size() == 5);
	CHECK(std::get<double>(*run.OutputValue("val_0")) == 2);
	CHECK(std::get<double>(*run.OutputValue("val_1")) == 5);
	CHECK(std::get<double>(*run.OutputValue("val_2")) == 8);
	CHECK(std::get<double>(*run.OutputValue("val_4")) == 0);
}

TEST_CASE(
	"Array Split output declarations require a supported source node and document format",
	"[imagegraph][dynamic_outputs]"
) {
	Document document;
	document.FormatVersion = 9;
	Node node{"solid", "image.solid", "", {}, {}};
	node.DynamicOutputs = {{"extra", ValueType::Scalar}};
	document.Nodes = {node};
	document.Outputs = {{"out", "solid", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
}

TEST_CASE(
	"Source channel array modes compile and preserve all four images", "[imagegraph][dynamic_outputs]"
) {
	for (const std::string type : {"pc.rgb_channel", "pc.hsv_channel"}) {
		Document document;
		document.FormatVersion = 9;
		Node source{
			"source",
			"image.solid",
			"",
			{},
			{{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 128}}}
		};
		Node channels{"channels", type, "", {}, {{"output_array", true}}};
		document.Nodes = {source, channels};
		document.Links = {{"source", "image", "channels", "surface_in"}};
		document.Outputs = {{"out", "channels", type == "pc.rgb_channel" ? "red" : "hue"}};
		Plan plan;
		Diagnostic diagnostic;
		INFO(diagnostic.Message);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		ImageArray output;
		REQUIRE(EvaluateArray(document, plan, "out", {}, output, diagnostic) == Status::Ok);
		REQUIRE(output.Images.size() == 4);
		CHECK(output.Images[3].Pixels == std::vector<uint8_t>{255, 255, 255, 128});
		document.Outputs[0].Port = "alpha";
		CHECK(Compile(document, plan, diagnostic) == Status::InvalidOutput);
	}
}

TEST_CASE(
	"Source RGB processor vectors linked scalar arrays before native constructor execution",
	"[imagegraph][dynamic_outputs]"
) {
	Document document;
	document.FormatVersion = 9;
	const ArrayValue array{ValueType::Scalar, {0.0, 1.0}};
	Node source{
		"source",
		"pc.array",
		"",
		{},
		{{"type", EnumValue{0}}, {"spread_array", true}},
		{{"value_0", ValueType::Array, Value{array}}}
	};
	Node color{"color", "pc.color_rgb", "", {}, {{"green", 0.0}, {"blue", 0.0}, {"alpha", 1.0}}};
	document.Nodes = {source, color};
	document.Links = {{"source", "array", "color", "red"}};
	document.Outputs = {{"out", "color", "color"}};
	Plan plan;
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	REQUIRE(EvaluateValue(document, plan, "out", {}, output, diagnostic) == Status::Ok);
	CHECK(
		std::get<ArrayValue>(output.Data).Elements ==
		std::vector<ElementValue>{Colour{0, 0, 0, 255}, Colour{255, 0, 0, 255}}
	);
}

TEST_CASE(
	"Source processor selects mixed numeric general array leaves before validation",
	"[imagegraph][dynamic_outputs]"
) {
	const auto *entry = FindCatalogueEntry("pc.color_rgb");
	REQUIRE(entry);
	Node node{"color", "pc.color_rgb", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	for (const auto &input : entry->Inputs)
		if (auto fallback = CatalogueDefault(input))
			context.Values.emplace_back(
				input.Id, input.Id == "green" || input.Id == "blue" ? Value{0.0} : std::move(*fallback)
			);
	ArrayValue general{ValueType::Any, {}};
	general.Items = {{ElementValue{0.0}}, {ElementValue{int64_t{1}}}};
	Value value = std::move(general);
	context.ValueViews.emplace_back("red", &value);
	REQUIRE(
		engine::imagegraph::detail::RunProcessorBatch(
			context, engine::imagegraph::detail::FindExecutor(node.Type)
		)
	);
	REQUIRE(context.OutputValues.size() == 1);
	CHECK(
		std::get<ArrayValue>(context.OutputValues[0].Data).Elements ==
		std::vector<ElementValue>{Colour{0, 0, 0, 255}, Colour{255, 0, 0, 255}}
	);
}

TEST_CASE(
	"Weighted selector owned state survives native links and preserves source small-array quirk",
	"[imagegraph][dynamic_outputs]"
) {
	Document document;
	document.FormatVersion = 9;
	Node source{
		"source",
		"pc.array",
		"",
		{},
		{{"type", EnumValue{0}}},
		{{"value_0", ValueType::Scalar, Value{10.0}},
		 {"value_1", ValueType::Scalar, Value{20.0}},
		 {"value_2", ValueType::Scalar, Value{30.0}}}
	};
	Node selector{
		"selector",
		"pc.array_randomizer",
		"",
		{},
		{},
		{{"weight_0", ValueType::Scalar, Value{0.0}},
		 {"weight_1", ValueType::Scalar, Value{0.0}},
		 {"weight_2", ValueType::Scalar, Value{1.0}}}
	};
	Node get{"get", "pc.array_get", "", {}, {{"mode", EnumValue{1}}, {"seed", 12345.0}}};
	document.Nodes = {source, selector, get};
	document.Links = {
		{"source", "array", "selector", "array_in"}, {"selector", "array_selector", "get", "array"}
	};
	document.Outputs = {{"out", "get", "value"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	const auto status = EvaluateValue(document, plan, "out", {}, value, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(std::get<double>(value.Data) == 30);
	document.Nodes[0].DynamicInputs.pop_back();
	document.Nodes[1].DynamicInputs.pop_back();
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "out", {}, value, diagnostic) == Status::Ok);
	CHECK(std::get<double>(value.Data) == 10);
}
