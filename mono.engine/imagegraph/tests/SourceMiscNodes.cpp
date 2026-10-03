#include "NodeExecutors.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_misc")
using namespace engine::imagegraph;
namespace {
	Document PinGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {{"pin", "pc.pin", "", {}, {}}};
		document.Outputs = {{"out", "pin", "out"}};
		return document;
	}
	Plan PinCompiled(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	Document PinRestored(const Document &document) {
		Document restored;
		Diagnostic diagnostic;
		const auto status = Read(Write(document), restored, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(restored == document);
		return restored;
	}
	Value PinValue(Document document, const EvaluationRequest &request = {}) {
		document = PinRestored(document);
		auto plan = PinCompiled(document);
		EvaluatedValue output;
		Diagnostic diagnostic;
		const auto status = EvaluateValue(document, plan, "out", request, output, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output.Data;
	}
	void PinJson(Document &document, std::string text) {
		document.Nodes.push_back(
			{"json", "pc.struct_json_parse", "", {}, {{"json_string", std::move(text)}}}
		);
		document.Links.push_back({"json", "struct", "pin", "in"});
	}
}
TEST_CASE("Pin unlinked Any input returns source literal numeric zero", "[source_misc]") {
	CHECK(PinValue(PinGraph()) == Value{0.});
}
TEST_CASE("Pin label controls affect display only and preserve forwarded source value", "[source_misc]") {
	auto d = PinGraph();
	d.Nodes.push_back({"source", "pc.number_simple", "", {}, {{"value", 17.}}});
	d.Links = {{"source", "number", "pin", "in"}};
	for (int64_t position = 0; position < 4; ++position) {
		d.Nodes[0].Values = {
			{"label_position", EnumValue{position}}, {"label_scale", -.5}, {"label_color", Colour{1, 2, 3, 4}}
		};
		CHECK(PinValue(d) == Value{17.});
	}
}
TEST_CASE("Pin forwards complete typed Any leaves without numeric conversion", "[source_misc]") {
	const std::array<Value, 4> values{
		Value{std::numeric_limits<int64_t>::max()},
		Value{true},
		Value{std::string{"text"}},
		Value{Colour{1, 2, 3, 4}}
	};
	const std::array<ValueType, 4> types{
		ValueType::Integer, ValueType::Boolean, ValueType::Text, ValueType::Colour
	};
	for (size_t i = 0; i < values.size(); ++i) {
		auto d = PinGraph();
		Node array{"array", "pc.array", "", {}, {{"type", EnumValue{0}}}};
		array.DynamicInputs = {{"input_0", types[i], values[i]}};
		d.Nodes.push_back(array);
		d.Nodes.push_back({"get", "pc.array_get", "", {}, {{"index", int64_t{0}}}});
		d.Links = {{"array", "array", "get", "array"}, {"get", "value", "pin", "in"}};
		CHECK(PinValue(d) == values[i]);
	}
}
TEST_CASE("Pin routes heterogeneous nested arrays into actual downstream array access", "[source_misc]") {
	auto d = PinGraph();
	PinJson(d, "[-1,[\"nested\",null]]");
	ArrayValue expected{ValueType::Any, {}};
	expected.Items = {
		{ElementValue{-1.}},
		{std::vector<SourceArrayItem>{
			{ElementValue{std::string{"nested"}}}, {ElementValue{UndefinedValue{}}}
		}}
	};
	CHECK(PinValue(d) == Value{expected});
	d.Nodes.push_back({"get", "pc.array_get", "", {}, {{"index", int64_t{1}}}});
	d.Links.push_back({"pin", "out", "get", "array"});
	d.Outputs = {{"out", "get", "value"}};
	const auto value = std::get<ArrayValue>(PinValue(d));
	REQUIRE(value.Items.size() == 2);
	CHECK(std::get<ElementValue>(value.Items[0].Data) == ElementValue{std::string{"nested"}});
	CHECK(std::holds_alternative<UndefinedValue>(std::get<ElementValue>(value.Items[1].Data)));
}
TEST_CASE(
	"Pin forwards Struct and Undefined from real typed producer with native graph persistence",
	"[source_misc]"
) {
	auto d = PinGraph();
	PinJson(d, "{\"field\":[1,null]}");
	const auto value = std::get<StructValue>(PinValue(d));
	REQUIRE(value.Data);
	REQUIRE(value.Data->Fields.size() == 1);
	CHECK(value.Data->Fields[0].first == "field");
	d.Nodes[1].Values[0].Data = std::string{"null"};
	CHECK(std::holds_alternative<UndefinedValue>(PinValue(d)));
}
TEST_CASE("Pin resolves authored animation upstream without own temporal state", "[source_misc]") {
	auto d = PinGraph();
	d.Nodes.push_back({"source", "pc.number_simple", "", {}, {{"value", 0.}}});
	d.Links = {{"source", "number", "pin", "in"}};
	d.Timeline = TimelineSettings{5, 0, 4, "loop", 30};
	d.Keyframes = {{"source", "value", 0, 2., "linear"}, {"source", "value", 4, 10., "linear"}};
	d.Nodes[1].SourceAnimatedInputs = {"value"};
	CHECK(PinValue(d, EvaluationRequest{.Tick = 2}) == Value{6.});
	CHECK(PinValue(d, EvaluationRequest{.Tick = 1, .Subframe = .5}) == Value{5.});
	CHECK(PinValue(d, EvaluationRequest{.Tick = 0}) == Value{2.});
}
TEST_CASE("Pin preserves captured surface format bytes hash and owned lifetime", "[source_misc]") {
	for (auto format :
		 {SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		auto d = PinGraph();
		d.Nodes.push_back({"capture", "image.captured", "", {}, {{"source_id", std::string{"source"}}}});
		d.Links = {{"capture", "image", "pin", "in"}};
		d = PinRestored(d);
		const auto plan = PinCompiled(d);
		Image source;
		source.Width = 1;
		source.Height = 1;
		source.Format = format;
		source.Pixels.resize(
			format == SurfaceFormat::R8Unorm	? 1
			: format == SurfaceFormat::R16Float ? 2
												: 4,
			0
		);
		source.Hash = SurfaceHash(source);
		std::array captures{RequestImageSource{"source", source}};
		EvaluationRequest request;
		request.ImageSources = captures;
		Image output;
		Diagnostic diagnostic;
		REQUIRE(Evaluate(d, plan, "out", request, output, diagnostic) == Status::Ok);
		CHECK(output.Pixels == source.Pixels);
		CHECK(output.Format == source.Format);
		CHECK(output.Hash == source.Hash);
		captures[0].Data.Pixels[0] = 7;
		CHECK(output.Pixels[0] == 0);
	}
}
TEST_CASE("Pin preserves nested image array shape and independently owns every frame", "[source_misc]") {
	auto d = PinGraph();
	d.Nodes.push_back({"capture", "image.captured", "", {}, {{"source_id", std::string{"source"}}}});
	Node inner{"inner", "pc.array", "", {}, {{"type", EnumValue{1}}}},
		outer{"outer", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	inner.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	outer.DynamicInputs = inner.DynamicInputs;
	d.Nodes.push_back(inner);
	d.Nodes.push_back(outer);
	d.Links = {
		{"capture", "image", "inner", "input_0"},
		{"capture", "image", "inner", "input_1"},
		{"inner", "array", "outer", "input_0"},
		{"capture", "image", "outer", "input_1"},
		{"outer", "array", "pin", "in"}
	};
	d = PinRestored(d);
	auto plan = PinCompiled(d);
	Image source{1, 1, {11, 22, 33, 44}};
	source.Hash = SurfaceHash(source);
	std::array captures{RequestImageSource{"source", source}};
	EvaluationRequest request;
	request.ImageSources = captures;
	ImageArray output;
	Diagnostic diagnostic;
	REQUIRE(EvaluateArray(d, plan, "out", request, output, diagnostic) == Status::Ok);
	REQUIRE(output.Items.size() == 2);
	REQUIRE(output.Images.size() == 3);
	REQUIRE(std::get<std::vector<ImageArrayItem>>(output.Items[0].Data).size() == 2);
	captures[0].Data.Pixels[0] = 99;
	for (const auto &image : output.Images)
		CHECK(image.Pixels == source.Pixels);
}
TEST_CASE(
	"Pin image clone refusal preserves caller output and borrowed value admission precedes allocation",
	"[source_misc]"
) {
	auto d = PinGraph();
	d.Nodes.push_back({"capture", "image.captured", "", {}, {{"source_id", std::string{"source"}}}});
	d.Links = {{"capture", "image", "pin", "in"}};
	auto plan = PinCompiled(d);
	Image source{128, 128, std::vector<uint8_t>(128 * 128 * 4, 19)};
	source.Hash = SurfaceHash(source);
	const std::array captures{RequestImageSource{"source", source}};
	EvaluationRequest request;
	request.ImageSources = captures;
	Image output{1, 1, {77, 77, 77, 77}};
	Diagnostic diagnostic;
	CHECK(Evaluate(d, plan, "out", request, output, diagnostic, 512) == Status::LimitExceeded);
	CHECK(output.Pixels == std::vector<uint8_t>{77, 77, 77, 77});
	const auto *entry = FindCatalogueEntry("pc.pin");
	REQUIRE(entry);
	const auto execute = detail::FindExecutor("pc.pin");
	REQUIRE(execute);
	detail::NodeContext context(d.Nodes[0], *entry, request);
	context.ByteBudget = 1;
	const Value payload = std::string(32768, 'v');
	context.ValueViews = {{"in", &payload}};
	CHECK_FALSE(execute(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
}
TEST_CASE(
	"Pin unknown authored controls are rejected rather than silently becoming evaluator features",
	"[source_misc]"
) {
	auto d = PinGraph();
	d.Nodes[0].Values = {{"invented", 3.}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(d, plan, diagnostic) != Status::Ok);
	CHECK(diagnostic.NodeId == "pin");
	CHECK(diagnostic.Port == "invented");
}
