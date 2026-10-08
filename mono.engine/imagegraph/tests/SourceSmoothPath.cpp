#include "nodes/Path.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_smooth_path")
using namespace engine::imagegraph;
namespace smooth_path_test {
	struct Graph {
		Document Doc;
		Plan Compiled;
		Diagnostic Error;
		Graph() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"smooth", "pc.path_smooth", "", {}, {}},
				{"sample", "pc.path_sample", "", {}, {{"ratio", .5}, {"type", EnumValue{2}}}}
			};
			Doc.Links = {{"smooth", "path_data", "sample", "path"}};
			Doc.Outputs = {
				{"path", "smooth", "path_data"},
				{"position", "smooth", "position_out"},
				{"sample", "sample", "position"}
			};
			Anchors({Vector2{0, 0}, Vector2{32, 0}});
		}
		void Set(std::string_view id, Value value) {
			for (auto &input : Doc.Nodes[0].DynamicInputs)
				if (input.Id == id) {
					input.Default = std::move(value);
					return;
				}
			for (auto &input : Doc.Nodes[0].Values)
				if (input.Port == id) {
					input.Data = std::move(value);
					return;
				}
			Doc.Nodes[0].Values.push_back({std::string(id), std::move(value)});
		}
		void Link(std::string_view id, Value value, ValueType type = ValueType::Any) {
			const auto producer = "linked_" + std::string(id);
			Doc.Junctions.push_back({producer, "", type, std::move(value)});
			Doc.Links.push_back({producer, "value", "smooth", std::string(id)});
		}
		void Anchors(std::initializer_list<Vector2> points) {
			Doc.Nodes[0].DynamicInputs.clear();
			std::erase_if(Doc.Nodes[0].Values, [](const auto &input) {
				return input.Port.starts_with("anchor_");
			});
			size_t i = 0;
			for (auto point : points)
				Doc.Nodes[0].DynamicInputs.push_back(
					{"anchor_" + std::to_string(i++), ValueType::Vector2, Value{point}}
				);
		}
		void CompileGraph() {
			const auto status = Compile(Doc, Compiled, Error);
			INFO(Error.Message << " " << Error.NodeId << ":" << Error.Port);
			REQUIRE(status == Status::Ok);
		}
		Value Run(std::string output = "path") {
			CompileGraph();
			EvaluatedValue value;
			const auto status = EvaluateValue(Doc, Compiled, output, {}, value, Error);
			INFO(Error.Message << " " << Error.NodeId << ":" << Error.Port);
			REQUIRE(status == Status::Ok);
			return value.Data;
		}
		void Refused(Status expected) {
			CompileGraph();
			EvaluatedValue value;
			value.Data = Vector2{71, 83};
			const auto before = value;
			const auto status = EvaluateValue(Doc, Compiled, "path", {}, value, Error);
			INFO(Error.Message << " " << Error.NodeId << ":" << Error.Port);
			CHECK(status == expected);
			CHECK(value == before);
			CHECK_FALSE(Error.Message.empty());
		}
	};
	struct Runtime {
		Node Authored{"sample", "pc.path_sample", "", {}, {}};
		EvaluationRequest Request;
		detail::NodeContext Context{Authored, *FindCatalogueEntry("pc.path_sample"), Request};
		detail::SourcePathShiftMemo Memo;
		detail::PathRuntime Path;
		explicit Runtime(const Path2D &path) {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
			Context.PathShiftMemo = &Memo;
			Context.Values = {{"path", path}};
			REQUIRE(detail::StampSourcePathShiftInputs(Context));
			const auto ready = Path.Init(Context, std::get<Path2D>(Context.Values[0].second));
			INFO(Context.FailureMessage);
			REQUIRE(ready);
		}
	};
	void Near(Vector2 point, double x, double y) {
		CHECK(point.X == Catch::Approx(x).margin(1e-8));
		CHECK(point.Y == Catch::Approx(y).margin(1e-8));
	}
	void Near(const Value &value, double x, double y) {
		Near(std::get<Vector2>(value), x, y);
	}
	void Near(detail::PathPoint point, double x, double y) {
		Near(Vector2{point.X, point.Y}, x, y);
	}
	double Cubic(double a, double b, double c, double d, double t) {
		const auto q = 1 - t;
		return q * q * q * a + 3 * q * q * t * b + 3 * q * t * t * c + t * t * t * d;
	}
	// Source measures 32 points per segment, including its start and excluding its endpoint.
	double LengthOracle(const Path2D &path) {
		double total = 0;
		const auto count = path.Anchors.size();
		for (size_t i = 0; i < (path.Loop ? count : count - 1); ++i) {
			const auto &a = path.Anchors[i].Controls, &b = path.Anchors[(i + 1) % count].Controls;
			Vector2 previous{a[0], a[1]};
			for (int j = 1; j < 32; ++j) {
				const auto t = double(j) / 32;
				const bool straight = a[4] == 0 && a[5] == 0 && b[2] == 0 && b[3] == 0;
				Vector2 point{
					straight ? a[0] + (b[0] - a[0]) * t : Cubic(a[0], a[0] + a[4], b[0] + b[2], b[0], t),
					straight ? a[1] + (b[1] - a[1]) * t : Cubic(a[1], a[1] + a[5], b[1] + b[3], b[1], t)
				};
				total += std::hypot(point.X - previous.X, point.Y - previous.Y);
				previous = point;
			}
		}
		return total;
	}
}
using namespace smooth_path_test;

