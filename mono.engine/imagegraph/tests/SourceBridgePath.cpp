#include "nodes/Path.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_bridge_path")
using namespace engine::imagegraph;
namespace bridge_path_test {
	Path2D Line(Vector2 start = {0, 0}, Vector2 end = {32, 0}, double weight = 2) {
		Path2D path;
		path.Anchors = {{{start.X, start.Y, 0, 0, 0, 0}, 0}, {{end.X, end.Y, 0, 0, 0, 0}, 0}};
		path.Weights = {{0, weight}, {100, weight}};
		return path;
	}
	Path2D Combined(std::initializer_list<Path2D> children) {
		Path2D path;
		auto &operation = path.SourceOperation.emplace();
		operation.Kind = SourcePathOperationKind::Combine;
		operation.Inputs = children;
		return path;
	}
	struct Graph {
		Document Doc;
		Plan Compiled;
		Diagnostic Error;
		Graph() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"bridge",
				 "pc.path_bridge",
				 "",
				 {},
				 {{"path", Line()}, {"amount", int64_t{3}}, {"seed", 17.}}},
				{"sample", "pc.path_sample", "", {}, {{"ratio", .5}, {"type", EnumValue{2}}}}
			};
			Doc.Nodes[0].DynamicInputs = {{"path_0", ValueType::Path2D, Value{Line({32, 12}, {64, 12}, 6)}}};
			Doc.Links = {{"bridge", "path", "sample", "path"}};
			Doc.Outputs = {
				{"path", "bridge", "path"}, {"point", "sample", "position"}, {"weight", "sample", "weight"}
			};
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
		void CompileGraph() {
			const auto status = Compile(Doc, Compiled, Error);
			INFO(Error.Message << " " << Error.NodeId << ":" << Error.Port);
			REQUIRE(status == Status::Ok);
		}
		Value Run(std::string output = "path", EvaluationRequest request = {}) {
			CompileGraph();
			EvaluatedValue value;
			const auto status = EvaluateValue(Doc, Compiled, output, request, value, Error);
			INFO(Error.Message << " " << Error.NodeId << ":" << Error.Port);
			REQUIRE(status == Status::Ok);
			return value.Data;
		}
		void Sample(double ratio, int64_t line = 0) {
			Doc.Nodes[1].Values = {{"ratio", ratio}, {"type", EnumValue{2}}, {"path_index", line}};
		}
		void Refused(Status expected, EvaluationRequest request = {}, std::string output = "path") {
			CompileGraph();
			EvaluatedValue value;
			value.Data = Vector2{71, 83};
			const auto before = value;
			const auto status = EvaluateValue(Doc, Compiled, output, request, value, Error);
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
	void Near(detail::PathPoint point, double x, double y, double weight) {
		CHECK(point.X == Catch::Approx(x).margin(1e-8));
		CHECK(point.Y == Catch::Approx(y).margin(1e-8));
		CHECK(point.Weight == Catch::Approx(weight).margin(1e-8));
	}
	void Near(Value value, double x, double y) {
		const auto point = std::get<Vector2>(value);
		CHECK(point.X == Catch::Approx(x).margin(1e-8));
		CHECK(point.Y == Catch::Approx(y).margin(1e-8));
	}
	const SourcePathBridgeData2D &Data(const Path2D &path) {
		REQUIRE(path.SourceOperation);
		REQUIRE(path.SourceOperation->Kind == SourcePathOperationKind::Bridge);
		REQUIRE(path.SourceOperation->Bridge);
		return *path.SourceOperation->Bridge;
	}
	double Cubic(double a, double b, double c, double d, double t) {
		const auto q = 1 - t;
		return q * q * q * a + 3 * q * q * t * b + 3 * q * t * t * c + t * t * t * d;
	}
	double SmoothLength(const SourcePathBridgeLine2D &line) {
		double length = 0;
		for (size_t i = 0; i < line.Controls.size(); ++i) {
			const auto &a = line.Anchors[i], &b = line.Anchors[i + 1];
			const auto &control = line.Controls[i];
			Vector2 before{a.X, a.Y};
			for (int k = 1; k < 32; ++k) {
				const auto t = double(k) / 32;
				Vector2 current{
					Cubic(a.X, control[0], control[2], b.X, t), Cubic(a.Y, control[1], control[3], b.Y, t)
				};
				length += std::hypot(current.X - before.X, current.Y - before.Y);
				before = current;
			}
		}
		return length;
	}
}
using namespace bridge_path_test;

