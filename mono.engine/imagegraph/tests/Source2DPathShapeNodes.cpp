#include "../src/nodes/Path.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <numbers>
TEST_SUITE_ID("engine.imagegraph.source_path_shape")
using namespace engine::imagegraph;
TEST_CASE("Shape Path builds every source family with owned sampled geometry", "[imagegraph][source_2d]") {
	constexpr std::array<size_t, 16> COUNTS{4, 4, 4, 64, 64, 64, 128, 64, 4, 8, 4, 129, 2, 64, 64, 64};
	size_t shapeIndex = 0;
	for (const std::string shape :
		 {"Rectangle",
		  "Trapezoid",
		  "Parallelogram",
		  "Ellipse",
		  "Arc",
		  "Squircle",
		  "Hypocycloid",
		  "Epitrochoid",
		  "Polygon",
		  "Star",
		  "Star Draw",
		  "Twist",
		  "Line",
		  "Curve",
		  "Spiral",
		  "Spiral Circle"}) {
		const auto run = imagegraph_test::RunNode(
			"pc.path_shape",
			{},
			{{"shape", shape},
			 {"position", Vector2{2, 3}},
			 {"position_unit", EnumValue{0}},
			 {"half_size", Vector2{4, 2}},
			 {"half_size_unit", EnumValue{0}},
			 {"inner_radius", 1.},
			 {"revolution", 1.},
			 {"corner_radius_unit", EnumValue{0}}}
		);
		INFO(shape);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const Value *value = run.OutputValue("path_data");
		REQUIRE(value);
		const auto &path = std::get<Path2D>(*value);
		REQUIRE(path.SourceOperation);
		CHECK(path.SourceOperation->Kind == SourcePathOperationKind::Shape);
		REQUIRE(path.SourceOperation->Shape);
		CHECK(path.SourceOperation->Shape->Points.size() == COUNTS[shapeIndex++]);
		CHECK(path.SourceOperation->Shape->Position == Vector2{2, 3});
		CHECK(path.SourceOperation->Shape->HalfSize == Vector2{4, 2});
		CHECK(detail::ValidSourcePath2D(path));
	}
}
TEST_CASE(
	"Shape Path preserves analytic ratio sampling independently of chord distance", "[imagegraph][source_2d]"
) {
	const auto run = imagegraph_test::RunNode(
		"pc.path_shape",
		{},
		{{"shape", std::string{"Ellipse"}},
		 {"position", Vector2{2, 3}},
		 {"position_unit", EnumValue{0}},
		 {"half_size", Vector2{4, 2}},
		 {"half_size_unit", EnumValue{0}},
		 {"rotation", 90.}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &path = std::get<Path2D>(*run.OutputValue("path_data"));
	Node node{"sample", "pc.path_shape", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, path));
	CHECK(runtime.LineCount() == 1);
	CHECK(runtime.SegmentCount() == 65);
	CHECK(runtime.MinX == -2);
	CHECK(runtime.MinY == 1);
	CHECK(runtime.MaxX == 6);
	CHECK(runtime.MaxY == 5);
	const auto point = runtime.PointRatio(.125);
	const double diagonal = std::sqrt(.5);
	CHECK(std::abs(point.X - (2 - 2 * diagonal)) < 1e-9);
	CHECK(std::abs(point.Y - (3 - 4 * diagonal)) < 1e-9);
	const auto distance = runtime.PointDistance(runtime.Length() * .125);
	CHECK(std::hypot(point.X - distance.X, point.Y - distance.Y) > .1);
	CHECK(std::abs(runtime.PointRatio(-.875).X - point.X) < 1e-9);
}
TEST_CASE(
	"Rounded Shape Path corners retain the source sample count and point order", "[imagegraph][source_2d]"
) {
	const auto run = imagegraph_test::RunNode(
		"pc.path_shape",
		{},
		{{"shape", std::string{"Rectangle"}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"half_size", Vector2{2, 2}},
		 {"half_size_unit", EnumValue{0}},
		 {"corner_radius", Vector4{1, 0, 0, 0}},
		 {"corner_radius_unit", EnumValue{0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &shape = *std::get<Path2D>(*run.OutputValue("path_data")).SourceOperation->Shape;
	REQUIRE(shape.Points.size() == 68);
	CHECK(std::abs(shape.Points.front().X + 2) < 1e-9);
	CHECK(std::abs(shape.Points.front().Y + 1) < 1e-9);
	CHECK(std::abs(shape.Points[64].X + 1) < 1e-9);
	CHECK(std::abs(shape.Points[64].Y + 2) < 1e-9);
	CHECK(shape.Points[65] == Vector2{2, -2});
	CHECK(shape.Points[66] == Vector2{2, 2});
	CHECK(shape.Points[67] == Vector2{-2, 2});
}

TEST_CASE(
	"Shape Path custom and cached pitch curves retain constructor controls", "[imagegraph][source_2d]"
) {
	Curve quarter;
	quarter.Header = {0, 1, 0, 0, 0, 1};
	quarter.Anchors = {{0, 0, 0, .25, 0, 0}, {0, 0, 1, .25, 0, 0}};
	const auto custom = imagegraph_test::RunNode(
		"pc.path_shape",
		{},
		{{"shape", std::string{"Curve"}},
		 {"position", Vector2{2, 3}},
		 {"position_unit", EnumValue{0}},
		 {"half_size", Vector2{4, 2}},
		 {"half_size_unit", EnumValue{0}},
		 {"curve_eq", EnumValue{2}},
		 {"custom_curve", quarter},
		 {"factor", 4.}}
	);
	INFO(custom.Message);
	REQUIRE(custom.Ok);
	const auto &points = std::get<Path2D>(*custom.OutputValue("path_data")).SourceOperation->Shape->Points;
	REQUIRE(points.size() == 64);
	for (const auto point : points)
		CHECK(std::abs(point.Y - 2) < 1e-9);
	Curve half = quarter;
	half.Anchors[0][3] = .5;
	half.Anchors[1][3] = .5;
	const auto spiral = imagegraph_test::RunNode(
		"pc.path_shape",
		{},
		{{"shape", std::string{"Spiral"}},
		 {"position", Vector2{2, 3}},
		 {"position_unit", EnumValue{0}},
		 {"half_size", Vector2{4, 2}},
		 {"half_size_unit", EnumValue{0}},
		 {"revolution", 2.},
		 {"pitch", .2},
		 {"fixed_pitch", true},
		 {"pitch_curved", true},
		 {"pitch_curve", half}}
	);
	INFO(spiral.Message);
	REQUIRE(spiral.Ok);
	const auto &spiralPoints =
		std::get<Path2D>(*spiral.OutputValue("path_data")).SourceOperation->Shape->Points;
	REQUIRE(spiralPoints.size() == 128);
	CHECK(std::abs(spiralPoints.front().X - 2.4) < 1e-9);
	CHECK(std::abs(spiralPoints.front().Y - 3) < 1e-9);
	for (const auto point : spiralPoints)
		CHECK(std::abs(std::hypot((point.X - 2) / 4, (point.Y - 3) / 2) - .1) < .0001);
}

TEST_CASE(
	"Shape Path feeds the source sampler and preserves results under byte refusal", "[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"shape",
		 "pc.path_shape",
		 "",
		 {},
		 {{"shape", std::string{"Line"}},
		  {"position", Vector2{0, 0}},
		  {"position_unit", EnumValue{0}},
		  {"half_size", Vector2{2, 9}},
		  {"half_size_unit", EnumValue{0}}}},
		{"sample", "pc.path_sample", "", {}, {{"ratio", .25}, {"type", EnumValue{2}}}}
	};
	document.Links = {{"shape", "path_data", "sample", "path"}};
	document.Outputs = {{"position", "sample", "position"}, {"path", "shape", "path_data"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	const auto status = EvaluateValue(document, plan, "position", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(std::get<Vector2>(result.Data) == Vector2{-1, 0});
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "sample", {}, snapshot, diagnostic) == Status::Ok);
	const auto findPath = [&]() -> const Value * {
		for (const auto &input : snapshot.Values())
			if (input.Port == "path") return &input.Data;
		return nullptr;
	};
	const Value *previous = findPath();
	REQUIRE(previous);
	const Value saved = *previous;
	CHECK(EvaluateNodeInputs(document, plan, "sample", {}, snapshot, diagnostic, 1) == Status::LimitExceeded);
	REQUIRE(findPath());
	CHECK(*findPath() == saved);
}

TEST_CASE("Shape Path applies lengthdir snapping before translation", "[imagegraph][source_2d]") {
	const auto run = imagegraph_test::RunNode(
		"pc.path_shape",
		{},
		{{"shape", std::string{"Ellipse"}},
		 {"position", Vector2{.25, .5}},
		 {"position_unit", EnumValue{0}},
		 {"half_size", Vector2{1.00005, 1.00005}},
		 {"half_size_unit", EnumValue{0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &points = std::get<Path2D>(*run.OutputValue("path_data")).SourceOperation->Shape->Points;
	CHECK(points[0] == Vector2{1.25, .5});
	CHECK(points[16] == Vector2{.25, -.5});
}
TEST_CASE(
	"Shape Path admits counts before allocation and rejects unknown fractional indexing",
	"[imagegraph][source_2d]"
) {
	const auto bounded = imagegraph_test::RunNode(
		"pc.path_shape", {}, {{"shape", std::string{"Polygon"}}, {"sides", int64_t{1024}}}
	);
	INFO(bounded.Message);
	REQUIRE(bounded.Ok);
	CHECK(std::get<Path2D>(*bounded.OutputValue("path_data")).SourceOperation->Shape->Points.size() == 1024);
	const auto oversized = imagegraph_test::RunNode(
		"pc.path_shape", {}, {{"shape", std::string{"Polygon"}}, {"sides", int64_t{1025}}}
	);
	CHECK(oversized.Code == Status::LimitExceeded);
	const auto fractional = imagegraph_test::RunNode(
		"pc.path_shape", {}, {{"shape", std::string{"Spiral"}}, {"revolution", 1.01}}
	);
	CHECK(fractional.Code == Status::UnsupportedExecution);
}

TEST_CASE("Shape Path preserves exact source zero and half-turn rotations", "[imagegraph][source_2d]") {
	for (const double rotation : {0., 180.}) {
		const auto run = imagegraph_test::RunNode(
			"pc.path_shape",
			{},
			{{"shape", std::string{"Line"}},
			 {"position", Vector2{1e16, 1e16}},
			 {"position_unit", EnumValue{0}},
			 {"half_size", Vector2{4, 2}},
			 {"half_size_unit", EnumValue{0}},
			 {"rotation", rotation}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto &points = std::get<Path2D>(*run.OutputValue("path_data")).SourceOperation->Shape->Points;
		const double direction = rotation == 0 ? -1 : 1;
		CHECK(points[0] == Vector2{1e16 + direction * 4, 1e16});
		CHECK(points[1] == Vector2{1e16 - direction * 4, 1e16});
	}
}