TEST_CASE(
	"Smooth Path excludes segment endpoints from metrics but samples the whole segment",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	auto path = std::get<Path2D>(graph.Run());
	REQUIRE(path.SourceSmooth);
	CHECK(path.SourceSmooth->NormalizedLength);
	Runtime runtime(path);
	CHECK(runtime.Path.Length() == 31);
	CHECK(runtime.Path.LineCount() == 1);
	CHECK(runtime.Path.SegmentCount() == 1);
	CHECK(runtime.Path.AccumulatedCount() == 1);
	CHECK(runtime.Path.AccumulatedAt(0) == 31);
	CHECK(runtime.Path.MinX == 0);
	CHECK(runtime.Path.MaxX == 31);
	Near(runtime.Path.PointRatio(.5), 16, 0);
	Near(runtime.Path.PointDistance(5), 160. / 31, 0);
	Near(runtime.Path.PointRatio(1), 0, 0);
	Near(runtime.Path.PointRatio(-.25), 0, 0);
	Near(runtime.Path.PointDistance(100), 32, 0);
	Near(graph.Run("sample"), 16, 0);
	SourcePathPointBuffer buffer{SourcePathPointClass::Spatial, {99, 88}, 17., 6};
	runtime.Path.PointRatioInto(.5, 0, buffer);
	Near(buffer.Position, 16, 0);
	CHECK(buffer.Class == SourcePathPointClass::Spatial);
	CHECK(buffer.Z == 17);
	CHECK(buffer.Weight == 1);
}

TEST_CASE(
	"Smooth equal anchor sampling visits the closing segment of an open path", "[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Set("normalized_length", false);
	Runtime runtime(std::get<Path2D>(graph.Run()));
	for (const auto &[ratio, x] :
		 std::array<std::pair<double, double>, 4>{{{.25, 16}, {.5, 32}, {.75, 16}, {1, 0}}})
		Near(runtime.Path.PointRatio(ratio), x, 0);
	Near(runtime.Path.PointDistance(31), 0, 0);
	graph.Set("sample_mode", EnumValue{1});
	graph.Set("sample_path", 1.25);
	Near(graph.Run("position"), 24, 0);
	graph.Set("loop", true);
	Runtime loop(std::get<Path2D>(graph.Run()));
	CHECK(loop.Path.Length() == 62);
	CHECK(loop.Path.SegmentCount() == 2);
	CHECK(loop.Path.MaxX == 32);
	Near(loop.Path.PointRatio(-.25), 16, 0);
}