TEST_CASE(
	"Bridge Uniform distribution connects corresponding source path positions and retains weights",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	const auto path = std::get<Path2D>(graph.Run());
	const auto &data = Data(path);
	REQUIRE(data.Lines.size() == 3);
	CHECK(data.LineCount == 3);
	CHECK_FALSE(data.Smooth);
	const auto length = std::hypot(32., 12.);
	Runtime runtime(path);
	CHECK(runtime.Path.LineCount() == 3);
	CHECK_FALSE(runtime.Path.HasBoundary);
	for (size_t i = 0; i < 3; ++i) {
		const auto ratio = i == 2 ? .999 : double(i) / 2;
		REQUIRE(data.Lines[i].Anchors.size() == 2);
		CHECK(runtime.Path.SegmentCount(i) == 2);
		CHECK(runtime.Path.Length(i) == Catch::Approx(length));
		CHECK(runtime.Path.AccumulatedCount(i) == 1);
		CHECK(runtime.Path.AccumulatedAt(0, i) == Catch::Approx(length));
		Near(runtime.Path.PointRatio(0, i), 32 * ratio, 0, 2);
		Near(runtime.Path.PointRatio(.5, i), 32 * ratio + 16, 6, 4);
		Near(runtime.Path.PointRatio(1, i), 32 * ratio + 32, 12, 6);
	}
	graph.Sample(.5, 1);
	Near(graph.Run("point"), 32, 6);
	CHECK(std::get<double>(graph.Run("weight")) == 4);
}

TEST_CASE(
	"Bridge amount one samples halfway and amount zero owns an empty path", "[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.Set("amount", int64_t{1});
	Runtime one(std::get<Path2D>(graph.Run()));
	CHECK(one.Path.LineCount() == 1);
	Near(one.Path.PointRatio(0), 16, 0, 2);
	graph.Set("amount", int64_t{0});
	const auto path = std::get<Path2D>(graph.Run());
	CHECK(Data(path).LineCount == 0);
	CHECK(Data(path).Lines.empty());
	Runtime empty(path);
	CHECK(empty.Path.LineCount() == 0);
	CHECK(empty.Path.SegmentCount() == 0);
	SourcePathPointBuffer out{SourcePathPointClass::Spatial, {71, 83}, 17., 9};
	empty.Path.PointRatioInto(.5, 0, out);
	CHECK(out.Position == Vector2{0, 0});
	CHECK(out.Weight == 9);
	CHECK(out.Z == 17);
	CHECK(out.Class == SourcePathPointClass::Spatial);
}

TEST_CASE(
	"Bridge loop changes Uniform denominator and wraps only the authored offset and range",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.Set("amount", int64_t{4});
	graph.Set("loop", true);
	graph.Set("range", Vector2{-.25, 1.25});
	graph.Set("offset", -.5);
	Runtime loop(std::get<Path2D>(graph.Run()));
	for (size_t i = 0; i < 4; ++i) {
		const double raw = -.25 + 1.5 * double(i) / 4 - .5;
		const double wrapped = raw - std::floor(raw);
		Near(loop.Path.PointRatio(0, i), 32 * std::min(wrapped, .999), 0, 2);
	}
	graph.Set("loop", false);
	graph.Set("range", Vector2{.8, .2});
	graph.Set("offset", 123.);
	const auto initialOpen = std::get<Path2D>(graph.Run());
	Runtime open(initialOpen);
	for (size_t i = 0; i < 4; ++i)
		Near(open.Path.PointRatio(0, i), 32 * (.8 - .6 * double(i) / 3), 0, 2);
	graph.Set("offset", -123.);
	const auto same = std::get<Path2D>(graph.Run());
	for (size_t i = 0; i < 4; ++i)
		CHECK(Data(same).Lines[i].Anchors == Data(initialOpen).Lines[i].Anchors);
	for (double numeric : {.5, .5001, -1.}) {
		Graph threshold;
		threshold.Doc.Junctions = {{"loop_control", "", ValueType::Scalar, numeric}};
		threshold.Doc.Links.push_back({"loop_control", "value", "bridge", "loop"});
		Runtime result(std::get<Path2D>(threshold.Run()));
		Near(result.Path.PointRatio(0, 1), numeric > .5 ? 32. / 3 : 16, 0, 2);
	}
}

