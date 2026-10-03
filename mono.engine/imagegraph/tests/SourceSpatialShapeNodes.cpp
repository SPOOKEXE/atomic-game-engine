#include "nodes/Path3D.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/StatefulTemporalCone.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_spatial_shape")
using namespace engine::imagegraph;
namespace {
	Document ShapePathGraph(int64_t shape = 0) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {{"shape", "pc.path_shape_3_d", "", {}, {{"shape", EnumValue{shape}}}}};
		d.Outputs = {{"path", "shape", "path_data"}};
		return d;
	}
	PathValue3D ShapePathValue(const Document &d, EvaluationRequest r = {}) {
		Plan p;
		Diagnostic diag;
		auto status = Compile(d, p, diag);
		INFO(diag.Message);
		INFO(diag.Port);
		REQUIRE(status == Status::Ok);
		EvaluatedValue v;
		status = EvaluateValue(d, p, "path", r, v, diag);
		INFO(diag.Message);
		INFO(diag.Port);
		REQUIRE(status == Status::Ok);
		return std::get<PathValue3D>(v.Data);
	}
	Vector3 ShapeAnchor(const PathValue3D &p, size_t i) {
		const auto &a = p.Data->Anchors.at(i).Controls;
		return {a[0], a[1], a[2]};
	}
	void ShapeClose(Vector3 a, Vector3 b) {
		CHECK(a.X == Catch::Approx(b.X).margin(1e-12));
		CHECK(a.Y == Catch::Approx(b.Y).margin(1e-12));
		CHECK(a.Z == Catch::Approx(b.Z).margin(1e-12));
	}
}
TEST_CASE(
	"Shape Path 3D rectangle carries literal polyline and source closed endpoint",
	"[imagegraph][source_spatial_shape]"
) {
	const auto p = ShapePathValue(ShapePathGraph());
	REQUIRE(p.Data);
	CHECK(p.Data->SourcePolyline);
	CHECK(p.Data->Loop);
	REQUIRE(p.Data->Anchors.size() == 5);
	const std::array<Vector3, 5> expected{
		{{-.5, -.5, 0}, {.5, -.5, 0}, {.5, .5, 0}, {-.5, .5, 0}, {-.5, -.5, 0}}
	};
	for (size_t i = 0; i < expected.size(); ++i)
		ShapeClose(ShapeAnchor(p, i), expected[i]);
	detail::PathRuntime3D runtime(*p.Data);
	REQUIRE(runtime.Valid());
	CHECK(runtime.Length() == 4);
	ShapeClose(runtime.Ratio(1).Position, expected[0]);
	ShapeClose(runtime.Ratio(.125).Position, {0, -.5, 0});
	ShapeClose(runtime.Ratio(-.125).Position, {-1, -.5, 0});
	CHECK_FALSE(std::isfinite(runtime.BySegment(0).Position.X));
	CHECK(ShapePathValue(ShapePathGraph(-99)) == p);
	CHECK(ShapePathValue(ShapePathGraph(99)) == ShapePathValue(ShapePathGraph(8)));
}
TEST_CASE(
	"Shape Path 3D ellipse polygon and star preserve negativeY and inner alternation",
	"[imagegraph][source_spatial_shape]"
) {
	auto ellipse = ShapePathGraph(1);
	ellipse.Nodes[0].Values.push_back({"resolution", int64_t{4}});
	const auto e = ShapePathValue(ellipse);
	REQUIRE(e.Data->Anchors.size() == 5);
	ShapeClose(ShapeAnchor(e, 0), {.5, 0, 0});
	ShapeClose(ShapeAnchor(e, 1), {0, -.5, 0});
	ShapeClose(ShapeAnchor(e, 2), {-.5, 0, 0});
	auto polygon = ShapePathGraph(2);
	polygon.Nodes[0].Values.push_back({"sides", int64_t{1}});
	const auto p = ShapePathValue(polygon);
	REQUIRE(p.Data->Anchors.size() == 4);
	ShapeClose(ShapeAnchor(p, 1), {-.25, -std::sqrt(3.) * .25, 0});
	auto star = ShapePathGraph(4);
	star.Nodes[0].Values.push_back({"sides", int64_t{3}});
	const auto s = ShapePathValue(star);
	REQUIRE(s.Data->Anchors.size() == 7);
	ShapeClose(ShapeAnchor(s, 1), {.125, -std::sqrt(3.) * .125, 0});
}
TEST_CASE(
	"Shape Path 3D spring sphere and spiral use raw revolution and pitch math",
	"[imagegraph][source_spatial_shape]"
) {
	for (int64_t kind : {6, 7, 8}) {
		auto d = ShapePathGraph(kind);
		d.Nodes[0].Values.push_back({"resolution", int64_t{4}});
		d.Nodes[0].Values.push_back({"revolution", 1.0});
		const auto p = ShapePathValue(d);
		CHECK_FALSE(p.Data->Loop);
		REQUIRE(p.Data->Anchors.size() == 4);
		if (kind == 6) ShapeClose(ShapeAnchor(p, 1), {0, -.5, .05});
		if (kind == 7) {
			ShapeClose(ShapeAnchor(p, 0), {0, 0, -.5});
			ShapeClose(ShapeAnchor(p, 1), {0, -std::sqrt(3.) * .25, -.25});
		}
		if (kind == 8) ShapeClose(ShapeAnchor(p, 1), {0, -.025, 0});
	}
	auto fractional = ShapePathGraph(6);
	fractional.Nodes[0].Values.push_back({"resolution", int64_t{5}});
	fractional.Nodes[0].Values.push_back({"revolution", .5});
	CHECK(ShapePathValue(fractional).Data->Anchors.size() == 3);
	auto reversed = ShapePathGraph(8);
	reversed.Nodes[0].Values.push_back({"resolution", int64_t{4}});
	reversed.Nodes[0].Values.push_back({"revolution", 1.0});
	reversed.Nodes[0].Values.push_back({"reverse", true});
	const auto p = ShapePathValue(reversed);
	ShapeClose(ShapeAnchor(p, 3), {0, .375, 0});
}
TEST_CASE(
	"Shape Path 3D rotates about source position then permutes absolute axes",
	"[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph();
	d.Nodes[0].Values.push_back({"position", Vector3{2, 3, 7}});
	d.Nodes[0].Values.push_back({"half_size", Vector3{1, 2, 8}});
	d.Nodes[0].Values.push_back({"rotation", 90.0});
	d.Nodes[0].Values.push_back({"up_axis", EnumValue{0}});
	ShapeClose(ShapeAnchor(ShapePathValue(d), 0), {7, 2, 4});
	d.Nodes[0].Values.back().Data = EnumValue{1};
	ShapeClose(ShapeAnchor(ShapePathValue(d), 0), {4, 7, 2});
}
TEST_CASE(
	"Shape Path 3D trueEuclidean spring sampler wraps open endpoints", "[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph(6);
	d.Nodes[0].Values.push_back({"resolution", int64_t{4}});
	d.Nodes[0].Values.push_back({"revolution", 1.0});
	const auto p = ShapePathValue(d);
	detail::PathRuntime3D runtime(*p.Data);
	REQUIRE(runtime.Valid());
	const double chord = std::sqrt(.5025);
	CHECK(runtime.Length() == Catch::Approx(chord * 3));
	ShapeClose(runtime.Ratio(.5).Position, {-.25, -.25, .075});
	ShapeClose(runtime.Ratio(1).Position, {.5, 0, 0});
	auto ordinary = *p.Data;
	ordinary.SourcePolyline = false;
	ordinary.Resolution = 4;
	detail::PathRuntime3D legacy(ordinary);
	CHECK(legacy.Length() != Catch::Approx(runtime.Length()));
}
TEST_CASE(
	"Shape Path 3D native sourceclass codec roundtrips with safe legacy default",
	"[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph();
	const auto value = ShapePathValue(d);
	d.Junctions = {{"saved", "", ValueType::Path3D, value}};
	const auto text = Write(d);
	CHECK(text.find("p3s 1 source_polyline 1") != std::string::npos);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(text, restored, diag) == Status::Ok);
	CHECK(restored == d);
	CHECK(std::get<PathValue3D>(*restored.Junctions[0].Default).Data->SourcePolyline);
	auto legacy = value;
	legacy.Data->SourcePolyline = false;
	d.Junctions[0].Default = legacy;
	const auto old = Write(d);
	CHECK(old.find("p3s") == std::string::npos);
	REQUIRE(Read(old, restored, diag) == Status::Ok);
	CHECK_FALSE(std::get<PathValue3D>(*restored.Junctions[0].Default).Data->SourcePolyline);
	for (const std::string replacement : {"source_polyline 0", "source_unknown 1"}) {
		auto malformed = text;
		const auto offset = malformed.find("source_polyline 1");
		REQUIRE(offset != std::string::npos);
		malformed.replace(offset, 17, replacement);
		const auto sentinel = restored;
		CHECK(Read(malformed, restored, diag) != Status::Ok);
		CHECK(restored == sentinel);
	}
}
TEST_CASE(
	"Shape Path 3D animation replay retains branch controls and marker", "[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph();
	d.Keyframes = {{"shape", "rotation", 0, 0.0, "linear"}, {"shape", "rotation", 2, 180.0}};
	const auto middle = ShapePathValue(d, EvaluationRequest{.Tick = 1});
	ShapeClose(ShapeAnchor(middle, 0), {.5, -.5, 0});
	const auto initial = ShapePathValue(d);
	CHECK(ShapePathValue(d, EvaluationRequest{.Tick = 0}) == initial);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(ShapePathValue(restored, EvaluationRequest{.Tick = 1}) == middle);
}
TEST_CASE(
	"Shape Path 3D separators zero divisors and huge counts refuse atomically",
	"[imagegraph][source_spatial_shape]"
) {
	for (const auto &[kind, port, value] : std::array<std::tuple<int64_t, std::string_view, Value>, 3>{
			 {{1, "resolution", int64_t{0}}, {6, "revolution", 1e20}, {3, "shape", EnumValue{3}}}
		 }) {
		auto d = ShapePathGraph(kind);
		if (port == "shape")
			d.Nodes[0].Values[0].Data = value;
		else
			d.Nodes[0].Values.push_back({std::string(port), value});
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		EvaluatedValue output;
		output.Data = Vector3{9, 8, 7};
		const auto code = EvaluateValue(d, p, "path", {}, output, diag);
		INFO(diag.Message);
		CHECK(code == (kind == 6 ? Status::LimitExceeded : Status::UnsupportedExecution));
		CHECK(output.Data == Value{Vector3{9, 8, 7}});
	}
}