TEST_CASE(
	"Smooth cardinal handles follow wrapped neighbors and clear open endpoint handles",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Anchors({{0, 0}, {6, 0}, {6, 8}});
	graph.Set("sample_mode", EnumValue{1});
	graph.Set("sample_path", .5);
	auto path = std::get<Path2D>(graph.Run());
	REQUIRE(path.Anchors.size() == 3);
	const auto &middle = path.Anchors[1].Controls;
	CHECK(middle[2] == Catch::Approx(-1.2));
	CHECK(middle[3] == Catch::Approx(-1.6));
	CHECK(middle[4] == Catch::Approx(1.6));
	CHECK(middle[5] == Catch::Approx(32. / 15));
	for (size_t i : {size_t{0}, size_t{2}})
		for (size_t j = 2; j < 6; ++j)
			CHECK(path.Anchors[i].Controls[j] == 0);
	Near(graph.Run("position"), 2.55, -.6);
	Runtime runtime(path);
	CHECK(runtime.Path.Length() == Catch::Approx(LengthOracle(path)).margin(1e-8));
	graph.Set("sample_path", 1.5);
	Near(graph.Run("position"), 6.6, 4.8);
	graph.Set("smoothness", 6.);
	Near(graph.Run("position"), 6.3, 4.4);
	graph.Set("loop", true);
	path = std::get<Path2D>(graph.Run());
	CHECK(path.Anchors[0].Controls[3] == Catch::Approx(10. / 6));
	CHECK(path.Anchors[0].Controls[5] == Catch::Approx(-1));
	Runtime loop(path);
	CHECK(loop.Path.Length() == Catch::Approx(LengthOracle(path)).margin(1e-8));
	CHECK(loop.Path.SegmentCount() == 3);
}

TEST_CASE(
	"Smooth rounds positive and negative half ties before generating geometry", "[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Anchors({{.5, -.5}, {31.5, 1.5}});
	graph.Set("round_anchor", true);
	auto path = std::get<Path2D>(graph.Run());
	CHECK(path.Anchors[0].Controls[0] == 0);
	CHECK(path.Anchors[0].Controls[1] == 0);
	CHECK(path.Anchors[1].Controls[0] == 32);
	CHECK(path.Anchors[1].Controls[1] == 2);
	graph.Anchors({{-1.5, -2.5}, {-3.5, -4.5}});
	path = std::get<Path2D>(graph.Run());
	CHECK(path.Anchors[0].Controls[0] == -2);
	CHECK(path.Anchors[0].Controls[1] == -2);
	CHECK(path.Anchors[1].Controls[0] == -4);
	CHECK(path.Anchors[1].Controls[1] == -4);
}

TEST_CASE(
	"Smooth dynamic sockets flatten coordinate rows in source input order and ignore depth three",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Anchors({{0, 0}, {32, 0}});
	graph.Set("anchor_0", ArrayValue{ValueType::Vector2, {Vector2{1, 2}, Vector2{3, 4}}});
	graph.Set("anchor_1", Vector2{5, 6});
	auto path = std::get<Path2D>(graph.Run());
	REQUIRE(path.Anchors.size() == 3);
	CHECK(path.Anchors[0].Controls[0] == 1);
	CHECK(path.Anchors[1].Controls[0] == 3);
	CHECK(path.Anchors[2].Controls[0] == 5);
	ArrayValue spatialRows;
	spatialRows.ElementType = ValueType::Any;
	spatialRows.Items = {{ElementValue{Vector3{7, 8, 99}}}, {ElementValue{Vector3{9, 10, -73}}}};
	graph.Set("anchor_0", spatialRows);
	path = std::get<Path2D>(graph.Run());
	REQUIRE(path.Anchors.size() == 3);
	CHECK(path.Anchors[0].Controls[0] == 7);
	CHECK(path.Anchors[0].Controls[1] == 8);
	CHECK(path.Anchors[1].Controls[0] == 9);
	CHECK(path.Anchors[1].Controls[1] == 10);
	CHECK(path.Anchors[2].Controls[0] == 5);
	Document restored;
	const auto decoded = Read(Write(graph.Doc), restored, graph.Error);
	INFO(graph.Error.Message);
	REQUIRE(decoded == Status::Ok);
	CHECK(restored == graph.Doc);
	graph.Doc = std::move(restored);
	path = std::get<Path2D>(graph.Run());
	REQUIRE(path.Anchors.size() == 3);
	CHECK(path.Anchors[0].Controls[0] == 7);
	CHECK(path.Anchors[0].Controls[1] == 8);
	CHECK(path.Anchors[1].Controls[0] == 9);
	CHECK(path.Anchors[1].Controls[1] == 10);
	ArrayValue deep;
	deep.ElementType = ValueType::Any;
	deep.Items = {
		{std::vector<SourceArrayItem>{{std::vector<SourceArrayItem>{{ElementValue{9.}}, {ElementValue{9.}}}}}}
	};
	graph.Set("anchor_0", deep);
	path = std::get<Path2D>(graph.Run());
	REQUIRE(path.Anchors.size() == 1);
	CHECK(path.Anchors[0].Controls[0] == 5);
	graph.Set("anchor_0", ArrayValue{ValueType::Scalar, {1.}});
	graph.Refused(Status::UnsupportedExecution);
}

