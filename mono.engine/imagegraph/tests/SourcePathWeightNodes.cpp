#include "SourcePathPayload.hpp"
#include "nodes/Path.hpp"
#include "nodes/Path3D.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_path_weight")
using namespace engine::imagegraph;
namespace {
	Path2D WeightLine() {
		Path2D path;
		path.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}};
		path.Weights = {{0, 2}, {100, 4}};
		return path;
	}
	Document WeightGraph() {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"weight", "pc.path_weight_adjust", "", {}, {{"path", WeightLine()}}},
			{"sample", "pc.path_sample", "", {}, {{"ratio", .5}}}
		};
		d.Links = {{"weight", "path", "sample", "path"}};
		d.Outputs = {
			{"weight", "sample", "weight"}, {"position", "sample", "position"}, {"path", "weight", "path"}
		};
		return d;
	}
	Value Sample(const Document &d, std::string_view port) {
		Diagnostic error;
		Plan plan;
		REQUIRE(Compile(d, plan, error) == Status::Ok);
		EvaluatedValue out;
		INFO(error.Message);
		REQUIRE(EvaluateValue(d, plan, std::string(port), {}, out, error) == Status::Ok);
		return std::move(out.Data);
	}
} // namespace
TEST_CASE(
	"Weight Adjust constant modes preserve signed multiplication and override", "[source_path_weight]"
) {
	for (const int64_t mode : {0, 1, 2}) {
		auto d = WeightGraph();
		d.Nodes[0].Values.push_back({"apply_mode", EnumValue{mode}});
		d.Nodes[0].Values.push_back({"value", -4.});
		CHECK(std::get<double>(Sample(d, "weight")) == (mode == 0 ? 0. : mode == 1 ? -12. : -4.));
		CHECK(std::get<Vector2>(Sample(d, "position")) == Vector2{5, 0});
	}
}
TEST_CASE("Weight Adjust curve uses source TOTAL_FRAMES lookup resolution", "[source_path_weight]") {
	Curve curve;
	curve.Header = {0, 1, 0, 0, 1, 0};
	curve.Anchors = {{0, 0, 0, 0, 1. / 3, 0}, {-1. / 3, -1, 1, 1, 0, 0}};
	for (const uint64_t frames : {uint64_t{1}, uint64_t{4}}) {
		auto d = WeightGraph();
		d.Timeline = TimelineSettings{frames, 0, frames - 1};
		d.Nodes[0].Values.push_back({"curve", curve});
		d.Nodes[0].Values.push_back({"adjust_type", EnumValue{1}});
		d.Nodes[0].Values.push_back({"apply_mode", EnumValue{2}});
		d.Nodes[1].Values = {{"ratio", .25}};
		CHECK(std::get<double>(Sample(d, "weight")) == Catch::Approx(frames == 1 ? .25 : .015625));
		const auto path = std::get<Path2D>(Sample(d, "path"));
		REQUIRE(path.SourceOperation);
		CHECK(path.SourceOperation->WeightCurve.size() == frames + 1);
	}
}
TEST_CASE(
	"Weight Adjust direction applies the final probe weight instead of "
	"the original weight",
	"[source_path_weight]"
) {
	auto d = WeightGraph();
	d.Nodes[0].Values.push_back({"adjust_type", EnumValue{2}});
	d.Nodes[0].Values.push_back({"value_range", Vector2{1, 1}});
	CHECK(std::get<double>(Sample(d, "weight")) == Catch::Approx(4.0029996));
	for (const double direction : {0., 90., 180., 450.}) {
		auto graph = WeightGraph();
		graph.Nodes[0].Values.push_back({"adjust_type", EnumValue{2}});
		graph.Nodes[0].Values.push_back({"apply_mode", EnumValue{2}});
		graph.Nodes[0].Values.push_back({"direction_shift", direction});
		CHECK(
			std::get<double>(Sample(graph, "weight")) == Catch::Approx(
															 direction == 0		? 0
															 : direction == 180 ? 1
																				: .5
														 )
		);
	}
}
TEST_CASE(
	"Weight Adjust path payload persists its exact source controls and "
	"sampled curve",
	"[source_path_weight]"
) {
	auto d = WeightGraph();
	d.Nodes[0].Values.push_back({"value", 7.});
	const auto blob = std::get<Path2D>(Sample(d, "path"));
	d.Nodes[1].Values.push_back({"path", blob});
	d.Links.clear();
	Diagnostic error;
	Document restored;
	REQUIRE(Read(Write(d), restored, error) == Status::Ok);
	CHECK(restored == d);
	CHECK(std::get<double>(Sample(restored, "weight")) == 10.);
}
TEST_CASE(
	"Weight Adjust catalogue noone input returns a zero point with "
	"default weight",
	"[source_path_weight]"
) {
	auto d = WeightGraph();
	d.Nodes[0].Values.clear();
	CHECK(std::get<Vector2>(Sample(d, "position")) == Vector2{});
	CHECK(std::get<double>(Sample(d, "weight")) == 1.);
}
TEST_CASE("Weight Adjust refused curve growth preserves the caller output", "[source_path_weight]") {
	auto d = WeightGraph();
	d.Timeline = TimelineSettings{Limits::MaximumArrayElements, 0, Limits::MaximumArrayElements - 1};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	EvaluatedValue output{"old", int64_t{17}, {}};
	CHECK(EvaluateValue(d, plan, "weight", {}, output, error) == Status::LimitExceeded);
	CHECK(std::get<int64_t>(output.Data) == 17);
	d = WeightGraph();
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	StatefulEvaluationResult state;
	state.Output = output;
	CHECK(EvaluateStateful(d, plan, "path", {}, state, error, 1) == Status::LimitExceeded);
	CHECK(std::get<int64_t>(std::get<EvaluatedValue>(state.Output).Data) == 17);
}
TEST_CASE(
	"Weight Adjust invalid inactive controls and native payloads fail "
	"without publishing",
	"[source_path_weight]"
) {
	auto d = WeightGraph();
	auto blob = std::get<Path2D>(Sample(d, "path"));
	blob.SourceOperation->WeightCurve[0] = std::numeric_limits<double>::infinity();
	CHECK_FALSE(detail::ValidSourcePath2D(blob));
	blob.SourceOperation->WeightCurve[0] = 1;
	blob.SourceOperation->WeightType = 3;
	CHECK_FALSE(detail::ValidSourcePath2D(blob));
	blob.SourceOperation->WeightType = 0;
	blob.SourceOperation->Kind = SourcePathOperationKind::Reverse;
	CHECK_FALSE(detail::ValidSourcePath2D(blob));
}

