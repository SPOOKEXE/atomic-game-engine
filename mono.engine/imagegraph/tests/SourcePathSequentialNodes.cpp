#include "SourcePathPayload.hpp"
#include "SourcePathShiftVisit.hpp"

#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

TEST_SUITE_ID("engine.imagegraph.source_path_sequential")
using namespace engine::imagegraph;
namespace {
	Path2D Line(double length = 10, double weight = 3) {
		Path2D path;
		path.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{length, 0, 0, 0, 0, 0}, 0}};
		path.Weights = {{0, weight}, {100, weight}};
		return path;
	}
	Document Graph(std::string type = "pc.path_extends") {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"modify", std::move(type), "", {}, {{"path", Line()}}},
			{"sample", "pc.path_sample", "", {}, {{"type", EnumValue{2}}, {"ratio", .5}}}
		};
		d.Links = {{"modify", "path", "sample", "path"}};
		d.Outputs = {
			{"position", "sample", "position"}, {"weight", "sample", "weight"}, {"path", "modify", "path"}
		};
		return d;
	}
	Value Evaluate(const Document &d, std::string_view output = "position", EvaluationRequest request = {}) {
		Plan plan;
		Diagnostic error;
		EvaluatedValue result;
		const auto compiled = Compile(d, plan, error);
		INFO(error.Message << " " << error.NodeId << ":" << error.Port);
		REQUIRE(compiled == Status::Ok);
		const auto evaluated = EvaluateValue(d, plan, std::string(output), request, result, error);
		INFO(error.Message << " " << error.NodeId << ":" << error.Port);
		REQUIRE(evaluated == Status::Ok);
		return result.Data;
	}
}
TEST_CASE(
	"Extends constructor metadata precedes requested side and signed length", "[source_path_sequential]"
) {
	auto d = Graph();
	d.Nodes[0].Values.push_back({"side", EnumValue{1}});
	d.Nodes[0].Values.push_back({"length", 5.});
	const auto path = std::get<Path2D>(Evaluate(d, "path"));
	const auto &s = *path.SourceOperation->Sequential;
	CHECK(s.CachedLength == 10);
	CHECK(s.CachedSegments == 2);
	CHECK(s.Accumulated == std::vector<double>{0, 10});
	CHECK(s.CachedBounds == Vector4{0, 0, 10, 0});
	d.Nodes[1].Values.back().Data = .9;
	CHECK(std::get<Vector2>(Evaluate(d)).X == Catch::Approx(13.49));
	CHECK(std::get<double>(Evaluate(d, "weight")) == 1);
	d.Nodes[0].Values.back().Data = -2.;
	d.Nodes[1].Values.back().Data = .5;
	CHECK(std::get<Vector2>(Evaluate(d)).X == Catch::Approx(4));
}
TEST_CASE(
	"Start extension preserves the exact zero constructor accumulated entry", "[source_path_sequential]"
) {
	auto d = Graph();
	d.Nodes[0].Values.push_back({"length", 5.});
	d.Nodes[1].Values.back().Data = .2;
	CHECK(std::get<Vector2>(Evaluate(d)).X == Catch::Approx(-2));
	CHECK(std::get<double>(Evaluate(d, "weight")) == 1);
	d.Nodes[1].Values.back().Data = .5;
	CHECK(std::get<Vector2>(Evaluate(d)).X == Catch::Approx(2.5));
	CHECK(std::get<double>(Evaluate(d, "weight")) == 3);
}
TEST_CASE(
	"Flatten repeats original line-zero lengths and forwards the global entry index",
	"[source_path_sequential]"
) {
	auto d = Graph("pc.path_flattern");
	Path2D combined;
	auto &operation = combined.SourceOperation.emplace();
	operation.Kind = SourcePathOperationKind::Combine;
	operation.Inputs = {Line(10, 3), Line(20, 4)};
	d.Nodes[0].Values[0].Data = combined;
	const auto path = std::get<Path2D>(Evaluate(d, "path"));
	const auto &s = *path.SourceOperation->Sequential;
	CHECK(s.FlattenLengths == std::vector<double>{10, 10});
	CHECK(s.FlattenOwners == std::vector<uint32_t>{0, 0});
	CHECK(s.Accumulated == std::vector<double>{10, 20});
	CHECK(s.CachedSegments == 2);
	CHECK(s.CachedBounds == Vector4{-4, -4, -4, -4});
	d.Nodes[1].Values.back().Data = .75;
	CHECK(std::get<Vector2>(Evaluate(d)).X == Catch::Approx(5));
	CHECK(std::get<double>(Evaluate(d, "weight")) == 4);
	d.Nodes[0].Values.push_back({"reverse", true});
	CHECK(std::get<Vector2>(Evaluate(d)).X == Catch::Approx(5));
}
TEST_CASE(
	"Smoothen symmetric dyadic pairs retain original weight and inert clamp", "[source_path_sequential]"
) {
	auto d = Graph("pc.path_smoothen");
	d.Nodes[0].Values.push_back({"span", .1});
	d.Nodes[0].Values.push_back({"step", int64_t{2}});
	d.Nodes[1].Values.back().Data = .05;
	CHECK(std::get<Vector2>(Evaluate(d)).X == Catch::Approx(11. / 12));
	CHECK(std::get<double>(Evaluate(d, "weight")) == 3);
	const auto initial = Evaluate(d);
	d.Nodes[0].Values.push_back({"clamp_curve", true});
	CHECK(Evaluate(d) == initial);
	const auto path = std::get<Path2D>(Evaluate(d, "path"));
	CHECK(path.SourceOperation->Sequential->Accumulated.empty());
}
TEST_CASE(
	"Smoothen inactive range bypasses zero steps while active undefined average refuses atomically",
	"[source_path_sequential]"
) {
	auto d = Graph("pc.path_smoothen");
	d.Nodes[0].Values.push_back({"range", Vector2{.6, .9}});
	d.Nodes[0].Values.push_back({"step", int64_t{0}});
	CHECK(std::get<Vector2>(Evaluate(d)).X == Catch::Approx(5));
	d.Nodes[1].Values.back().Data = .7;
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	EvaluatedValue result{"sentinel", Vector2{4, 7}, std::nullopt};
	CHECK(EvaluateValue(d, plan, "position", {}, result, error) == Status::InvalidValue);
	CHECK(result.Data == Value{Vector2{4, 7}});
}
TEST_CASE(
	"Sequential native codec preserves caches and point classes while clearing memo IDs",
	"[source_path_sequential]"
) {
	for (const std::string type : {"pc.path_extends", "pc.path_flattern", "pc.path_smoothen"}) {
		auto d = Graph(type);
		auto path = std::get<Path2D>(Evaluate(d, "path"));
		auto &operation = *path.SourceOperation;
		operation.EvaluationMemoId = 400;
		if (type == "pc.path_smoothen") {
			operation.Sequential->SmoothPoint.Class = SourcePathPointClass::Spatial;
			operation.Sequential->SmoothPoint.Z = 4;
		}
		d.Junctions = {{"saved", "", ValueType::Path2D, path}};
		Document restored;
		Diagnostic error;
		REQUIRE(Read(Write(d), restored, error) == Status::Ok);
		const auto &saved = std::get<Path2D>(*restored.Junctions[0].Default);
		CHECK(saved == path);
		CHECK(saved.SourceOperation->EvaluationMemoId == 0);
	}
}
TEST_CASE(
	"Invalid update retains sampled wrapper cache without mutating borrowed prior", "[source_path_sequential]"
) {
	auto d = Graph();
	d.Nodes[0].Values.push_back({"length", 5.});
	d.Nodes[0].Values.push_back({"side", EnumValue{1}});
	d.Nodes[1].Values.back().Data = .90001;
	Plan plan;
	Diagnostic error;
	StatefulEvaluationResult result;
	EvaluationRequest request;
	request.DataReplay = &result.Data;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStateful(d, plan, "position", request, result, error) == Status::Ok);
	const auto first = std::get<EvaluatedValue>(result.Output).Data;
	const auto prior = result.Data;
	const auto before = prior;
	d.Nodes[0].Values.erase(d.Nodes[0].Values.begin());
	d.Nodes[1].Values.back().Data = .90002;
	request.Tick = 1;
	request.DataReplay = &prior;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStateful(d, plan, "position", request, result, error) == Status::Ok);
	CHECK(std::get<EvaluatedValue>(result.Output).Data == first);
	CHECK(prior == before);
	const auto sentinel = result;
	CHECK(EvaluateStateful(d, plan, "position", request, result, error, 1) == Status::LimitExceeded);
	CHECK(result.Data == sentinel.Data);
	CHECK(std::get<EvaluatedValue>(result.Output).Data == std::get<EvaluatedValue>(sentinel.Output).Data);
	CHECK(prior == before);
}
TEST_CASE(
	"Sequential six and two decimal journals preserve linked first-sample order", "[source_path_sequential]"
) {
	for (const std::string type : {"pc.path_extends", "pc.path_smoothen"}) {
		auto d = Graph(type);
		if (type == "pc.path_extends") d.Nodes[0].Values.push_back({"length", 5.});
		d.Nodes[1].Values.back().Data = .5000001;
		d.Nodes.push_back(
			{"second", "pc.path_sample", "", {}, {{"type", EnumValue{2}}, {"ratio", .5000002}}}
		);
		d.Links.push_back({"modify", "path", "second", "path"});
		d.Outputs.push_back({"second", "second", "position"});
		Plan plan;
		Diagnostic error;
		StatefulOutputEvaluationResult result;
		const std::array<std::string, 2> ids{"position", "second"};
		REQUIRE(Compile(d, plan, error) == Status::Ok);
		REQUIRE(EvaluateStatefulOutputs(d, plan, ids, {}, result, error) == Status::Ok);
		CHECK(
			std::get<EvaluatedValue>(result.Outputs[0].Output).Data ==
			std::get<EvaluatedValue>(result.Outputs[1].Output).Data
		);
		const auto first = std::get<EvaluatedValue>(result.Outputs[0].Output).Data;
		std::swap(d.Nodes[1], d.Nodes[2]);
		REQUIRE(Compile(d, plan, error) == Status::Ok);
		REQUIRE(EvaluateStatefulOutputs(d, plan, ids, {}, result, error) == Status::Ok);
		CHECK(
			std::get<EvaluatedValue>(result.Outputs[0].Output).Data ==
			std::get<EvaluatedValue>(result.Outputs[1].Output).Data
		);
		CHECK(std::get<EvaluatedValue>(result.Outputs[0].Output).Data != first);
	}
}
TEST_CASE(
	"A failed later sampling node discards all candidate sequential buffers and caches",
	"[source_path_sequential]"
) {
	auto d = Graph("pc.path_smoothen");
	Plan plan;
	Diagnostic error;
	StatefulOutputEvaluationResult result;
	const std::array<std::string, 1> initial{"position"};
	EvaluationRequest request;
	request.DataReplay = &result.Data;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStatefulOutputs(d, plan, initial, request, result, error) == Status::Ok);
	const auto prior = result.Data;
	const auto snapshot = prior;
	const auto sentinel = result;
	d.Nodes.push_back({"bad", "pc.path_smoothen", "", {}, {{"path", Line()}, {"step", int64_t{0}}}});
	d.Nodes.push_back({"bad_sample", "pc.path_sample", "", {}, {{"ratio", .5}}});
	d.Links.push_back({"bad", "path", "bad_sample", "path"});
	d.Outputs.push_back({"bad", "bad_sample", "position"});
	const std::array<std::string, 2> outputs{"position", "bad"};
	request.DataReplay = &prior;
	request.Tick = 1;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	CHECK(EvaluateStatefulOutputs(d, plan, outputs, request, result, error) == Status::InvalidValue);
	CHECK(result.Data == sentinel.Data);
	CHECK(prior == snapshot);
	CHECK(
		std::get<EvaluatedValue>(result.Outputs[0].Output).Data ==
		std::get<EvaluatedValue>(sentinel.Outputs[0].Output).Data
	);
	d.Nodes[2].Values.back().Data = int64_t{1};
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStatefulOutputs(d, plan, outputs, request, result, error) == Status::Ok);
	CHECK(prior == snapshot);
}
TEST_CASE(
	"Actual returned 3D point class belongs to the sampler across rows ticks and reset",
	"[source_path_sequential]"
) {
	PathValue3D raw;
	auto &data = raw.Data.emplace();
	data.Anchors = {{{0, 0, 0, 0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0, 0, 0, 0}, 0}};
	auto transformed = raw;
	PathTransform3D transform;
	transform.Position.Z = 3;
	transformed.Data->Transforms.push_back(transform);
	Document d;
	d.FormatVersion = 9;
	d.Nodes = {{"sample", "pc.path_sample", "", {}, {{"ratio", .5}}}};
	d.Junctions = {{"saved", "", ValueType::Path3D, transformed}};
	d.Links = {{"saved", "value", "sample", "path"}};
	d.Outputs = {{"position", "sample", "position"}};
	CHECK(std::get<Vector2>(Evaluate(d)) == Vector2{5, 0});
	d.Junctions[0].Default = raw;
	Plan plan;
	Diagnostic error;
	StatefulEvaluationResult result;
	EvaluationRequest request;
	request.DataReplay = &result.Data;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStateful(d, plan, "position", request, result, error) == Status::Ok);
	CHECK(std::get<Vector3>(std::get<EvaluatedValue>(result.Output).Data) == Vector3{5, 0, 0});
	const auto prior = result.Data;
	const auto original = prior;
	d.Junctions[0].Default = transformed;
	request.Tick = 1;
	request.DataReplay = &prior;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStateful(d, plan, "position", request, result, error) == Status::Ok);
	CHECK(std::get<Vector3>(std::get<EvaluatedValue>(result.Output).Data) == Vector3{5, 0, 3});
	CHECK(prior == original);
	request.Tick = 0;
	request.DataReplay = nullptr;
	REQUIRE(EvaluateStateful(d, plan, "position", request, result, error) == Status::Ok);
	CHECK(std::get<Vector2>(std::get<EvaluatedValue>(result.Output).Data) == Vector2{5, 0});
	d.Nodes.insert(d.Nodes.begin(), {"rows", "pc.array", "", {}, {{"type", EnumValue{0}}}});
	d.Nodes[0].DynamicInputs = {
		{"input_0", ValueType::Any, std::nullopt}, {"input_1", ValueType::Any, std::nullopt}
	};
	d.Junctions = {{"raw", "", ValueType::Path3D, raw}, {"transform", "", ValueType::Path3D, transformed}};
	d.Links = {
		{"raw", "value", "rows", "input_0"},
		{"transform", "value", "rows", "input_1"},
		{"rows", "array", "sample", "path"}
	};
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	const auto rowsStatus = EvaluateStateful(d, plan, "position", request, result, error);
	INFO(error.Message << " " << error.NodeId << ":" << error.Port);
	REQUIRE(rowsStatus == Status::Ok);
	const auto &positions = std::get<ArrayValue>(std::get<EvaluatedValue>(result.Output).Data);
	CHECK(positions.ElementType == ValueType::Vector3);
	REQUIRE(positions.Elements.size() == 2);
	CHECK(std::get<Vector3>(positions.Elements[0]) == Vector3{5, 0, 0});
	CHECK(std::get<Vector3>(positions.Elements[1]) == Vector3{5, 0, 3});
}
TEST_CASE(
	"Mixed spatial sequential wrappers preserve ignored versus assigned replacement semantics",
	"[source_path_sequential]"
) {
	PathValue3D spatial;
	auto &data = spatial.Data.emplace();
	data.Anchors = {{{0, 0, 0, 0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0, 0, 0, 0}, 0}};
	for (const std::string type : {"pc.path_extends", "pc.path_flattern", "pc.path_smoothen"}) {
		auto d = Graph(type);
		d.Nodes[0].Values.clear();
		if (type == "pc.path_extends") d.Nodes[0].Values = {{"side", EnumValue{1}}, {"length", 0.}};
		d.Junctions = {{"spatial", "", ValueType::Path3D, spatial}};
		d.Links.push_back({"spatial", "value", "modify", "path"});
		CHECK(std::get<Vector2>(Evaluate(d)) == Vector2{type == "pc.path_smoothen" ? 5. : 0., 0});
	}
}
TEST_CASE(
	"A spatial caller keeps its class when the next row is a planar sequential wrapper",
	"[source_path_sequential]"
) {
	PathValue3D raw;
	auto &spatial = raw.Data.emplace();
	spatial.Anchors = {{{0, 0, 2, 0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 2, 0, 0, 0, 0, 0, 0}, 0}};
	auto d = Graph();
	d.Nodes[0].Values.push_back({"length", 0.});
	d.Nodes.insert(
		d.Nodes.begin() + 1, {"rows", "pc.array", "", {}, {{"type", EnumValue{0}}, {"spread_array", true}}}
	);
	d.Nodes[1].DynamicInputs = {
		{"input_0", ValueType::Any, std::nullopt}, {"input_1", ValueType::Any, std::nullopt}
	};
	ArrayValue rawItems;
	rawItems.ElementType = ValueType::Any;
	rawItems.Items.push_back({raw});
	d.Junctions = {{"raw", "", ValueType::Array, rawItems}};
	d.Links = {
		{"raw", "value", "rows", "input_0"},
		{"modify", "path", "rows", "input_1"},
		{"rows", "array", "sample", "path"}
	};
	const auto values = std::get<ArrayValue>(Evaluate(d));
	REQUIRE(values.ElementType == ValueType::Vector3);
	REQUIRE(values.Elements.size() == 2);
	CHECK(std::get<Vector3>(values.Elements[0]) == Vector3{5, 0, 2});
	CHECK(std::get<Vector3>(values.Elements[1]) == Vector3{5, 0, 2});
}
TEST_CASE(
	"Sequential owned cache validation and retained capacity are explicit", "[source_path_sequential]"
) {
	auto d = Graph();
	auto path = std::get<Path2D>(Evaluate(d, "path"));
	auto &state = *path.SourceOperation->Sequential;
	state.Cache.reserve(7);
	CHECK(
		detail::SourcePath2DBytes<true>(path) - detail::SourcePath2DBytes<false>(path) ==
		7 * sizeof(SourcePathSequentialCachePoint)
	);
	state.Cache = {{{.001}, 0, {{2, 3}, 4}}, {{.004}, 0, {{5, 6}, 7}}};
	CHECK_FALSE(detail::ValidSourcePath2D(path));
	state.Cache[0].Coordinate = 0;
	CHECK(detail::ValidSourcePath2D(path));
	state.Cache[0].Line = uint32_t(Limits::MaximumArrayElements + 1);
	CHECK_FALSE(detail::ValidSourcePath2D(path));
	state.Cache[0].Line = 0;
	state.Cache[0].Coordinate = std::numeric_limits<double>::infinity();
	CHECK_FALSE(detail::ValidSourcePath2D(path));
}
TEST_CASE(
	"An explicitly empty path is distinct from the catalogue noone default", "[source_path_sequential]"
) {
	auto d = Graph();
	d.Nodes[0].Values = {{"path", Path2D{}}, {"length", 4.}};
	CHECK(std::get<Vector2>(Evaluate(d)) == Vector2{2, 0});
	d.Nodes[0].Values.erase(d.Nodes[0].Values.begin());
	CHECK(std::get<Path2D>(Evaluate(d, "path")) == Path2D{});
}
TEST_CASE(
	"Unvisited retained paths cannot import prior tags into the current journal namespace",
	"[source_path_sequential]"
) {
	auto d = Graph();
	d.Nodes[0].Values.push_back({"length", 5.});
	d.Nodes.push_back({"untouched", "pc.path_extends", "", {}, {{"path", Line()}, {"length", 2.}}});
	d.Outputs.push_back({"untouched", "untouched", "path"});
	Plan plan;
	Diagnostic error;
	StatefulEvaluationResult result;
	EvaluationRequest request;
	request.DataReplay = &result.Data;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStateful(d, plan, "untouched", request, result, error) == Status::Ok);
	auto prior = result.Data;
	REQUIRE(prior.Entries.size() == 1);
	auto &old = std::get<Path2D>(prior.Entries[0].Values[0].Data);
	old.SourceOperation->EvaluationMemoId = 1;
	const auto expected = old;
	const auto original = prior;
	request.DataReplay = &prior;
	REQUIRE(EvaluateStateful(d, plan, "position", request, result, error) == Status::Ok);
	CHECK(prior == original);
	const auto entry =
		std::find_if(result.Data.Entries.begin(), result.Data.Entries.end(), [](const auto &state) {
			return state.NodeId == "untouched";
		});
	REQUIRE(entry != result.Data.Entries.end());
	const auto &retained = std::get<Path2D>(entry->Values[0].Data);
	CHECK(retained == expected);
	CHECK(retained.SourceOperation->EvaluationMemoId == 0);
	CHECK(retained.SourceOperation->Sequential->Cache.empty());
}
TEST_CASE(
	"Fresh unconnected Smoothen exposes its source default boundary to Extends", "[source_path_sequential]"
) {
	auto d = Graph("pc.path_smoothen");
	d.Nodes[0].Values.clear();
	d.Nodes.push_back({"extent", "pc.path_extends", "", {}, {{"length", 4.}}});
	d.Links.push_back({"modify", "path", "extent", "path"});
	d.Outputs.push_back({"extent", "extent", "path"});
	const auto path = std::get<Path2D>(Evaluate(d, "extent"));
	CHECK(path.SourceOperation->Sequential->CachedBounds == Vector4{0, 0, 1, 1});
	CHECK(path.SourceOperation->Sequential->CachedLength == 0);
	CHECK(path.SourceOperation->Sequential->CachedSegments == 1);
}

TEST_CASE(
	"Live cache-group paths retain late consumer sampling after producer output release",
	"[source_path_sequential][frame_cache_groups]"
) {
	auto document = Graph("pc.path_smoothen");
	document.Nodes.front().Values.push_back({"span", .1});
	document.Nodes.front().Values.push_back({"step", int64_t{2}});
	DataReplayState prior;
	REQUIRE(
		RetainCacheGroupReplayNode(
			prior.CacheGroups, "modify", "pc.path_smoothen", {}, Limits::MaximumEvaluationBytes
		)
			.Code == Status::Ok
	);
	EvaluationRequest request;
	request.DataReplay = &prior;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult result;
	const auto evaluated = EvaluateStateful(document, plan, "position", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	const auto &path = std::get<Path2D>(*result.Data.CacheGroups.Nodes.front().Outputs.front().Data);
	REQUIRE(path.SourceOperation);
	REQUIRE(path.SourceOperation->Sequential);
	CHECK(path.SourceOperation->EvaluationMemoId == 0);
	CHECK_FALSE(path.SourceOperation->Sequential->Cache.empty());
	CHECK(prior.CacheGroups.Nodes.front().Outputs.empty());
}