TEST_CASE(
	"Smooth constructor emptiness and single anchor sampling preserve source distinctions",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Anchors({});
	Near(graph.Run("position"), 0, 0);
	auto path = std::get<Path2D>(graph.Run());
	Runtime empty(path);
	CHECK(empty.Path.LineCount() == 1);
	CHECK(empty.Path.Length() == 0);
	CHECK(empty.Path.SegmentCount() == 0);
	Near(empty.Path.PointRatio(.7), 0, 0);
	graph.Anchors({{7, 11}});
	Near(graph.Run("position"), 0, 0);
	graph.Set("sample_mode", EnumValue{1});
	graph.Set("sample_path", .75);
	Near(graph.Run("position"), 7, 11);
	graph.Anchors({{7, 11}, {7, 11}});
	Near(graph.Run("position"), 7, 11);
	graph.Set("sample_mode", EnumValue{0});
	graph.Refused(Status::InvalidValue);
}

TEST_CASE(
	"Smooth refuses undefined negative segment indices and nonfinite controls atomically",
	"[imagegraph][path_smooth]"
) {
	for (double sample : {-.2, -2.}) {
		Graph graph;
		graph.Set("sample_mode", EnumValue{1});
		graph.Set("sample_path", sample);
		graph.Refused(Status::InvalidValue);
	}
	Graph fractional;
	fractional.Set("sample_mode", .5);
	fractional.Set("sample_path", .75);
	Near(fractional.Run("position"), 0, 0);
	for (const auto port : {"sample_path", "smoothness"}) {
		Graph graph;
		graph.Set(port, std::numeric_limits<double>::infinity());
		Plan plan;
		Diagnostic diagnostic;
		CHECK(Compile(graph.Doc, plan, diagnostic) == Status::InvalidValue);
	}
	Graph zero;
	zero.Anchors({{0, 0}, {6, 0}, {6, 8}});
	zero.Set("smoothness", 0.);
	zero.Refused(Status::InvalidValue);
	Graph overflow;
	overflow.Anchors({{std::numeric_limits<double>::max(), 0}, {-std::numeric_limits<double>::max(), 0}});
	overflow.Refused(Status::InvalidValue);
	Graph tooMany;
	ArrayValue rows;
	rows.ElementType = ValueType::Vector2;
	for (size_t i = 0; i <= Limits::MaximumPathAnchors; ++i)
		rows.Elements.push_back(Vector2{double(i), 0});
	tooMany.Set("anchor_0", rows);
	tooMany.Refused(Status::LimitExceeded);
}

TEST_CASE(
	"Smooth empty refresh retains position history and byte refusal preserves the caller result",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Set("sample_path", .5);
	graph.CompileGraph();
	StatefulEvaluationResult state;
	EvaluationRequest request;
	request.DataReplay = &state.Data;
	REQUIRE(
		EvaluateStateful(graph.Doc, graph.Compiled, "position", request, state, graph.Error) == Status::Ok
	);
	Near(std::get<EvaluatedValue>(state.Output).Data, 16, 0);
	const auto previous = state.Data;
	const auto output = std::get<EvaluatedValue>(state.Output);
	graph.Anchors({});
	graph.CompileGraph();
	request.Tick = 1;
	CHECK(
		EvaluateStateful(graph.Doc, graph.Compiled, "position", request, state, graph.Error, 1) ==
		Status::LimitExceeded
	);
	CHECK(state.Data == previous);
	CHECK(std::get<EvaluatedValue>(state.Output) == output);
	REQUIRE(
		EvaluateStateful(graph.Doc, graph.Compiled, "position", request, state, graph.Error) == Status::Ok
	);
	Near(std::get<EvaluatedValue>(state.Output).Data, 16, 0);
	CHECK(state.Data.Entries.size() == 1);
	REQUIRE(state.Data.Entries[0].Values.size() == 1);
	Near(state.Data.Entries[0].Values[0].Data, 16, 0);
	Graph fresh;
	fresh.Anchors({});
	Near(fresh.Run("position"), 0, 0);
}

