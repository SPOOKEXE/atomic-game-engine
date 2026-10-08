#include "../src/ProcessorBatch.hpp"
#include "../src/SourcePathWaveRandom.hpp"
#include "nodes/Path.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/StatefulTemporalCone.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

TEST_SUITE_ID("engine.imagegraph.source_path_wave")
using namespace engine::imagegraph;
namespace source_wave_test {
	Path2D Line(double length = 10, double y = 0) {
		Path2D path;
		path.Anchors = {{{0, y, 0, 0, 0, 0}, 0}, {{length, y, 0, 0, 0, 0}, 0}};
		path.Weights = {{0, 3}, {100, 3}};
		return path;
	}
	Curve Constant(double value) {
		Curve curve;
		curve.Header = {0, 1, 0, 0, 1, 0};
		curve.Anchors = {{0, 0, 0, value, 0, 0}, {0, 0, 1, value, 0, 0}};
		return curve;
	}
	Curve Ramp() {
		Curve curve;
		curve.Header = {0, 1, 0, 0, 1, 0};
		curve.Anchors = {{0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 0, 0}};
		return curve;
	}
	struct Graph {
		Document Doc;
		Plan Compiled;
		Diagnostic Error;
		Graph() {
			Doc.FormatVersion = 9;
			Doc.Timeline = TimelineSettings{33, 0, 32, "loop", 33};
			Doc.Nodes = {
				{"wave",
				 "pc.path_wave",
				 "",
				 {},
				 {{"path", Line()},
				  {"seed", 0.},
				  {"frequency", Vector2{1, 1}},
				  {"amplitude", Vector2{2, 2}},
				  {"phase", Vector2{0, 0}},
				  {"direction", EnumValue{1}},
				  {"angle", Vector2{0, 0}},
				  {"amplitude_curve", Constant(1)}}},
				{"sample", "pc.path_sample", "", {}, {{"ratio", .25}}}
			};
			Doc.Links = {{"wave", "path", "sample", "path"}};
			Doc.Outputs = {
				{"path", "wave", "path"}, {"point", "sample", "position"}, {"weight", "sample", "weight"}
			};
		}
		void Set(std::string_view port, Value value) {
			for (auto &input : Doc.Nodes[0].Values)
				if (input.Port == port) {
					input.Data = std::move(value);
					return;
				}
			Doc.Nodes[0].Values.push_back({std::string(port), std::move(value)});
		}
		void Sample(double ratio, int64_t line = 0) {
			Doc.Nodes[1].Values = {{"ratio", ratio}, {"path_index", line}, {"type", EnumValue{2}}};
		}
		Value Run(std::string output = "path") {
			const auto compiled = Compile(Doc, Compiled, Error);
			INFO(Error.Message << " " << Error.NodeId << ":" << Error.Port);
			REQUIRE(compiled == Status::Ok);
			EvaluatedValue value;
			const auto status = EvaluateValue(Doc, Compiled, output, {}, value, Error);
			INFO(Error.Message << " " << Error.NodeId << ":" << Error.Port);
			REQUIRE(status == Status::Ok);
			return value.Data;
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
	void Near(const detail::PathPoint &point, double x, double y, double weight = 3) {
		CHECK(point.X == Catch::Approx(x).margin(1e-8));
		CHECK(point.Y == Catch::Approx(y).margin(1e-8));
		CHECK(point.Weight == Catch::Approx(weight).margin(1e-8));
	}
	void Near(const Value &value, double x, double y) {
		const auto point = std::get<Vector2>(value);
		CHECK(point.X == Catch::Approx(x).margin(1e-8));
		CHECK(point.Y == Catch::Approx(y).margin(1e-8));
	}
	double Progress(int mode, double time) {
		const double fraction = time - std::trunc(time);
		return mode == 0   ? (std::abs(fraction * 2 - 1) - .5) * 2
			   : mode == 1 ? std::cos(time * std::numbers::pi * 2)
						   : (fraction > .5 ? 1 : -1);
	}
}
using namespace source_wave_test;

TEST_CASE(
	"Wave samples all three modes and post functions in fixed and normal directions",
	"[imagegraph][path_wave]"
) {
	for (int mode = 0; mode < 3; ++mode)
		for (int post = 0; post < 3; ++post)
			for (int direction = 0; direction < 2; ++direction) {
				Graph graph;
				graph.Set("mode", EnumValue{mode});
				graph.Set("post_fn", EnumValue{post});
				graph.Set("direction", EnumValue{direction});
				graph.Set("angle", Vector2{90, 90});
				Runtime runtime(std::get<Path2D>(graph.Run()));
				for (double ratio : {0., .125, .25, .5, .75, .99}) {
					double wave = Progress(mode, ratio);
					if (post == 1) wave = std::abs(wave);
					if (post == 2) wave = std::max(0., wave);
					Near(runtime.Path.PointRatio(ratio), 10 * ratio, -2 * wave);
				}
			}
	Graph graph;
	graph.Set("mode", EnumValue{2});
	graph.Sample(.25);
	Near(graph.Run("point"), .5, 0);
	CHECK(std::get<double>(graph.Run("weight")) == 3);
}

TEST_CASE(
	"Wave inclusive ranges bypass the child outside and preserve loop and signed frac quirks",
	"[imagegraph][path_wave]"
) {
	Graph graph;
	graph.Set("range", Vector2{.25, .75});
	Runtime range(std::get<Path2D>(graph.Run()));
	Near(range.Path.PointRatio(.125), 1.25, 0);
	Near(range.Path.PointRatio(.25), 2.5, 0);
	Near(range.Path.PointRatio(.75), 7.5, 0);
	Near(range.Path.PointRatio(1), 9.999, 0);
	graph.Set("range", Vector2{-1, 2});
	Runtime open(std::get<Path2D>(graph.Run()));
	Near(open.Path.PointRatio(1), 9.9 + 2 * Progress(0, .99), 0);
	graph.Set("loop", true);
	Runtime loop(std::get<Path2D>(graph.Run()));
	Near(loop.Path.PointRatio(1), 11.999, 0);
	Near(loop.Path.PointRatio(1.25), 2.49875, 0);
	Near(loop.Path.PointRatio(-.25), 11.50025, 0);
	graph.Set("loop", false);
	graph.Set("phase", Vector2{-.25, -.25});
	Runtime negative(std::get<Path2D>(graph.Run()));
	Near(negative.Path.PointRatio(0), 4, 0);
}

TEST_CASE(
	"Wave metrics use range endpoints and distance divides by line zero length", "[imagegraph][path_wave]"
) {
	Path2D combined;
	auto &op = combined.SourceOperation.emplace();
	op.Kind = SourcePathOperationKind::Combine;
	op.Inputs = {Line(10), Line(20, 4)};
	Graph graph;
	graph.Set("path", combined);
	graph.Set("frequency", Vector2{-4, -2});
	graph.Set("amplitude", Vector2{-9, -3});
	Runtime runtime(std::get<Path2D>(graph.Run()));
	const double multiplier = 2 * std::sqrt(3.5);
	CHECK(runtime.Path.LineCount() == 2);
	for (size_t line = 0; line < 2; ++line) {
		CHECK(runtime.Path.Length(line) == Catch::Approx((line + 1) * 10 * multiplier));
		CHECK(runtime.Path.SegmentCount(line) == 1);
		CHECK(runtime.Path.AccumulatedCount(line) == 1);
		CHECK(runtime.Path.AccumulatedAt(0, line) == Catch::Approx((line + 1) * 10 * multiplier));
	}
	CHECK(runtime.Path.MinX == 0);
	CHECK(runtime.Path.MaxX == 10);
	graph.Set("frequency", Vector2{1, 1});
	graph.Set("amplitude", Vector2{0, 0});
	Runtime distance(std::get<Path2D>(graph.Run()));
	Near(distance.Path.PointDistance(5, 1), 10, 4);
	SourcePathPointBuffer output;
	distance.Path.PointDistanceInto(5, 1, output);
	CHECK(output.Position == Vector2{10, 4});
	CHECK(output.Weight == 3);
}

TEST_CASE("Wave weight uses progress without clamping in all three modes", "[imagegraph][path_wave]") {
	for (int64_t mode = 0; mode < 3; ++mode) {
		Graph graph;
		graph.Set("use_weight", true);
		graph.Set("weight_mode", EnumValue{mode});
		graph.Set("range_2", Vector2{2, 6});
		graph.Set("mode", EnumValue{2});
		Runtime runtime(std::get<Path2D>(graph.Run()));
		Near(runtime.Path.PointRatio(.25), .5, 0, mode == 0 ? -2 : mode == 1 ? 1 : -6);
		Near(runtime.Path.PointRatio(.75), 9.5, 0, mode == 0 ? 6 : mode == 1 ? 9 : 18);
	}
}

TEST_CASE(
	"Wave curves shift and clamp amplitude while only the angle flag controls direction mapping",
	"[imagegraph][path_wave]"
) {
	Graph graph;
	graph.Set("amplitude_curve", Constant(.5));
	graph.Set("amplitude_curved", false);
	Runtime amplitude(std::get<Path2D>(graph.Run()));
	Near(amplitude.Path.PointRatio(0), 1, 0);
	graph.Set("amplitude_curved", true);
	Runtime toggle(std::get<Path2D>(graph.Run()));
	Near(toggle.Path.PointRatio(0), 1, 0);
	graph.Set("amplitude_curve", Ramp());
	graph.Set("shift", .25);
	Runtime shifted(std::get<Path2D>(graph.Run()));
	Near(shifted.Path.PointRatio(0), .5, 0);
	graph.Set("range", Vector2{.25, .75});
	graph.Set("clamp_curve", true);
	graph.Set("mode", EnumValue{2});
	Runtime clamped(std::get<Path2D>(graph.Run()));
	Near(clamped.Path.PointRatio(.5), 5 - 2 * .75, 0);
	graph.Set("amplitude_curve", Constant(1));
	graph.Set("shift", 0.);
	graph.Set("angle", Vector2{0, 90});
	graph.Set("angle_curve", Constant(1));
	graph.Set("angle_curved", false);
	Runtime straight(std::get<Path2D>(graph.Run()));
	const double angle = .5 * std::numbers::pi / 2;
	Near(straight.Path.PointRatio(.5), 5 - 2 * std::cos(angle), 2 * std::sin(angle));
	graph.Set("angle_curved", true);
	Runtime curved(std::get<Path2D>(graph.Run()));
	Near(curved.Path.PointRatio(.5), 5, 2);
}

TEST_CASE(
	"Wave iteration draws continue from the final seeded range and wiggle stream", "[imagegraph][path_wave]"
) {
	Graph graph;
	graph.Set("mode", EnumValue{1});
	graph.Set("iteration", int64_t{3});
	graph.Set("freqency", 1.);
	graph.Set("amplitude_2", .5);
	graph.Set("shift_2", .1);
	// Values come from the independent pinned HTML5 JS oracle, after frequency seed 3's first draw.
	const std::array<double, 2> draws{.10294104977647822, .24684200726768096};
	const double firstPhase = .1 + draws[0], secondPhase = firstPhase + .1 + draws[1];
	const double progress =
		Progress(1, .25) + .5 * Progress(1, firstPhase + .5) + .25 * Progress(1, secondPhase + .75);
	Runtime iterations(std::get<Path2D>(graph.Run()));
	Near(iterations.Path.PointRatio(.25), 2.5 + 2 * progress, 0);
	graph.Set("mode", EnumValue{2});
	graph.Set("iteration", int64_t{2});
	graph.Set("shift_2", 0.);
	graph.Set("wiggle", true);
	graph.Set("wiggle_amplitude", Vector2{-2, 3});
	graph.Set("wiggle_frequency", 2.);
	Runtime wiggle(std::get<Path2D>(graph.Run()));
	const double amplitude = 2 + 1.832428678326508;
	Near(wiggle.Path.PointRatio(.25), 2.5 - 1.5 * amplitude, 0);
}

TEST_CASE(
	"Wave processor rows and doubled line seeds use independent source random fixtures",
	"[imagegraph][path_wave]"
) {
	Graph graph;
	graph.Set("amplitude", Vector2{1, 3});
	graph.Set("seed", ArrayValue{ValueType::Scalar, {0., 0.}});
	const auto rows = std::get<ArrayValue>(graph.Run());
	REQUIRE(rows.Elements.size() == 2);
	Runtime first(std::get<Path2D>(rows.Elements[0])), second(std::get<Path2D>(rows.Elements[1]));
	Near(first.Path.PointRatio(0), 2.2148661162773458, 0);
	Near(second.Path.PointRatio(0), 1.365984311497763, 0);
	Path2D combined;
	auto &op = combined.SourceOperation.emplace();
	op.Kind = SourcePathOperationKind::Combine;
	op.Inputs = {Line(), Line(10, 4)};
	graph.Set("seed", 0.);
	graph.Set("path", combined);
	Runtime lines(std::get<Path2D>(graph.Run()));
	Near(lines.Path.PointRatio(0, 1), 1.0109988851523952, 4);
	graph.Set("seed", -.25);
	Runtime negative(std::get<Path2D>(graph.Run()));
	Near(negative.Path.PointRatio(0), 1.1480123555045632, 0);
}

TEST_CASE(
	"Wave ratio memo owns rounded keys and refuses work without committing buffer state",
	"[imagegraph][path_wave]"
) {
	Graph graph;
	Runtime runtime(std::get<Path2D>(graph.Run()));
	SourcePathPointBuffer first;
	runtime.Path.PointRatioInto(.2500001, 0, first);
	const auto entries = runtime.Memo.Entries.size();
	SourcePathPointBuffer second;
	second.Weight = 8;
	runtime.Path.PointRatioInto(.2500004, 0, second);
	CHECK(second == first);
	CHECK(runtime.Memo.Entries.size() == entries);
	REQUIRE(!runtime.Memo.Owners.empty());
	const auto buffers = runtime.Memo.Owners[0].WaveBuffers;
	runtime.Memo.LookupWork = detail::SourcePathShiftMemo::MAXIMUM_LOOKUP_WORK;
	SourcePathPointBuffer refused;
	refused.Position = {99, 88};
	refused.Weight = 7;
	runtime.Path.PointRatioInto(.3, 0, refused);
	CHECK(runtime.Context.FailureCode == Status::LimitExceeded);
	CHECK(runtime.Memo.Entries.size() == entries);
	CHECK(runtime.Memo.Owners[0].WaveBuffers == buffers);
}

TEST_CASE(
	"Wave missing child resets position while retaining caller weight and point class",
	"[imagegraph][path_wave]"
) {
	Graph graph;
	graph.Doc.Nodes[0].Values.erase(graph.Doc.Nodes[0].Values.begin());
	Runtime runtime(std::get<Path2D>(graph.Run()));
	CHECK(runtime.Path.LineCount() == 1);
	CHECK(runtime.Path.Length() == 0);
	CHECK(runtime.Path.SegmentCount() == 0);
	SourcePathPointBuffer supplied;
	supplied.Class = SourcePathPointClass::Spatial;
	supplied.Position = {9, 12};
	supplied.Z = 42;
	supplied.Weight = 7;
	runtime.Path.PointRatioInto(.25, 0, supplied);
	CHECK(supplied.Position == Vector2{});
	CHECK(supplied.Class == SourcePathPointClass::Spatial);
	CHECK(supplied.Z == 42);
	CHECK(supplied.Weight == 7);
}

TEST_CASE(
	"Wave source random helper matches independent HTML5 signed frac and draw fixtures",
	"[imagegraph][path_wave]"
) {
	CHECK(detail::SourcePathWaveFrac(-.25) == -.25);
	detail::SourcePathWaveRandom random;
	REQUIRE(random.Seed(2));
	CHECK(random.Unit() == Catch::Approx(.005499442576197648));
	CHECK(random.Unit() == Catch::Approx(.6848448131628543));
	const auto range = random.SeededRange(-2, 3, -.25);
	REQUIRE(range);
	CHECK(*range == Catch::Approx(-1.6299691112385921));
	CHECK(random.Unit() == Catch::Approx(.9255384131919305));
	const auto positive = random.Wiggle(-2, 3, 2, .25, 0);
	REQUIRE(positive);
	CHECK(*positive == Catch::Approx(1.832428678326508));
	CHECK(random.Unit() == Catch::Approx(.5099332265136453));
	const auto negative = random.Wiggle(-2, 3, -2, .25, 0);
	REQUIRE(negative);
	CHECK(*negative == Catch::Approx(-1.0734813427894756));
	CHECK(random.Unit() == Catch::Approx(.8567445170398543));
}

TEST_CASE(
	"Wave stateful refresh clears sampled positions and preserves child getter buffers",
	"[imagegraph][path_wave]"
) {
	Graph graph;
	graph.Sample(0);
	REQUIRE(Compile(graph.Doc, graph.Compiled, graph.Error) == Status::Ok);
	const std::array<std::string, 1> selected{"weight"};
	const auto cone = AnalyzeStatefulTemporalCone(graph.Doc, graph.Compiled, selected);
	REQUIRE(cone.Valid);
	CHECK(cone.DataProcessors == 2);
	CHECK(cone.FirstFrameData);
	StatefulEvaluationResult state;
	EvaluationRequest request;
	request.DataReplay = &state.Data;
	REQUIRE(EvaluateStateful(graph.Doc, graph.Compiled, "weight", request, state, graph.Error) == Status::Ok);
	CHECK(std::get<double>(std::get<EvaluatedValue>(state.Output).Data) == 3);
	const auto before = state.Data;
	const auto output = std::get<EvaluatedValue>(state.Output);
	graph.Set("path", Path2D{});
	graph.Set("amplitude", Vector2{4, 4});
	REQUIRE(Compile(graph.Doc, graph.Compiled, graph.Error) == Status::Ok);
	request.Tick = 1;
	CHECK(
		EvaluateStateful(graph.Doc, graph.Compiled, "weight", request, state, graph.Error, 1) ==
		Status::LimitExceeded
	);
	CHECK(state.Data == before);
	CHECK(std::get<EvaluatedValue>(state.Output) == output);
	REQUIRE(EvaluateStateful(graph.Doc, graph.Compiled, "weight", request, state, graph.Error) == Status::Ok);
	CHECK(std::get<double>(std::get<EvaluatedValue>(state.Output).Data) == 3);
	REQUIRE(EvaluateStateful(graph.Doc, graph.Compiled, "point", request, state, graph.Error) == Status::Ok);
	Near(std::get<EvaluatedValue>(state.Output).Data, 4, 0);
	Graph fresh;
	fresh.Set("path", Path2D{});
	fresh.Set("amplitude", Vector2{4, 4});
	fresh.Sample(0);
	CHECK(std::get<double>(fresh.Run("weight")) == 1);
	REQUIRE(!state.Data.Entries.empty());
	const auto &retained = std::get<Path2D>(state.Data.Entries[0].Values[0].Data);
	REQUIRE(retained.SourceOperation);
	REQUIRE(retained.SourceOperation->Wave);
	CHECK(retained.SourceOperation->Wave->Buffers[0].Weight == 3);
	Document persisted;
	persisted.FormatVersion = 9;
	persisted.Nodes = {{"sample", "pc.path_sample", "", {}, {{"path", retained}, {"ratio", 0.}}}};
	persisted.Outputs = {{"point", "sample", "position"}};
	Document restored;
	REQUIRE(Read(Write(persisted), restored, graph.Error) == Status::Ok);
	CHECK(restored == persisted);
	const auto &saved = std::get<Path2D>(restored.Nodes[0].Values[0].Data);
	Runtime runtime(saved);
	Near(runtime.Path.PointRatio(0), 4, 0);
}

TEST_CASE(
	"Wave refuses unsupported counts missing seed and undefined samples atomically", "[imagegraph][path_wave]"
) {
	Graph graph;
	Status expected = Status::UnsupportedExecution;
	std::string output = "path";
	SECTION("missing source constructor seed") {
		std::erase_if(graph.Doc.Nodes[0].Values, [](const auto &value) { return value.Port == "seed"; });
	}
	SECTION("negative iterations") {
		graph.Set("iteration", int64_t{-1});
	}
	SECTION("iteration limit") {
		graph.Set("iteration", int64_t{Limits::MaximumArrayElements + 1});
		expected = Status::LimitExceeded;
	}
	SECTION("iteration arithmetic overflow") {
		graph.Set("iteration", int64_t{4});
		graph.Set("amplitude_2", std::numeric_limits<double>::max());
		expected = Status::InvalidValue;
	}
	SECTION("zero curve normalization span") {
		graph.Set("range", Vector2{.5, .5});
		graph.Set("clamp_curve", true);
		graph.Sample(.5);
		output = "point";
		expected = Status::InvalidValue;
	}
	REQUIRE(Compile(graph.Doc, graph.Compiled, graph.Error) == Status::Ok);
	EvaluatedValue retained;
	retained.Data = std::string{"retained"};
	const auto status = EvaluateValue(graph.Doc, graph.Compiled, output, {}, retained, graph.Error);
	INFO(graph.Error.Message << " " << graph.Error.NodeId << ":" << graph.Error.Port);
	CHECK(status == expected);
	CHECK(std::get<std::string>(retained.Data) == "retained");
}

TEST_CASE("Wave admits zero and maximum bounded iterations", "[imagegraph][path_wave]") {
	Graph graph;
	graph.Set("iteration", int64_t{0});
	Runtime zero(std::get<Path2D>(graph.Run()));
	Near(zero.Path.PointRatio(.25), 2.5, 0);
	graph.Set("iteration", int64_t{Limits::MaximumArrayElements});
	graph.Set("amplitude_2", 0.);
	graph.Set("freqency", 0.);
	graph.Set("shift_2", 0.);
	const auto maximum = std::get<Path2D>(graph.Run());
	REQUIRE(maximum.SourceOperation);
	REQUIRE(maximum.SourceOperation->Wave);
	CHECK(maximum.SourceOperation->Wave->Iteration == int64_t(Limits::MaximumArrayElements));
}

TEST_CASE("Wave refuses a missing spatial payload before output publication", "[imagegraph][path_wave]") {
	Graph graph;
	const auto &node = graph.Doc.Nodes[0];
	const auto *entry = FindCatalogueEntry(node.Type);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(entry);
	REQUIRE(executor);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	detail::SourcePathShiftMemo memo;
	context.PathShiftMemo = &memo;
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.InputProvenanceResolved = true;
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	for (const auto &input : node.Values) {
		std::erase_if(context.Values, [&](const auto &value) { return value.first == input.Port; });
		context.Values.emplace_back(input.Port, input.Data);
	}
	for (auto &[port, value] : context.Values)
		if (port == "path") value = PathValue3D{};
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::UnsupportedExecution);
	CHECK(context.FailurePort == "path");
	CHECK(context.OutputValues.empty());
	CHECK(context.DataUpdates.empty());
}

TEST_CASE(
	"Wave refuses future replay receipts within the same tick without changing caller history",
	"[imagegraph][path_wave]"
) {
	Graph graph;
	const auto wave = std::get<Path2D>(graph.Run());
	DataReplayState prior;
	DataReplayEntry entry;
	entry.NodeId = "wave";
	entry.Tick = 0;
	entry.Subframe = .8;
	entry.Initialized = true;
	entry.Values = {{0, wave}};
	prior.Entries.push_back(std::move(entry));
	const auto before = prior;
	EvaluationRequest request;
	request.Subframe = .2;
	request.DataReplay = &prior;
	SECTION("positive same tick") {}
	SECTION("negative source clock") {
		request.NegativeFrame = true;
		request.Tick = 1;
	}
	EvaluatedValue retained;
	retained.Data = std::string{"retained"};
	CHECK(
		EvaluateValue(graph.Doc, graph.Compiled, "path", request, retained, graph.Error) ==
		Status::InvalidValue
	);
	CHECK(std::get<std::string>(retained.Data) == "retained");
	CHECK(prior == before);
}