TEST_CASE(
	"Bridge flattens primary child lines before each dynamic input child line", "[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.Set("amount", int64_t{1});
	graph.Set("path", Combined({Line({0, 0}, {32, 0}, 2), Line({100, 10}, {132, 10}, 4)}));
	graph.Set("path_0", Combined({Line({-50, 20}, {-18, 20}, 8), Line({200, 30}, {232, 30}, 16)}));
	graph.Doc.Nodes[0].DynamicInputs.push_back(
		{"path_1", ValueType::Path2D, Value{Line({300, 40}, {332, 40}, 32)}}
	);
	const auto path = std::get<Path2D>(graph.Run());
	const auto &line = Data(path).Lines[0];
	REQUIRE(line.Anchors.size() == 5);
	const std::array<Vector3, 5> expected{
		{{16, 0, 2}, {116, 10, 4}, {-34, 20, 8}, {216, 30, 16}, {316, 40, 32}}
	};
	for (size_t i = 0; i < expected.size(); ++i)
		CHECK(line.Anchors[i] == expected[i]);
	Runtime runtime(path);
	CHECK(runtime.Path.SegmentCount() == 5);
	CHECK(runtime.Path.AccumulatedCount() == 4);
	CHECK(
		runtime.Path.Length() ==
		Catch::Approx(
			std::hypot(100., 10.) + std::hypot(150., 10.) + std::hypot(250., 10.) + std::hypot(100., 10.)
		)
	);
}

TEST_CASE(
	"Bridge two source paths overwrite both smooth controls with the final endpoint",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.Set("amount", int64_t{1});
	graph.Set("smooth", true);
	const auto path = std::get<Path2D>(graph.Run());
	const auto &line = Data(path).Lines[0];
	REQUIRE(line.Controls.size() == 1);
	CHECK(line.Controls[0] == std::array<double, 4>{48, 12, 48, 12});
	Runtime runtime(path);
	const auto expected = std::hypot(32., 12.) * (1 - 1. / 32768);
	CHECK(runtime.Path.Length() == Catch::Approx(expected));
	CHECK(runtime.Path.Length() == Catch::Approx(SmoothLength(line)));
	CHECK(runtime.Path.SegmentCount() == 2);
	Near(runtime.Path.PointRatio(.5), 44, 10.5, 4);
	Near(runtime.Path.PointRatio(1), 48, 12, 6);
	graph.Sample(.5);
	Near(graph.Run("point"), 44, 10.5);
	for (double numeric : {.5, .5001, -1.}) {
		Graph threshold;
		threshold.Doc.Junctions = {{"smooth_control", "", ValueType::Scalar, numeric}};
		threshold.Doc.Links.push_back({"smooth_control", "value", "bridge", "smooth"});
		const auto thresholdPath = std::get<Path2D>(threshold.Run());
		CHECK(Data(thresholdPath).Smooth == (numeric > .5));
		Runtime result(thresholdPath);
		Near(result.Path.PointRatio(.5), numeric > .5 ? 28 : 16, numeric > .5 ? 10.5 : 6, 4);
	}
}

TEST_CASE(
	"Bridge smooth interior handles halve neighbor distance and preserve sampled metrics",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.Set("amount", int64_t{1});
	graph.Set("smooth", true);
	graph.Set("path_0", Line({6, 0}, {38, 0}, 4));
	graph.Doc.Nodes[0].DynamicInputs.push_back(
		{"path_1", ValueType::Path2D, Value{Line({6, 8}, {38, 8}, 8)}}
	);
	const auto path = std::get<Path2D>(graph.Run());
	const auto &line = Data(path).Lines[0];
	REQUIRE(line.Anchors.size() == 3);
	REQUIRE(line.Controls.size() == 2);
	const std::array<double, 4> first{16, 0, 20.2, -2.4}, second{24.4, 3.2, 22, 8};
	for (size_t i = 0; i < 4; ++i) {
		CHECK(line.Controls[0][i] == Catch::Approx(first[i]));
		CHECK(line.Controls[1][i] == Catch::Approx(second[i]));
	}
	Runtime runtime(path);
	CHECK(runtime.Path.Length() == Catch::Approx(SmoothLength(line)).margin(1e-8));
	CHECK(runtime.Path.AccumulatedCount() == 2);
	CHECK(runtime.Path.SegmentCount() == 3);
}