TEST_CASE(
	"Smooth native format nine preserves policy in scalar flat and nested path values",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Set("normalized_length", false);
	const auto path = std::get<Path2D>(graph.Run());
	Document saved;
	saved.FormatVersion = 9;
	saved.Nodes = {{"argument", "pc.argument", "", {}, {{"default_value", path}, {"type", EnumValue{1}}}}};
	saved.Nodes.push_back(
		{"flat", "pc.argument", "", {}, {{"default_value", ArrayValue{ValueType::Path2D, {path, path}}}}}
	);
	ArrayValue nested;
	nested.ElementType = ValueType::Path2D;
	nested.Nested = {{path}, {path, path}};
	saved.Nodes.push_back({"nested", "pc.argument", "", {}, {{"default_value", nested}}});
	Document restored;
	Diagnostic error;
	const auto decoded = Read(Write(saved), restored, error);
	INFO(error.Message);
	REQUIRE(decoded == Status::Ok);
	CHECK(restored == saved);
	Runtime runtime(std::get<Path2D>(restored.Nodes[0].Values[0].Data));
	CHECK(runtime.Path.Length() == 31);
	Near(runtime.Path.PointRatio(.75), 16, 0);
	const auto &flat = std::get<ArrayValue>(restored.Nodes[1].Values[0].Data);
	for (const auto &leaf : flat.Elements) {
		Runtime row(std::get<Path2D>(leaf));
		CHECK(row.Path.Length() == 31);
	}
	const auto &tree = std::get<ArrayValue>(restored.Nodes[2].Values[0].Data);
	for (const auto &row : tree.Nested)
		for (const auto &leaf : row) {
			Runtime item(std::get<Path2D>(leaf));
			CHECK(item.Path.Length() == 31);
		}
}

TEST_CASE(
	"Smooth reverse and transform wrappers retain source metrics through public sampling",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Doc.Nodes.push_back({"reverse", "pc.path_reverse", "", {}, {}});
	graph.Doc.Nodes.push_back(
		{"transform",
		 "pc.path_transform",
		 "",
		 {},
		 {{"scale", Vector2{2, 3}},
		  {"position", Vector2{0, 0}},
		  {"position_unit", EnumValue{0}},
		  {"anchor", Vector2{0, 0}},
		  {"anchor_unit", EnumValue{0}}}}
	);
	graph.Doc.Links = {
		{"smooth", "path_data", "reverse", "path"},
		{"reverse", "path", "transform", "path"},
		{"transform", "path", "sample", "path"}
	};
	graph.Doc.Outputs.push_back({"wrapped", "transform", "path"});
	const auto path = std::get<Path2D>(graph.Run("wrapped"));
	Runtime runtime(path);
	CHECK(runtime.Path.Length() == 31);
	Near(graph.Run("sample"), 32, 0);
}

