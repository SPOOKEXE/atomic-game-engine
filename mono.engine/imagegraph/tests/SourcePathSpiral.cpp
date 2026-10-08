#include "nodes/Path.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/StatefulTemporalCone.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

TEST_SUITE_ID("engine.imagegraph.source_path_spiral")
using namespace engine::imagegraph;
namespace source_spiral_test {
	Path2D Line(double length = 10, double y = 0) {
		Path2D path;
		path.Anchors = {{{0, y, 0, 0, 0, 0}, 0}, {{length, y, 0, 0, 0, 0}, 0}};
		path.Weights = {{0, 2}, {100, 2}};
		return path;
	}
	Curve Constant(double value) {
		Curve curve;
		curve.Header = {0, 1, 0, 0, 1, 0};
		curve.Anchors = {{0, 0, 0, value, 0, 0}, {0, 0, 1, value, 0, 0}};
		return curve;
	}
	struct Graph {
		Document Doc;
		Plan Compiled;
		Diagnostic Error;
		Graph() {
			Doc.FormatVersion = 9;
			Doc.Timeline = TimelineSettings{};
			Doc.Timeline->Frames = 33;
			Doc.Nodes = {
				{"spiral",
				 "pc.path_spiral",
				 "",
				 {},
				 {{"path", Line()},
				  {"frequency", 1.},
				  {"amplitude", 2.},
				  {"spiral", .5},
				  {"phase", 0.},
				  {"direction", EnumValue{1}},
				  {"angle", Vector2{90, 90}},
				  {"amplitude_curve", Constant(1)}}}
			};
			Doc.Outputs = {{"path", "spiral", "path"}};
		}
		void Set(std::string_view port, Value value) {
			for (auto &input : Doc.Nodes[0].Values)
				if (input.Port == port) {
					input.Data = std::move(value);
					return;
				}
			Doc.Nodes[0].Values.push_back({std::string(port), std::move(value)});
		}
		Path2D Run() {
			REQUIRE(Compile(Doc, Compiled, Error) == Status::Ok);
			EvaluatedValue value;
			auto status = EvaluateValue(Doc, Compiled, "path", {}, value, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			return std::get<Path2D>(value.Data);
		}
	};
	struct Runtime {
		Node NodeValue{"sample", "pc.path_sample", "", {}, {}};
		EvaluationRequest Request;
		detail::NodeContext Context{NodeValue, *FindCatalogueEntry("pc.path_sample"), Request};
		detail::SourcePathShiftMemo Memo;
		detail::PathRuntime Path;
		explicit Runtime(const Path2D &path) {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
			Context.PathShiftMemo = &Memo;
			Context.Values = {{"path", path}};
			REQUIRE(detail::StampSourcePathShiftInputs(Context));
			REQUIRE(Path.Init(Context, std::get<Path2D>(Context.Values[0].second)));
		}
	};
	void Near(const detail::PathPoint &point, double x, double y, double weight = 2) {
		CHECK(point.X == Catch::Approx(x).margin(1e-8));
		CHECK(point.Y == Catch::Approx(y).margin(1e-8));
		CHECK(point.Weight == Catch::Approx(weight).margin(1e-8));
	}
}
using namespace source_spiral_test;
TEST_CASE("Spiral fixed and path-normal offsets use independent analytic axes", "[imagegraph][path_spiral]") {
	Graph graph;
	auto value = graph.Run();
	Runtime fixed(value);
	Near(fixed.Path.PointRatio(0), 0, -1);
	Near(fixed.Path.PointRatio(.25), .5, 0);
	Near(fixed.Path.PointRatio(.5), 5, 1);
	graph.Set("direction", EnumValue{0});
	value = graph.Run();
	Runtime normal(value);
	Near(normal.Path.PointRatio(0), 1, 0);
	Near(normal.Path.PointRatio(.25), 2.5, -2);
	Near(normal.Path.PointRatio(.5), 4, 0);
	Document stored;
	stored.FormatVersion = 9;
	stored.Nodes = {{"sample", "pc.path_sample", "", {}, {{"path", value}, {"ratio", .25}}}};
	stored.Outputs = {{"point", "sample", "position"}};
	Document restored;
	Diagnostic error;
	REQUIRE(Read(Write(stored), restored, error) == Status::Ok);
	CHECK(restored == stored);
	Plan plan;
	REQUIRE(Compile(restored, plan, error) == Status::Ok);
	EvaluatedValue output;
	REQUIRE(EvaluateValue(restored, plan, "point", {}, output, error) == Status::Ok);
	CHECK(std::get<Vector2>(output.Data) == Vector2{2.5, -2});
	auto clone = value;
	REQUIRE(clone.SourceOperation);
	clone.SourceOperation->Inputs[0].Anchors[0].Controls[0] = 99;
	CHECK(value.SourceOperation->Inputs[0].Anchors[0].Controls[0] == 0);
}
TEST_CASE("Spiral range is inclusive and open endpoint clamps before phase", "[imagegraph][path_spiral]") {
	Graph graph;
	graph.Set("range", Vector2{.25, .75});
	Runtime restricted(graph.Run());
	Near(restricted.Path.PointRatio(.2), 2, 0);
	Near(restricted.Path.PointRatio(.25), .5, 0);
	Near(restricted.Path.PointRatio(.8), 8, 0);
	graph.Set("range", Vector2{-2, 2});
	graph.Set("amplitude", 0.);
	Runtime open(graph.Run());
	Near(open.Path.PointRatio(1), 9.9, 0);
	Near(open.Path.PointRatio(-.1), 0, 0);
	graph.Set("loop", true);
	Runtime loop(graph.Run());
	Near(loop.Path.PointRatio(1), 9.999, 0);
	Near(loop.Path.PointRatio(-.1), 9.0001, 0);
}
TEST_CASE(
	"Spiral weighting and constant amplitude and direction curves retain source controls",
	"[imagegraph][path_spiral]"
) {
	for (int64_t mode = 0; mode < 3; ++mode) {
		Graph graph;
		graph.Set("use_weight", true);
		graph.Set("weight_mode", EnumValue{mode});
		graph.Set("range_2", Vector2{3, 5});
		Runtime runtime(graph.Run());
		Near(runtime.Path.PointRatio(0), 0, -1, mode == 0 ? 5 : mode == 1 ? 7 : 10);
		Near(runtime.Path.PointRatio(.5), 5, 1, mode == 0 ? 3 : mode == 1 ? 5 : 6);
	}
	Graph graph;
	graph.Set("amplitude_curve", Constant(.5));
	graph.Set("amplitude_curved", false);
	Runtime amplitude(graph.Run());
	Near(amplitude.Path.PointRatio(.25), 1.5, 0);
	graph.Set("angle", Vector2{0, 90});
	graph.Set("angle_curved", true);
	graph.Set("angle_curve", Constant(0));
	Runtime direction(graph.Run());
	Near(direction.Path.PointRatio(0), .5, 0);
}
TEST_CASE("Spiral evaluation refuses byte admission atomically", "[imagegraph][path_spiral]") {
	Graph graph;
	REQUIRE(Compile(graph.Doc, graph.Compiled, graph.Error) == Status::Ok);
	const auto before = graph.Compiled;
	CHECK(Compile(graph.Doc, graph.Compiled, graph.Error, 1) == Status::LimitExceeded);
	CHECK(graph.Compiled == before);
}

TEST_CASE(
	"Spiral metadata forwards child bounds and uses base amplitude for length", "[imagegraph][path_spiral]"
) {
	Graph graph;
	auto value = graph.Run();
	REQUIRE(value.SourceOperation);
	REQUIRE(value.SourceOperation->Spiral);
	CHECK(value.SourceOperation->Spiral->AmplitudeCurve.size() == 129);
	Runtime runtime(value);
	CHECK(runtime.Path.Length() == Catch::Approx(10 * std::sqrt(3.)));
	CHECK(
		runtime.Path.AccumulatedAt(runtime.Path.AccumulatedCount() - 1) == Catch::Approx(10 * std::sqrt(3.))
	);
	CHECK(runtime.Path.LineCount() == 1);
	CHECK(runtime.Path.SegmentCount() == 1);
	CHECK(runtime.Path.MinX == 0);
	CHECK(runtime.Path.MaxX == 10);
	CHECK(runtime.Path.MinY == 0);
	CHECK(runtime.Path.MaxY == 0);
	graph.Set("angle_curved", true);
	graph.Set("angle_curve", Constant(.5));
	auto curved = graph.Run();
	REQUIRE(curved.SourceOperation->Spiral);
	CHECK(curved.SourceOperation->Spiral->DirectionCurve.size() == 33);
	graph.Set("amplitude_curve", Constant(0));
	Runtime zero(graph.Run());
	CHECK(zero.Path.Length() == Catch::Approx(10 * std::sqrt(3.)));
	Near(zero.Path.PointRatio(.25), 2.5, 0);
}

TEST_CASE("Spiral six decimal memo retains the first raw ratio sample", "[imagegraph][path_spiral]") {
	Graph graph;
	graph.Set("amplitude", 0.);
	auto value = graph.Run();
	Runtime first(value);
	const auto a = first.Path.PointRatio(.2500001);
	const auto collision = first.Path.PointRatio(.2500004);
	CHECK(a.X == Catch::Approx(2.500001).margin(1e-10));
	CHECK(collision.X == a.X);
	Runtime reversed(value);
	const auto b = reversed.Path.PointRatio(.2500004);
	CHECK(b.X == Catch::Approx(2.500004).margin(1e-10));
	CHECK(reversed.Path.PointRatio(.2500001).X == b.X);
	Runtime refused(value);
	refused.Context.ByteBudget = 0;
	(void)refused.Path.PointRatio(.4);
	CHECK(refused.Context.FailureCode == Status::LimitExceeded);
	CHECK(refused.Memo.Entries.empty());
}

TEST_CASE(
	"Spiral amplitude range clamp and equal range use pre-normalization curve progress",
	"[imagegraph][path_spiral]"
) {
	Graph graph;
	Curve ramp;
	ramp.Header = {0, 1, 0, 0, 1, 0};
	ramp.Anchors = {{0, 0, 0, 0, 1. / 3, 1. / 3}, {-1. / 3, -1. / 3, 1, 1, 0, 0}};
	graph.Set("amplitude_curve", ramp);
	graph.Set("range", Vector2{.25, .75});
	graph.Set("clamp_curve", true);
	Runtime clamped(graph.Run());
	Near(clamped.Path.PointRatio(.25), 2.5, 0);
	graph.Set("clamp_curve", false);
	Runtime raw(graph.Run());
	Near(raw.Path.PointRatio(.25), 2, 0);
	graph.Set("range", Vector2{.25, .25});
	graph.Set("clamp_curve", true);
	Runtime equal(graph.Run());
	Near(equal.Path.PointRatio(.25), 2.5, 0);
}
TEST_CASE("Spiral multiline distance denominator remains source line zero", "[imagegraph][path_spiral]") {
	Path2D multi;
	multi.SourceOperation.emplace();
	multi.SourceOperation->Kind = SourcePathOperationKind::Combine;
	multi.SourceOperation->Inputs = {Line(), Line(20, 5)};
	Graph graph;
	graph.Set("path", multi);
	graph.Set("amplitude", 0.);
	Runtime runtime(graph.Run());
	CHECK(runtime.Path.LineCount() == 2);
	CHECK(runtime.Path.Length(0) == 10);
	CHECK(runtime.Path.Length(1) == 20);
	CHECK(runtime.Path.SegmentCount(1) == 1);
	Near(runtime.Path.PointDistance(5, 1), 10, 5);
	Near(runtime.Path.PointRatio(.25, 1), 5, 5);
}

TEST_CASE("Spiral shared memo admission bounds aggregate lookup work", "[imagegraph][path_spiral]") {
	Graph graph;
	Runtime runtime(graph.Run());
	runtime.Memo.LookupWork = detail::SourcePathShiftMemo::MAXIMUM_LOOKUP_WORK;
	const auto count = runtime.Memo.Entries.size();
	(void)runtime.Path.PointRatio(.333);
	CHECK(runtime.Context.FailureCode == Status::LimitExceeded);
	CHECK(runtime.Memo.Entries.size() == count);
}

TEST_CASE("Spiral participates in first frame data temporal cones", "[imagegraph][path_spiral]") {
	Graph graph;
	(void)graph.Run();
	const std::array<std::string, 1> outputs{"path"};
	const auto cone = AnalyzeStatefulTemporalCone(graph.Doc, graph.Compiled, outputs);
	REQUIRE(cone.Valid);
	CHECK(cone.DataProcessors == 1);
	CHECK(cone.FirstFrameData);
}
TEST_CASE("Spiral source noone differs from an explicitly authored empty path", "[imagegraph][path_spiral]") {
	Graph absent;
	auto &values = absent.Doc.Nodes[0].Values;
	values.erase(
		std::remove_if(values.begin(), values.end(), [](const auto &value) { return value.Port == "path"; }),
		values.end()
	);
	auto missing = absent.Run();
	Runtime missingRuntime(missing);
	Near(missingRuntime.Path.PointRatio(0), 0, 0, 1);
	SourcePathPointBuffer supplied;
	supplied.Position = {77, 88};
	supplied.Z = 99;
	supplied.Weight = 7;
	(void)missingRuntime.Path.PointRatioInto(.5, 0, supplied);
	CHECK(supplied.Position.X == 0);
	CHECK(supplied.Position.Y == 0);
	CHECK(supplied.Weight == 7);
	CHECK(supplied.Z == 99);
	Graph explicitEmpty;
	explicitEmpty.Set("path", Path2D{});
	Runtime emptyRuntime(explicitEmpty.Run());
	Near(emptyRuntime.Path.PointRatio(0), 0, -1, 1);
}

TEST_CASE(
	"Spiral owned value identity includes controls tables buffers and cache", "[imagegraph][path_spiral]"
) {
	Graph graph;
	const auto original = graph.Run();
	REQUIRE(original.SourceOperation);
	REQUIRE(original.SourceOperation->Spiral);
	SECTION("Controls") {
		auto changed = original;
		changed.SourceOperation->Spiral->Frequency += 1;
		CHECK(changed != original);
		CHECK(original.SourceOperation->Spiral->Frequency == 1);
	}
	SECTION("Amplitude table") {
		auto changed = original;
		REQUIRE_FALSE(changed.SourceOperation->Spiral->AmplitudeCurve.empty());
		changed.SourceOperation->Spiral->AmplitudeCurve[0] += .5;
		CHECK(changed != original);
		CHECK(original.SourceOperation->Spiral->AmplitudeCurve[0] == 1);
	}
	SECTION("Direction table") {
		auto changed = original;
		changed.SourceOperation->Spiral->DirectionCurve.assign(33, .5);
		CHECK(changed != original);
		CHECK(original.SourceOperation->Spiral->DirectionCurve.empty());
	}
	SECTION("Reusable buffers") {
		auto changed = original;
		changed.SourceOperation->Spiral->Buffers[1].Weight += 7;
		CHECK(changed != original);
		CHECK(original.SourceOperation->Spiral->Buffers[1].Weight == 1);
	}
	SECTION("Sample cache") {
		auto changed = original;
		changed.SourceOperation->Spiral->Cache.emplace_back();
		CHECK(changed != original);
		CHECK(original.SourceOperation->Spiral->Cache.empty());
	}
	SECTION("Evaluation memo handles are not durable identity") {
		auto changed = original;
		changed.SourceOperation->EvaluationMemoId += 17;
		CHECK(changed == original);
	}
}
