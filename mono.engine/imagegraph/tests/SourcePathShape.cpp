#include "../src/SourcePathPayload3D.hpp"
#include "../src/SourcePathShapeCodec.hpp"
#include "../src/nodes/Path.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sstream>

TEST_SUITE_ID("engine.imagegraph.source_path_shape_payload")
using namespace engine::imagegraph;
namespace {
	Path2D ShapeLine() {
		Path2D path;
		auto &operation = path.SourceOperation.emplace();
		operation.Kind = SourcePathOperationKind::Shape;
		auto &shape = operation.Shape.emplace();
		shape.Kind = SourcePathShapeKind2D::Line;
		shape.Points = {{0, 0}, {10, 0}};
		shape.Loop = false;
		shape.HalfSize = {5, 0};
		return path;
	}
} // namespace
TEST_CASE(
	"Shape path sampled geometry survives owned cloning and native graph "
	"serialization",
	"[imagegraph][source_path]"
) {
	const auto original = ShapeLine();
	auto clone = original;
	clone.SourceOperation->Shape->Points[0].X = 99;
	CHECK(original.SourceOperation->Shape->Points[0].X == 0);
	CHECK(detail::SourcePath2DBytes<false>(original) == sizeof(SourcePathData2D) + 2 * sizeof(Vector2));
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"sample", "pc.path_sample", "", {}, {{"path", original}, {"ratio", .5}}}};
	document.Outputs = {{"position", "sample", "position"}};
	Document restored;
	Diagnostic diagnostic;
	const auto encoded = Write(document);
	CHECK(encoded.find("po shape \"Line\"") != std::string::npos);
	REQUIRE(Read(encoded, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(restored, plan, "position", {}, value, diagnostic) == Status::Ok);
	CHECK(std::get<Vector2>(value.Data) == Vector2{5, 0});
	const auto before = restored;
	std::string malformed = encoded;
	const std::string prefix = "po shape \"Line\" 0 2 ";
	const auto offset = malformed.find(prefix);
	REQUIRE(offset != std::string::npos);
	malformed.replace(
		offset, prefix.size(), "po shape \"Line\" 0 " + std::to_string(Limits::MaximumPathAnchors + 1) + " "
	);
	CHECK(Read(malformed, restored, diagnostic) == Status::Malformed);
	CHECK(restored == before);
}
TEST_CASE(
	"Shape codec bounds geometry before allocating and refuses unknown "
	"source names",
	"[imagegraph][source_path]"
) {
	for (const auto &stored :
		 {std::string{"\"Unknown\" 1 2 0 0 1 1 0 90 0 4 0 0 1 1"},
		  std::string{"\"Line\" 0 "} + std::to_string(Limits::MaximumPathAnchors + 1) + " 0 0 1 1 0 90 0 4",
		  std::string{"\"Line\" 2 2 0 0 1 1 0 90 0 4 0 0 1 1"}}) {
		std::istringstream stream{stored};
		SourcePathShapeData2D shape;
		bool admitted = false;
		CHECK_FALSE(detail::ReadSourcePathShape(stream, shape, [&](uint64_t) {
			admitted = true;
			return true;
		}));
		CHECK_FALSE(admitted);
		CHECK(shape.Points.empty());
	}
	std::ostringstream encoded;
	detail::WriteSourcePathShape(encoded, *ShapeLine().SourceOperation->Shape);
	std::istringstream stream{encoded.str()};
	SourcePathShapeData2D shape;
	CHECK_FALSE(detail::ReadSourcePathShape(stream, shape, [](uint64_t) { return false; }));
	CHECK(shape.Points.empty());
}
TEST_CASE(
	"Shape line sampler preserves source distance endpoints and "
	"degenerate behavior",
	"[imagegraph][source_path]"
) {
	const Node node{"sampler", "pc.path_sample", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const EvaluationRequest request;
	detail::NodeContext context{node, *entry, request};
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime runtime;
	auto path = ShapeLine();
	REQUIRE(runtime.Init(context, path));
	CHECK(runtime.SegmentCount() == 2);
	CHECK(runtime.LengthAccumulated == std::vector<double>{10, 10});
	CHECK(runtime.PointRatio(1).X == Catch::Approx(9.9));
	CHECK(runtime.PointDistance(-2).X == 8);
	CHECK(runtime.PointDistance(100).X == Catch::Approx(9.9));
	path.SourceOperation->Shape->Points[1].X = .05;
	REQUIRE(runtime.Init(context, path));
	CHECK(runtime.PointDistance(.025).X == Catch::Approx(-.05));
	path.SourceOperation->Shape->Points[1].X = 0;
	REQUIRE(runtime.Init(context, path));
	CHECK(runtime.PointDistance(1).X == 0);
	CHECK(runtime.PointDistance(1).Y == 0);
}
TEST_CASE(
	"Shape analytic undefined samples do not reject finite chord geometry", "[imagegraph][source_path]"
) {
	auto path = ShapeLine();
	path.SourceOperation->Shape->Kind = SourcePathShapeKind2D::Squircle;
	path.SourceOperation->Shape->Factor = 0;
	CHECK(detail::ValidSourcePath2D(path));
	const Node node{"sampler", "pc.path_sample", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const EvaluationRequest request;
	detail::NodeContext context{node, *entry, request};
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, path));
	CHECK(runtime.PointDistance(5).X == 5);
	CHECK(context.FailureCode == Status::Ok);
	CHECK_FALSE(std::isfinite(runtime.PointRatio(.125).X));
	CHECK(context.FailureCode == Status::InvalidValue);
}
TEST_CASE("Shape replacement byte refusal leaves its existing runtime intact", "[imagegraph][source_path]") {
	const Node node{"sampler", "pc.path_sample", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const EvaluationRequest request;
	detail::EvaluationBudget budget{65536};
	detail::NodeContext context{node, *entry, request, budget};
	context.ByteBudget = 65536;
	detail::PathRuntime runtime;
	const auto first = ShapeLine();
	REQUIRE(runtime.Init(context, first));
	const auto oldBytes = budget.Used();
	auto replacement = ShapeLine();
	replacement.SourceOperation->Shape->Points.resize(8, {1, 1});
	context.ByteBudget = replacement.SourceOperation->Shape->Points.size() * 2 * sizeof(double) - 1;
	CHECK_FALSE(runtime.Init(context, replacement));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(budget.Used() == oldBytes);
	CHECK(runtime.PointRatio(.5).X == 5);
}
TEST_CASE(
	"Shape operations remain planar and compose through existing path wrappers", "[imagegraph][source_path]"
) {
	PathData3D invalid;
	invalid.SourceOperation.emplace().Kind = SourcePathOperationKind::Shape;
	CHECK_FALSE(detail::ValidSourcePath3D(invalid));
	PathData3D wrapped;
	wrapped.Source2D = ShapeLine();
	CHECK(detail::ValidSourcePath3D(wrapped));
	Path2D reversed;
	auto &operation = reversed.SourceOperation.emplace();
	operation.Kind = SourcePathOperationKind::Reverse;
	operation.Inputs = {ShapeLine()};
	const auto sample = imagegraph_test::RunNode("pc.path_sample", {}, {{"path", reversed}, {"ratio", .2}});
	INFO(sample.Message);
	REQUIRE(sample.Ok);
	CHECK(std::get<Vector2>(*sample.OutputValue("position")) == Vector2{8, 0});
}
TEST_CASE(
	"Shape analytic ratio snaps lengthdir components before translating the source position",
	"[imagegraph][source_path]"
) {
	SourcePathShapeData2D shape;
	shape.Kind = SourcePathShapeKind2D::Ellipse;
	shape.Position = {100, 200};
	shape.HalfSize = {1, 1};
	const auto baseline = detail::SourceShapeRatio(shape, {}, 0, .5 / 360.);
	REQUIRE(baseline);
	CHECK(baseline->X == 101);
	shape.Position = {100.123, 200.123};
	for (const auto kind :
		 {SourcePathShapeKind2D::Ellipse, SourcePathShapeKind2D::Arc, SourcePathShapeKind2D::Squircle}) {
		shape.Kind = kind;
		shape.AngleRange = {0, 360};
		shape.Factor = 2;
		const auto horizontal = detail::SourceShapeRatio(shape, {}, 0, .5 / 360.);
		REQUIRE(horizontal);
		CHECK(horizontal->X == shape.Position.X + 1);
		const auto vertical = detail::SourceShapeRatio(shape, {}, 0, 90.5 / 360.);
		REQUIRE(vertical);
		CHECK(vertical->Y == shape.Position.Y - 1);
	}
}