TEST_CASE(
	"Shape Path 3D compiled transform sample and instancer consumers preserve source sampler",
	"[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph(6);
	d.Nodes[0].Values.push_back({"resolution", int64_t{4}});
	d.Nodes[0].Values.push_back({"revolution", 1.0});
	d.Nodes.push_back({"transform", "pc.path_3_d_transform", "", {}, {{"position", Vector3{1, 2, 3}}}});
	d.Nodes.push_back({"sample", "pc.path_sample", "", {}, {{"ratio", .5}}});
	d.Links = {{"shape", "path_data", "transform", "path"}, {"transform", "path", "sample", "path"}};
	d.Outputs = {{"point", "sample", "position"}};
	Plan plan;
	Diagnostic diagnostic;
	auto compiled = Compile(d, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluatedValue point;
	const auto evaluated = EvaluateValue(d, plan, "point", {}, point, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	ShapeClose(std::get<Vector3>(point.Data), {.75, 1.75, 3.075});
	d.Nodes.resize(1);
	d.Nodes.push_back({"cube", "pc.3_d_mesh_cube", "", {}, {}});
	d.Nodes.push_back(
		{"instances",
		 "pc.3_d_instancer",
		 "",
		 {},
		 {{"amounts", int64_t{3}}, {"seed", 0.0}, {"follow_path", false}, {"shift_position", Vector3{}}}}
	);
	d.Links = {{"cube", "mesh", "instances", "mesh"}, {"shape", "path_data", "instances", "shift_path"}};
	d.Outputs = {{"mesh", "instances", "mesh"}};
	compiled = Compile(d, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	const auto rendered = EvaluateValue(d, plan, "mesh", {}, point, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(rendered == Status::Ok);
	const auto mesh = std::get<MeshValue3D>(point.Data);
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Instances.size() == 3);
	CHECK(mesh.Data->Instances[0].Fields[0] == Catch::Approx(.5));
	CHECK(mesh.Data->Instances[1].Fields[0] == Catch::Approx(-.25));
	CHECK(mesh.Data->Instances[1].Fields[1] == Catch::Approx(-.25));
	CHECK(mesh.Data->Instances[1].Fields[2] == Catch::Approx(.075));
	CHECK(mesh.Data->Instances[2].Fields[0] == Catch::Approx(.5));
}
TEST_CASE(
	"Shape Path 3D plainNode receives whole arrays without invented processor batches",
	"[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph(1);
	d.Junctions.push_back(
		{"resolutions", "", ValueType::Array, ArrayValue{ValueType::Integer, {int64_t{4}, int64_t{8}}}}
	);
	d.Links.push_back({"resolutions", "value", "shape", "resolution"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	output.Data = 31.0;
	CHECK(EvaluateValue(d, plan, "path", {}, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "resolution");
	CHECK(output.Data == Value{31.0});
	auto unused = ShapePathGraph();
	unused.Junctions.push_back(
		{"resolutions", "", ValueType::Array, ArrayValue{ValueType::Integer, {int64_t{4}, int64_t{8}}}}
	);
	unused.Links.push_back({"resolutions", "value", "shape", "resolution"});
	CHECK(ShapePathValue(unused).Data->Anchors.size() == 5);
	auto vector = ShapePathGraph();
	vector.Junctions.push_back(
		{"coordinates", "", ValueType::Array, ArrayValue{ValueType::Scalar, {2., 3., 4.}}}
	);
	vector.Links.push_back({"coordinates", "value", "shape", "position"});
	ShapeClose(ShapeAnchor(ShapePathValue(vector), 0), {1.5, 2.5, 4});
}
TEST_CASE(
	"Shape Path 3D surface coordinate getter and sourcepolyline validation are bounded",
	"[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph();
	d.Nodes.push_back(
		{"size",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{3}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	d.Links = {{"size", "image", "shape", "position"}};
	ShapeClose(ShapeAnchor(ShapePathValue(d), 0), {1.5, 2.5, 0});
	auto invalid = ShapePathValue(ShapePathGraph());
	invalid.Data->Anchors[0].Controls[3] = 1.;
	CHECK_FALSE(detail::ValidRuntimeValue(Value{invalid}));
	invalid = ShapePathValue(ShapePathGraph());
	invalid.Data->Anchors.back().Controls[0] += 1;
	CHECK_FALSE(detail::ValidRuntimeValue(Value{invalid}));
	const auto *entry = FindCatalogueEntry("pc.path_shape_3_d");
	REQUIRE(entry);
	const auto *processing = FindCatalogueInput(*entry, "attribute_process");
	CHECK_FALSE(processing);
}

TEST_CASE(
	"Shape Path 3D empty updates preserve cached length without retaining vertices",
	"[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph(6);
	d.Nodes[0].Values.push_back({"resolution", int64_t{4}});
	d.Nodes[0].Values.push_back({"revolution", 0.});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	const std::array<std::string, 1> selected{"path"};
	CHECK(AnalyzeStatefulTemporalCone(d, plan, selected).DataProcessors == 1);
	StatefulEvaluationResult state;
	EvaluationRequest request;
	request.DataReplay = &state.Data;
	auto evaluate = [&] {
		const auto code = EvaluateStateful(d, plan, "path", request, state, diagnostic);
		INFO(diagnostic.Message);
		INFO(diagnostic.Port);
		REQUIRE(code == Status::Ok);
		return std::get<PathValue3D>(std::get<EvaluatedValue>(state.Output).Data);
	};
	const auto fresh = evaluate();
	REQUIRE(fresh.Data->Anchors.empty());
	CHECK_FALSE(fresh.Data->Loop);
	CHECK_FALSE(fresh.Data->SourceEmptyCache);
	ShapeClose(detail::PathRuntime3D(*fresh.Data).Ratio(.5).Position, {});
	CHECK(detail::PathRuntime3D(*fresh.Data).Length() == 0);
	d.Nodes[0].Values.back().Data = 1.;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	request.Tick = 1;
	const auto populated = evaluate();
	REQUIRE(populated.Data->Anchors.size() == 4);
	const double priorLength = detail::PathRuntime3D(*populated.Data).Length();
	CHECK(priorLength == Catch::Approx(3 * std::sqrt(.5025)));
	d.Nodes[0].Values.back().Data = 0.;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	request.Tick = 2;
	const auto empty = evaluate();
	REQUIRE(empty.Data->Anchors.empty());
	REQUIRE(empty.Data->SourceEmptyCache);
	CHECK(empty.Data->SourceEmptyCache->Length == priorLength);
	CHECK(empty.Data->SourceEmptyCache->SegmentCount == 4);
	CHECK(detail::PathRuntime3D(*empty.Data).Length() == priorLength);
	CHECK(
		ValueClonePayloadBytes(Value{empty}) ==
		sizeof(Value) + sizeof(PathData3D) + empty.Data->SourceEmptyCache->Accumulated.size() * sizeof(double)
	);
	CHECK_FALSE(std::isfinite(detail::PathRuntime3D(*empty.Data).Ratio(.5).Position.X));
	request.Tick = 3;
	CHECK(evaluate() == empty);
	CHECK(populated.Data->Anchors.size() == 4);
	REQUIRE(state.Data.Entries.size() == 1);
	REQUIRE(state.Data.Entries[0].Values.size() == 1);
	CHECK(std::get<PathValue3D>(state.Data.Entries[0].Values[0].Data) == empty);
	auto wrongOwner = state.Data;
	wrongOwner.Entries[0].Values[0].Data = Vector3{1, 2, 3};
	const auto priorOutput = std::get<EvaluatedValue>(state.Output);
	request.DataReplay = &wrongOwner;
	CHECK(EvaluateStateful(d, plan, "path", request, state, diagnostic) == Status::InvalidValue);
	CHECK(std::get<EvaluatedValue>(state.Output) == priorOutput);
	CHECK(std::get<Vector3>(wrongOwner.Entries[0].Values[0].Data) == Vector3{1, 2, 3});
	request.DataReplay = &state.Data;
	const auto oldState = state.Data;
	const auto oldOutput = std::get<EvaluatedValue>(state.Output);
	CHECK(EvaluateStateful(d, plan, "path", request, state, diagnostic, 1) == Status::LimitExceeded);
	CHECK(state.Data == oldState);
	CHECK(std::get<EvaluatedValue>(state.Output) == oldOutput);
	// A caller resets a prefix by clearing its replay owner, rather than reusing stale geometry.
	state = {};
	request.Tick = 0;
	CHECK(evaluate() == fresh);
}
TEST_CASE(
	"Shape Path 3D stale empty state has durable named fields and refuses undefined consumers",
	"[imagegraph][source_spatial_shape]"
) {
	PathValue3D empty;
	auto &data = empty.Data.emplace();
	data.SourcePolyline = true;
	data.Loop = true;
	data.Resolution = 1;
	data.SourceEmptyCache = SourcePolylineEmptyCache3D{4, 5};
	Document d;
	d.FormatVersion = 9;
	d.Junctions = {{"saved", "", ValueType::Path3D, empty}};
	const auto text = Write(d);
	CHECK(text.find("source_empty_cache 1 4 5") != std::string::npos);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == d);
	for (const std::string replacement :
		 {"source_empty_cache 1 -4 5",
		  "source_empty_cache 1 4 0",
		  "source_empty_cache 1 4 1025",
		  "source_unknown_cache 1 4 5",
		  "source_empty_cache 2"}) {
		auto bad = text;
		const auto offset = bad.find("source_empty_cache 1 4 5");
		REQUIRE(offset != std::string::npos);
		bad.replace(offset, std::string_view{"source_empty_cache 1 4 5"}.size(), replacement);
		const auto sentinel = restored;
		CHECK(Read(bad, restored, diagnostic) != Status::Ok);
		CHECK(restored == sentinel);
	}
	d.Nodes = {{"sample", "pc.path_sample", "", {}, {{"ratio", .5}}}};
	d.Links = {{"saved", "value", "sample", "path"}};
	d.Outputs = {{"point", "sample", "position"}};
	Plan plan;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	EvaluatedValue out;
	out.Data = Vector3{9, 8, 7};
	const auto code = EvaluateValue(d, plan, "point", {}, out, diagnostic);
	INFO(diagnostic.Message);
	CHECK(code == Status::InvalidValue);
	CHECK(out.Data == Value{Vector3{9, 8, 7}});
	auto invalid = empty;
	invalid.Data->SourcePolyline = false;
	CHECK_FALSE(detail::ValidRuntimeValue(Value{invalid}));
	invalid = empty;
	invalid.Data->Anchors.resize(2);
	CHECK_FALSE(detail::ValidRuntimeValue(Value{invalid}));
}
TEST_CASE(
	"Shape Path 3D zero length retained entries stay distinct from fresh empty",
	"[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph();
	d.Nodes[0].Values.push_back({"half_size", Vector3{}});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult state;
	EvaluationRequest request;
	request.DataReplay = &state.Data;
	REQUIRE(EvaluateStateful(d, plan, "path", request, state, diagnostic) == Status::Ok);
	const auto populated = std::get<PathValue3D>(std::get<EvaluatedValue>(state.Output).Data);
	CHECK(detail::PathRuntime3D(*populated.Data).Length() == 0);
	CHECK_FALSE(std::isfinite(detail::PathRuntime3D(*populated.Data).Ratio(0).Position.X));
	d.Nodes[0].Values[0].Data = EnumValue{6};
	d.Nodes[0].Values.push_back({"resolution", int64_t{4}});
	d.Nodes[0].Values.push_back({"revolution", 0.});
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	request.Tick = 1;
	REQUIRE(EvaluateStateful(d, plan, "path", request, state, diagnostic) == Status::Ok);
	const auto empty = std::get<PathValue3D>(std::get<EvaluatedValue>(state.Output).Data);
	REQUIRE(empty.Data->SourceEmptyCache);
	CHECK(empty.Data->SourceEmptyCache->Length == 0);
	CHECK(empty.Data->SourceEmptyCache->SegmentCount == 5);
	CHECK_FALSE(std::isfinite(detail::PathRuntime3D(*empty.Data).Ratio(0).Position.X));
}

TEST_CASE(
	"Shape Path 3D general tuples stay whole and surface sequences use Vec3 fallback",
	"[imagegraph][source_spatial_shape]"
) {
	auto d = ShapePathGraph();
	d.Nodes.push_back(
		{"source", "pc.struct_json_parse", "", {}, {{"json_string", std::string{"[2,3,4,5]"}}}}
	);
	d.Nodes.push_back({"array", "pc.array", "", {}, {{"type", EnumValue{0}}, {"spread_array", true}}});
	d.Nodes.back().DynamicInputs = {{"input_0", ValueType::Any, std::nullopt}};
	d.Links = {{"source", "struct", "array", "input_0"}, {"array", "array", "shape", "position"}};
	d.Outputs.push_back({"tuple", "array", "array"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	EvaluatedValue collected;
	REQUIRE(EvaluateValue(d, plan, "tuple", {}, collected, diagnostic) == Status::Ok);
	REQUIRE(std::get<ArrayValue>(collected.Data).Elements.size() == 4);
	ShapeClose(ShapeAnchor(ShapePathValue(d), 0), {1.5, 2.5, 4});
	d.Links.back().ToPort = "half_size";
	ShapeClose(ShapeAnchor(ShapePathValue(d), 0), {-2, -3, 0});
	d.Nodes[1].Values[0].Data = std::string{R"([2,3,4,"unused"])"};
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(d, plan, "tuple", {}, collected, diagnostic) == Status::Ok);
	REQUIRE(std::get<ArrayValue>(collected.Data).Items.size() == 4);
	ShapeClose(ShapeAnchor(ShapePathValue(d), 0), {-2, -3, 0});
	d.Nodes[1].Values[0].Data = std::string{R"([2,"invalid",4])"};
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	EvaluatedValue sentinel;
	sentinel.Data = Vector3{9, 8, 7};
	const auto status = EvaluateValue(d, plan, "path", {}, sentinel, diagnostic);
	INFO(diagnostic.Message);
	CHECK(status == Status::TypeMismatch);
	CHECK(sentinel.Data == Value{Vector3{9, 8, 7}});
	CHECK(diagnostic.NodeId == "shape");
	CHECK(diagnostic.Port == "half_size");
	d = ShapePathGraph();
	ArrayValue dimensions;
	dimensions.ElementType = ValueType::Scalar;
	dimensions.Nested = {{2., 3.}, {4., 3.}};
	d.Nodes.push_back(
		{"surface",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{255, 255, 255, 255}}}}
	);
	d.Junctions.push_back({"sizes", "", ValueType::Array, dimensions});
	d.Links = {{"sizes", "value", "surface", "dimension"}, {"surface", "surface_out", "shape", "position"}};
	d.Outputs.push_back({"surfaces", "surface", "surface_out"});
	const auto surfaceCompiled = Compile(d, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(surfaceCompiled == Status::Ok);
	ImageArray images;
	const auto arrayStatus = EvaluateArray(d, plan, "surfaces", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(arrayStatus == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Width == 2);
	CHECK(images.Images[1].Width == 4);
	ShapeClose(ShapeAnchor(ShapePathValue(d), 0), {.5, .5, 0});
}
