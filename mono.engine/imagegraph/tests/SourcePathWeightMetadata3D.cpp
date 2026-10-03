#include "SourcePathPayload3D.hpp"
#include "nodes/Path3D.hpp"

#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_path_weight_metadata_3d")
using namespace engine::imagegraph;
TEST_CASE(
	"Spatial shape metadata retains exact bbox and accumulated cache "
	"through empty source updates",
	"[source_path_weight]"
) {
	Document d;
	d.FormatVersion = 9;
	d.Nodes = {
		{"shape",
		 "pc.path_shape_3_d",
		 "",
		 {},
		 {{"shape", EnumValue{6}}, {"resolution", int64_t{4}}, {"revolution", 1.}}}
	};
	d.Outputs = {{"path", "shape", "path_data"}};
	Diagnostic error;
	Plan plan;
	StatefulEvaluationResult state;
	EvaluationRequest request;
	request.DataReplay = &state.Data;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStateful(d, plan, "path", request, state, error) == Status::Ok);
	const auto populated = std::get<PathValue3D>(std::get<EvaluatedValue>(state.Output).Data);
	REQUIRE(populated.Data->SourceBounds2D);
	const auto bounds = populated.Data->SourceBounds2D;
	detail::PathRuntime3D runtime(*populated.Data);
	std::vector<double> expected;
	for (size_t i = 0; i < runtime.SourceAccumulatedCount(); ++i)
		expected.push_back(runtime.SourceAccumulatedAt(i));
	d.Nodes[0].Values.back().Data = 0.;
	request.Tick = 1;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStateful(d, plan, "path", request, state, error) == Status::Ok);
	const auto empty = std::get<PathValue3D>(std::get<EvaluatedValue>(state.Output).Data);
	REQUIRE(empty.Data->Anchors.empty());
	REQUIRE(empty.Data->SourceEmptyCache);
	CHECK(empty.Data->SourceEmptyCache->Accumulated == expected);
	CHECK(empty.Data->SourceBounds2D == bounds);
	CHECK(populated.Data->Anchors.size() == 4);
	request.Tick = 2;
	REQUIRE(EvaluateStateful(d, plan, "path", request, state, error) == Status::Ok);
	CHECK(std::get<PathValue3D>(std::get<EvaluatedValue>(state.Output).Data) == empty);
	d.Junctions = {{"saved", "", ValueType::Path3D, empty}};
	Document restored;
	REQUIRE(Read(Write(d), restored, error) == Status::Ok);
	CHECK(restored.Junctions == d.Junctions);
	auto bad = empty;
	bad.Data->SourceEmptyCache->Accumulated.back() += 1;
	CHECK_FALSE(detail::ValidSourcePath3D(*bad.Data));
	bad = empty;
	bad.Data->SourceEmptyCache->Accumulated.pop_back();
	CHECK_FALSE(detail::ValidSourcePath3D(*bad.Data));
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	const auto sentinel = state;
	CHECK(EvaluateStateful(d, plan, "path", request, state, error, 1) == Status::LimitExceeded);
	CHECK(state.Data == sentinel.Data);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == std::get<EvaluatedValue>(sentinel.Output).Data);
}
TEST_CASE("Fresh empty spatial source keeps source BoundingBox noone defaults", "[source_path_weight]") {
	Document d;
	d.FormatVersion = 9;
	d.Nodes = {
		{"shape",
		 "pc.path_shape_3_d",
		 "",
		 {},
		 {{"shape", EnumValue{6}}, {"resolution", int64_t{4}}, {"revolution", 0.}}}
	};
	d.Outputs = {{"path", "shape", "path_data"}};
	Plan plan;
	Diagnostic error;
	EvaluatedValue out;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateValue(d, plan, "path", {}, out, error) == Status::Ok);
	const auto path = std::get<PathValue3D>(out.Data);
	CHECK(path.Data->SourceBounds2D == std::optional<Vector4>{Vector4{-4, -4, -4, -4}});
	CHECK_FALSE(path.Data->SourceEmptyCache);
}

TEST_CASE(
	"Spatial path clone admission counts retained tables and owned operation children", "[source_path_weight]"
) {
	Value leaf{PathValue3D{}};
	auto &data = std::get<PathValue3D>(leaf).Data.emplace();
	data.SourcePolyline = true;
	data.Resolution = 1;
	data.SourceEmptyCache = SourcePolylineEmptyCache3D{3, 4, {1, 2, 3, 3}};
	data.SourceEmptyCache->Accumulated.reserve(12);
	REQUIRE(detail::ValidSourcePath3D(data));
	const auto leafBytes =
		sizeof(PathData3D) + data.SourceEmptyCache->Accumulated.capacity() * sizeof(double);
	CHECK(ValueClonePayloadBytes(leaf) == sizeof(Value) + leafBytes);
	Value nested{PathValue3D{}};
	auto &outer = std::get<PathValue3D>(nested).Data.emplace();
	auto &op = outer.SourceOperation.emplace();
	op.Kind = SourcePathOperationKind::Reverse;
	op.Inputs.reserve(3);
	op.Inputs.push_back(std::get<PathValue3D>(leaf));
	auto &child = *op.Inputs.front().Data;
	child.SourceEmptyCache->Accumulated.reserve(16);
	REQUIRE(detail::ValidSourcePath3D(outer));
	const auto childBytes =
		sizeof(PathData3D) + child.SourceEmptyCache->Accumulated.capacity() * sizeof(double);
	const auto nestedBytes = sizeof(PathData3D) + sizeof(SourcePathData3D) +
							 op.Inputs.capacity() * sizeof(PathValue3D) + childBytes;
	CHECK(ValueClonePayloadBytes(nested) == sizeof(Value) + nestedBytes);
	CHECK(detail::RetainedPayloadBytes(std::get<PathValue3D>(nested)) == nestedBytes);
}