TEST_CASE("Weight Adjust path transport survives native array selection", "[source_path_weight]") {
	auto d = WeightGraph();
	Node array{"array", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	array.DynamicInputs = {{"input_0", ValueType::Path2D, std::nullopt}};
	d.Nodes.push_back(std::move(array));
	d.Nodes.push_back({"pick", "pc.array_get", "", {}, {{"index", int64_t{0}}}});
	d.Nodes[0].Values.push_back({"value", 5.});
	d.Links = {
		{"weight", "path", "array", "input_0"},
		{"array", "array", "pick", "array"},
		{"pick", "value", "sample", "path"}
	};
	CHECK(std::get<double>(Sample(d, "weight")) == 8.);
	Diagnostic error;
	Document restored;
	REQUIRE(Read(Write(d), restored, error) == Status::Ok);
	CHECK(std::get<double>(Sample(restored, "weight")) == 8.);
}
TEST_CASE(
	"Weight Adjust accepts a compiled spatial shape and projects XY "
	"while preserving source bounds",
	"[source_path_weight]"
) {
	auto d = WeightGraph();
	d.Nodes[0].Values = {{"apply_mode", EnumValue{2}}, {"value", -3.}};
	d.Nodes[1].Values = {{"ratio", .125}};
	d.Nodes.insert(
		d.Nodes.begin(),
		{"shape", "pc.path_shape_3_d", "", {}, {{"shape", EnumValue{2}}, {"sides", int64_t{3}}}}
	);
	d.Links.push_back({"shape", "path_data", "weight", "path"});
	CHECK(std::get<double>(Sample(d, "weight")) == -3.);
	const auto point = std::get<Vector2>(Sample(d, "position"));
	CHECK(point.X == Catch::Approx(.21875));
	CHECK(point.Y == Catch::Approx(-std::sqrt(3.) * 3 / 32));
	const auto out = std::get<Path2D>(Sample(d, "path"));
	REQUIRE(out.SourceOperation->WeightInput3D);
	CHECK(out.SourceOperation->Inputs.empty());
	CHECK(
		out.SourceOperation->WeightInput3D->SourceBounds2D ==
		std::optional<Vector4>{Vector4{-.5, -.5, .5, .5}}
	);
	Diagnostic error;
	Document restored;
	REQUIRE(Read(Write(d), restored, error) == Status::Ok);
	CHECK(std::get<Vector2>(Sample(restored, "position")) == point);
	d.Junctions = {{"saved", "", ValueType::Path2D, out}};
	d.Links = {{"saved", "value", "sample", "path"}};
	REQUIRE(Read(Write(d), restored, error) == Status::Ok);
	CHECK(restored.Junctions == d.Junctions);
	CHECK(std::get<Vector2>(Sample(restored, "position")) == point);
}
TEST_CASE("Weight Adjust saved spatial child strips nested Shift memo identities", "[source_path_weight]") {
	auto d = WeightGraph();
	PathValue3D child;
	auto &data = child.Data.emplace();
	data.Source2D.emplace();
	auto &shift = data.Source2D->SourceOperation.emplace();
	shift.Kind = SourcePathOperationKind::Shift;
	shift.Inputs = {WeightLine()};
	shift.ShiftDistance = 2;
	shift.EvaluationMemoId = 404;
	d.Junctions = {{"spatial", "", ValueType::Path3D, child}};
	d.Nodes[0].Values.clear();
	d.Links.push_back({"spatial", "value", "weight", "path"});
	const auto path = std::get<Path2D>(Sample(d, "path"));
	REQUIRE(path.SourceOperation->WeightInput3D->Source2D->SourceOperation);
	CHECK(path.SourceOperation->WeightInput3D->Source2D->SourceOperation->EvaluationMemoId == 0);
	CHECK(child.Data->Source2D->SourceOperation->EvaluationMemoId == 404);
	CHECK(std::get<Vector2>(Sample(d, "position")) == Vector2{5, -2});
}
TEST_CASE(
	"Weight Adjust preserves raw spatial source length math while dropping Z from its output point",
	"[source_path_weight]"
) {
	auto d = WeightGraph();
	auto anchor = [](double x) {
		ArrayValue value{ValueType::Scalar, {}};
		for (double n : {x, 0., 3., 0., 0., 0., 0., 0., 0., 0.})
			value.Elements.emplace_back(n);
		return value;
	};
	Node spatial{"spatial", "pc.path_3_d", "", {}, {}};
	spatial.DynamicInputs = {
		{"anchor_0", ValueType::Array, anchor(0)}, {"anchor_1", ValueType::Array, anchor(10)}
	};
	d.Nodes.insert(d.Nodes.begin(), std::move(spatial));
	d.Nodes[1].Values = {{"value", 2.}};
	d.Links.push_back({"spatial", "path_data", "weight", "path"});
	CHECK(std::get<Vector2>(Sample(d, "position")) == Vector2{5, 0});
	CHECK(std::get<double>(Sample(d, "weight")) == 3.);
	const auto path = std::get<Path2D>(Sample(d, "path"));
	REQUIRE(path.SourceOperation->WeightInput3D);
	detail::PathRuntime3D child(*path.SourceOperation->WeightInput3D);
	CHECK(child.Length() == Catch::Approx(32 * std::sqrt(9. + .3125 * .3125)));
	CHECK(child.SourceBoundary() == std::optional<Vector4>{Vector4{0, 0, 10, 0}});
}