TEST_CASE(
	"Smooth source getters use numeric diagonals and physical surface dimensions for linked anchors",
	"[imagegraph][path_smooth]"
) {
	Graph numeric;
	numeric.Doc.Project.emplace();
	numeric.Doc.Project->SurfaceWidth = 100;
	numeric.Doc.Project->SurfaceHeight = 200;
	numeric.Doc.Junctions = {{"number", "", ValueType::Scalar, 12.}};
	numeric.Doc.Links.push_back({"number", "value", "smooth", "anchor_1"});
	auto path = std::get<Path2D>(numeric.Run());
	CHECK(path.Anchors[1].Controls[0] == 12);
	CHECK(path.Anchors[1].Controls[1] == 12);
	Graph surface;
	surface.Doc.Nodes.push_back(
		{"surface",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{3}}, {"height", int64_t{7}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	surface.Doc.Links.push_back({"surface", "image", "smooth", "anchor_1"});
	path = std::get<Path2D>(surface.Run());
	CHECK(path.Anchors[1].Controls[0] == 3);
	CHECK(path.Anchors[1].Controls[1] == 7);
}

TEST_CASE(
	"Smooth controls stay one source update and widget mode clamps before dispatch",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Set("sample_mode", 3.);
	graph.Set("sample_path", 1.25);
	Near(graph.Run("position"), 24, 0);
	graph.Set("sample_mode", -1.);
	graph.Set("sample_path", .25);
	Near(graph.Run("position"), 8, 0);
	for (const auto port : {"loop", "round_anchor", "normalized_length"}) {
		Graph array;
		array.Link(port, ArrayValue{ValueType::Boolean, {false, true}});
		array.Refused(Status::UnsupportedExecution);
	}
	Graph samples;
	samples.Link("sample_path", ArrayValue{ValueType::Scalar, {0., .5}});
	samples.Refused(Status::UnsupportedExecution);
}

TEST_CASE(
	"Smooth exact distance memo shares full point weights and keeps unclamped keys distinct",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Set("sample_path", .5);
	const auto path = std::get<Path2D>(graph.Run());
	REQUIRE(path.SourceSmooth);
	REQUIRE_FALSE(path.SourceSmooth->Cache.empty());
	const auto &initial = path.SourceSmooth->Cache.front();
	CHECK(initial.Distance == 15.5);
	CHECK(initial.Weight == 1);
	Near(initial.Position, 16, 0);
	Runtime runtime(path);
	SourcePathPointBuffer hit{SourcePathPointClass::Spatial, {71, 83}, 17., 3};
	runtime.Path.PointDistanceInto(15.5, 0, hit);
	Near(hit.Position, 16, 0);
	CHECK(hit.Weight == 1);
	CHECK(hit.Class == SourcePathPointClass::Spatial);
	CHECK(hit.Z == 17);
	auto sample = [&](double distance, double weight) {
		SourcePathPointBuffer out;
		out.Weight = weight;
		runtime.Path.PointDistanceInto(distance, 0, out);
		REQUIRE(runtime.Context.FailureCode == Status::Ok);
		return out;
	};
	const auto before = runtime.Memo.Entries.size();
	const auto a = sample(1., 4), b = sample(1. + 1e-8, 7);
	CHECK(a.Weight == 4);
	CHECK(b.Weight == 7);
	CHECK(b.Position.X > a.Position.X);
	CHECK(runtime.Memo.Entries.size() == before + 2);
	CHECK(sample(1., 9).Weight == 4);
	CHECK(sample(1. + 1e-8, 9).Weight == 7);
	const auto left = sample(-1, 5), other = sample(-2, 8);
	Near(left.Position, 0, 0);
	Near(other.Position, 0, 0);
	CHECK(left.Weight == 5);
	CHECK(other.Weight == 8);
	CHECK(sample(-1, 9).Weight == 5);
	CHECK(sample(-2, 9).Weight == 8);
	detail::PathRuntime second;
	REQUIRE(second.Init(runtime.Context, std::get<Path2D>(runtime.Context.Values[0].second)));
	SourcePathPointBuffer shared;
	shared.Weight = 13;
	second.PointDistanceInto(1., 0, shared);
	CHECK(shared.Weight == 4);
	Near(shared.Position, 32. / 31, 0);
	Graph unnormalized;
	unnormalized.Set("normalized_length", false);
	Runtime equal(std::get<Path2D>(unnormalized.Run()));
	const auto initialCount = equal.Memo.Entries.size();
	SourcePathPointBuffer caller;
	caller.Weight = 3;
	equal.Path.PointDistanceInto(5, 0, caller);
	CHECK(caller.Weight == 3);
	caller.Weight = 7;
	equal.Path.PointDistanceInto(5, 0, caller);
	CHECK(caller.Weight == 7);
	CHECK(equal.Memo.Entries.size() == initialCount);
}

TEST_CASE(
	"Smooth cached distance values survive native roundtrip and malformed cache is refused",
	"[imagegraph][path_smooth]"
) {
	Graph graph;
	graph.Set("sample_path", .5);
	auto path = std::get<Path2D>(graph.Run());
	REQUIRE(path.SourceSmooth);
	path.SourceSmooth->Cache.insert(path.SourceSmooth->Cache.begin(), {-1, {0, 0}, 5});
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"sample", "pc.path_sample", "", {}, {{"path", path}, {"ratio", .5}, {"type", EnumValue{2}}}}
	};
	document.Outputs = {{"weight", "sample", "weight"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Runtime runtime(std::get<Path2D>(restored.Nodes[0].Values[0].Data));
	SourcePathPointBuffer out;
	out.Weight = 11;
	runtime.Path.PointDistanceInto(-1, 0, out);
	CHECK(out.Weight == 5);
	Near(out.Position, 0, 0);
	path.SourceSmooth->Cache[0].Distance = std::numeric_limits<double>::quiet_NaN();
	document.Nodes[0].Values[0].Data = path;
	Plan plan;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
}
