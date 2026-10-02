#include "NodeHarness.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
TEST_SUITE_ID("engine.imagegraph.source_spatial_points")
using namespace engine::imagegraph;
namespace {
	Document SpatialScatterDocument(int64_t shape = 0) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes.push_back(
			{"scatter",
			 "pc.scatter_points_3_d",
			 "",
			 {},
			 {{"seed", 12345.0}, {"shape", EnumValue{shape}}, {"amount", int64_t{3}}}}
		);
		document.Outputs = {{"points", "scatter", "points"}};
		return document;
	}
	Vector3 SpatialResult(const EvaluatedValue &value, size_t index) {
		return std::get<Vector3>(std::get<ArrayValue>(value.Data).Elements.at(index));
	}
	void SpatialClose(Vector3 actual, Vector3 expected) {
		CHECK(std::abs(actual.X - expected.X) < 1e-13);
		CHECK(std::abs(actual.Y - expected.Y) < 1e-13);
		CHECK(std::abs(actual.Z - expected.Z) < 1e-13);
	}
} // namespace
TEST_CASE(
	"Source 3D scatter graph preserves official HTML5 cube and sphere "
	"draw sequences",
	"[imagegraph][source_spatial_points]"
) {
	// Literal vectors were evaluated by the complete official Function_Maths.js
	// runtime. These verify that RNG profile and the pinned source call order,
	// not the licensed desktop runner.
	constexpr std::array<Vector3, 3> cube{
		{{.5980197403570728, -.968251465804992, .7012718011165373},
		 {-.40084680141920537, -.07954860715174517, -.6930258254022458},
		 {.27429381631049043, -.6714398878027871, -.9012026404501883}}
	};
	constexpr std::array<Vector3, 3> sphere{
		{{.24485942736621313, -.39645095882946096, .2871360258892787},
		 {.11577173817902642, .03598411452732016, -.8455147424533714},
		 {-.49820845298951166, -.6686924346988247, .4957062410401517}}
	};
	for (int64_t shape = 0; shape < 2; shape++) {
		Document document = SpatialScatterDocument(shape);
		Plan plan;
		Diagnostic diagnostic;
		const Status compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue value;
		const Status evaluated = EvaluateValue(document, plan, "points", {}, value, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		REQUIRE(std::get<ArrayValue>(value.Data).Elements.size() == 3);
		for (size_t index = 0; index < 3; index++)
			SpatialClose(SpatialResult(value, index), (shape ? sphere : cube)[index]);
		const Value reference = value.Data;
		EvaluationRequest replay;
		replay.Tick = 42;
		replay.Subframe = .25;
		replay.NegativeFrame = true;
		REQUIRE(EvaluateValue(document, plan, "points", replay, value, diagnostic) == Status::Ok);
		CHECK(value.Data == reference);
		REQUIRE(EvaluateValue(document, plan, "points", {}, value, diagnostic) == Status::Ok);
		CHECK(value.Data == reference);
		Document restored;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
	}
}
TEST_CASE(
	"Source 3D scatter to point camera graph retains view coordinates "
	"and orthographic aspect",
	"[imagegraph][source_spatial_points]"
) {
	Document document = SpatialScatterDocument();
	document.Nodes[0].Values.push_back({"center", Vector3{2, 0, 0}});
	document.Nodes[0].Values.push_back({"half_size", Vector3{0, 0, 0}});
	document.Nodes.push_back(
		{"camera",
		 "pc.point_3_d_camera",
		 "",
		 {},
		 {{"postioning_mode", EnumValue{0}},
		  {"projection", EnumValue{1}},
		  {"orthographic_scale", .5},
		  {"dimension", Vector2{2, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"range_from_unit", EnumValue{0}},
		  {"range_to_unit", EnumValue{0}},
		  {"depth_from_unit", EnumValue{0}},
		  {"depth_to_unit", EnumValue{0}},
		  {"roll", 90.0}}}
	);
	document.Links = {{"scatter", "points", "camera", "points"}};
	document.Outputs = {{"points", "camera", "points"}};
	Plan plan;
	Diagnostic diagnostic;
	const Status compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluatedValue value;
	const Status evaluated = EvaluateValue(document, plan, "points", {}, value, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	REQUIRE(std::get<ArrayValue>(value.Data).Elements.size() == 3);
	for (size_t i = 0; i < 3; i++)
		SpatialClose(SpatialResult(value, i), {1, 1.5, 2});
	// The inherited anchor and scale controls and Roll are unused by the pinned
	// point projector.
	document.Nodes.back().Values.push_back({"anchor", Vector3{100, 200, 300}});
	document.Nodes.back().Values.push_back({"scale", Vector3{7, 8, 9}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "points", {}, value, diagnostic) == Status::Ok);
	SpatialClose(SpatialResult(value, 0), {1, 1.5, 2});
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(restored, plan, "points", {}, value, diagnostic) == Status::Ok);
	SpatialClose(SpatialResult(value, 0), {1, 1.5, 2});
}
TEST_CASE(
	"Source point camera perspective remaps the projected depth through the clipping range",
	"[imagegraph][source_spatial_points]"
) {
	ArrayValue input{ValueType::Vector3, {Vector3{2, 0, 0}}};
	auto run = imagegraph_test::RunNode(
		"pc.point_3_d_camera",
		{},
		{{"points", input},
		 {"postioning_mode", EnumValue{0}},
		 {"projection", EnumValue{0}},
		 {"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"range_from_unit", EnumValue{0}},
		 {"range_to_unit", EnumValue{0}},
		 {"depth_from", Vector2{1, 10}},
		 {"depth_from_unit", EnumValue{0}},
		 {"depth_to_unit", EnumValue{0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const Value *value = run.OutputValue("points");
	REQUIRE(value);
	const auto &points = std::get<ArrayValue>(*value);
	REQUIRE(points.Elements.size() == 1);
	SpatialClose(std::get<Vector3>(points.Elements[0]), {1, .75, -4.0 / 81});
}
TEST_CASE(
	"Source spatial point processors reject invalid count and singular "
	"projection atomically",
	"[imagegraph][source_spatial_points]"
) {
	Document document = SpatialScatterDocument();
	document.Nodes[0].Values.push_back({"amount", int64_t{-1}});
	// Replace rather than duplicate a durable input key.
	document.Nodes[0].Values.erase(document.Nodes[0].Values.begin() + 2);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	CHECK(EvaluateValue(document, plan, "points", {}, value, diagnostic) == Status::LimitExceeded);
	document = SpatialScatterDocument();
	document.Nodes.push_back(
		{"camera",
		 "pc.point_3_d_camera",
		 "",
		 {},
		 {{"depth_from", Vector2{1, 1}}, {"depth_from_unit", EnumValue{0}}}}
	);
	document.Links = {{"scatter", "points", "camera", "points"}};
	document.Outputs = {{"points", "camera", "points"}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateValue(document, plan, "points", {}, value, diagnostic) == Status::InvalidValue);
}
TEST_CASE(
	"Source point camera preserves skipped scalar rows in a bounded general array",
	"[imagegraph][source_spatial_points]"
) {
	ArrayValue input;
	input.ElementType = ValueType::Any;
	input.Items = {{{ElementValue{Vector3{2, 0, 0}}}}, {{ElementValue{double{99}}}}};
	auto run = imagegraph_test::RunNode(
		"pc.point_3_d_camera",
		{},
		{{"points", input},
		 {"postioning_mode", EnumValue{0}},
		 {"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"range_from_unit", EnumValue{0}},
		 {"range_to_unit", EnumValue{0}},
		 {"depth_from_unit", EnumValue{0}},
		 {"depth_to_unit", EnumValue{0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const Value *value = run.OutputValue("points");
	REQUIRE(value);
	const auto &output = std::get<ArrayValue>(*value);
	REQUIRE(output.Items.size() == 2);
	SpatialClose(std::get<Vector3>(std::get<ElementValue>(output.Items[0].Data)), {1, 1.5, 2});
	CHECK(std::holds_alternative<UndefinedValue>(std::get<ElementValue>(output.Items[1].Data)));
	CHECK(engine::imagegraph::detail::ValidRuntimeValue(*value));
	Node node{"camera", "pc.point_3_d_camera", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *FindCatalogueEntry(node.Type), request);
	context.ByteBudget = 16;
	context.Values = {{"points", input}};
	REQUIRE(engine::imagegraph::detail::FindExecutor(node.Type));
	CHECK_FALSE(engine::imagegraph::detail::FindExecutor(node.Type)(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
}