TEST_CASE(
	"Bridge Random uses exactly the captured draw per output line and refuses stale observations",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.Set("distribution", EnumValue{1});
	graph.CompileGraph();
	SourceBuiltinRandomCapture capture;
	EvaluationRequest request;
	request.Tick = 3;
	request.Subframe = .25;
	const auto prepared =
		PrepareSourceBuiltinRandomCapture(graph.Doc, graph.Compiled, "bridge", request, capture, graph.Error);
	INFO(graph.Error.Message);
	REQUIRE(prepared == Status::Ok);
	for (double draw : {.2, .8, .3})
		capture.Draws.push_back({SourceBuiltinRandomOperation::Random, 0, 1, draw});
	request.BuiltinRandomCaptures = {&capture, 1};
	const auto path = std::get<Path2D>(graph.Run("path", request));
	Runtime runtime(path);
	for (size_t i = 0; i < 3; ++i)
		Near(runtime.Path.PointRatio(0, i), 32 * (i == 0 ? .2 : i == 1 ? .8 : .3), 0, 2);
	request.Tick = 4;
	graph.Refused(Status::UnsupportedExecution, request);
	request.Tick = 3;
	capture.Draws.pop_back();
	graph.Refused(Status::InvalidValue, request);
	Graph missing;
	missing.Set("distribution", EnumValue{1});
	missing.Refused(Status::UnsupportedExecution);
}

TEST_CASE(
	"Bridge distance extrapolates negative positions and invalid line sampling clears only XY",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.Set("amount", int64_t{1});
	Runtime runtime(std::get<Path2D>(graph.Run()));
	const auto length = runtime.Path.Length();
	Near(runtime.Path.PointDistance(-length / 2), 0, -6, 0);
	Near(runtime.Path.PointRatio(-1), 16, 0, 2);
	Near(runtime.Path.PointRatio(2), 48, 12, 6);
	SourcePathPointBuffer out{SourcePathPointClass::Spatial, {71, 83}, 17., 9};
	runtime.Path.PointDistanceInto(5, 7, out);
	CHECK(out.Position == Vector2{0, 0});
	CHECK(out.Weight == 9);
	CHECK(out.Z == 17);
	CHECK(out.Class == SourcePathPointClass::Spatial);
}

TEST_CASE(
	"Bridge refuses source undefined smoothing and bounded count or array controls atomically",
	"[imagegraph][path_bridge]"
) {
	Graph smooth;
	smooth.Doc.Nodes[0].DynamicInputs.clear();
	smooth.Set("smooth", true);
	smooth.Refused(Status::UnsupportedExecution);
	Graph negative;
	negative.Set("amount", int64_t{-1});
	negative.Refused(Status::LimitExceeded);
	Graph excessive;
	excessive.Set("amount", int64_t{Limits::MaximumArrayElements + 1});
	excessive.Refused(Status::LimitExceeded);
	Graph aggregate;
	aggregate.Set("amount", int64_t{2048});
	aggregate.Refused(Status::LimitExceeded);
	Graph overflow;
	overflow.Set("range", Vector2{-std::numeric_limits<double>::max(), std::numeric_limits<double>::max()});
	overflow.Refused(Status::InvalidValue);
	Graph arrays;
	arrays.Doc.Junctions = {
		{"amounts", "", ValueType::Any, ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{2}}}}
	};
	arrays.Doc.Links.push_back({"amounts", "value", "bridge", "amount"});
	arrays.Refused(Status::UnsupportedExecution);
	Graph invalid;
	invalid.Set("offset", std::numeric_limits<double>::infinity());
	Plan plan;
	Diagnostic error;
	CHECK(Compile(invalid.Doc, plan, error) == Status::InvalidValue);
}

TEST_CASE(
	"Bridge produced geometry and weights survive native scalar flat and nested carriers",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.Set("smooth", true);
	const auto path = std::get<Path2D>(graph.Run());
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"scalar", "pc.argument", "", {}, {{"default_value", path}}},
		{"flat", "pc.argument", "", {}, {{"default_value", ArrayValue{ValueType::Path2D, {path, path}}}}}
	};
	ArrayValue nested;
	nested.ElementType = ValueType::Path2D;
	nested.Nested = {{path}, {path, path}};
	document.Nodes.push_back({"nested", "pc.argument", "", {}, {{"default_value", nested}}});
	Document restored;
	Diagnostic error;
	const auto decoded = Read(Write(document), restored, error);
	INFO(error.Message);
	REQUIRE(decoded == Status::Ok);
	CHECK(restored == document);
	Runtime runtime(std::get<Path2D>(restored.Nodes[0].Values[0].Data));
	CHECK(runtime.Path.Length() == Catch::Approx(std::hypot(32., 12.) * (1 - 1. / 32768)));
	Near(runtime.Path.PointRatio(.5), 28, 10.5, 4);
}

TEST_CASE(
	"Bridge byte refusal preserves state and published output before a complete retry",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.CompileGraph();
	StatefulEvaluationResult state;
	EvaluationRequest request;
	request.DataReplay = &state.Data;
	REQUIRE(EvaluateStateful(graph.Doc, graph.Compiled, "path", request, state, graph.Error) == Status::Ok);
	const auto previous = state.Data;
	const auto output = std::get<EvaluatedValue>(state.Output);
	request.Tick = 1;
	graph.Set("amount", int64_t{8});
	graph.CompileGraph();
	CHECK(
		EvaluateStateful(graph.Doc, graph.Compiled, "path", request, state, graph.Error, 1) ==
		Status::LimitExceeded
	);
	CHECK(state.Data == previous);
	CHECK(std::get<EvaluatedValue>(state.Output) == output);
	REQUIRE(EvaluateStateful(graph.Doc, graph.Compiled, "path", request, state, graph.Error) == Status::Ok);
	CHECK(Data(std::get<Path2D>(std::get<EvaluatedValue>(state.Output).Data)).LineCount == 8);
}

TEST_CASE(
	"Bridge missing primary input retains old geometry while current amount changes declared lines",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.CompileGraph();
	StatefulEvaluationResult result;
	EvaluationRequest request;
	request.DataReplay = &result.Data;
	REQUIRE(EvaluateStateful(graph.Doc, graph.Compiled, "path", request, result, graph.Error) == Status::Ok);
	const auto previous = std::get<Path2D>(std::get<EvaluatedValue>(result.Output).Data);
	std::erase_if(graph.Doc.Nodes[0].Values, [](const auto &input) { return input.Port == "path"; });
	graph.Set("amount", int64_t{5});
	graph.CompileGraph();
	request.Tick = 1;
	REQUIRE(EvaluateStateful(graph.Doc, graph.Compiled, "path", request, result, graph.Error) == Status::Ok);
	const auto retained = std::get<Path2D>(std::get<EvaluatedValue>(result.Output).Data);
	CHECK(Data(retained).LineCount == 5);
	CHECK(Data(retained).Lines == Data(previous).Lines);
	Graph cold;
	std::erase_if(cold.Doc.Nodes[0].Values, [](const auto &input) { return input.Port == "path"; });
	const auto absent = std::get<Path2D>(cold.Run());
	CHECK(Data(absent).LineCount == 3);
	CHECK(Data(absent).Lines.empty());
	Runtime runtime(absent);
	CHECK_FALSE(runtime.Path.HasBoundary);
	CHECK_FALSE(std::isfinite(runtime.Path.Length()));
	CHECK(runtime.Context.FailureCode == Status::InvalidValue);
}

TEST_CASE(
	"Bridge future subframe replay is refused when a real timestamp loses tiny fractions",
	"[imagegraph][path_bridge]"
) {
	Graph graph;
	graph.CompileGraph();
	StatefulEvaluationResult result;
	EvaluationRequest request;
	REQUIRE(EvaluateStateful(graph.Doc, graph.Compiled, "path", request, result, graph.Error) == Status::Ok);
	REQUIRE(result.Data.Entries.size() == 1);
	constexpr uint64_t TICK = 32;
	constexpr double EARLIER = 1e-20, LATER = 2e-20;
	REQUIRE(FrameTimeToReal({TICK, EARLIER, false}) == FrameTimeToReal({TICK, LATER, false}));
	auto receipt = result.Data;
	receipt.Entries[0].Tick = TICK;
	receipt.Entries[0].Subframe = LATER;
	receipt.Entries[0].Values[0].Frame = TICK;
	request.Tick = TICK;
	request.Subframe = EARLIER;
	request.DataReplay = &receipt;
	const auto before = receipt;
	graph.Refused(Status::InvalidValue, request);
	CHECK(graph.Error.NodeId == "bridge");
	CHECK(graph.Error.Port == "path");
	CHECK(receipt == before);
}
