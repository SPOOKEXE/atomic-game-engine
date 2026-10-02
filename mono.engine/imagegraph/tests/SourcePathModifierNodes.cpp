#include "nodes/Path.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_path_modifier")
using namespace engine::imagegraph;
namespace {
	Path2D ModifierLine(Vector2 a = {0, 0}, Vector2 b = {10, 10}) {
		Path2D p;
		p.Anchors = {{{a.X, a.Y, 0, 0, 0, 0}, 0}, {{b.X, b.Y, 0, 0, 0, 0}, 0}};
		p.Weights = {{0, 1}, {100, 1}};
		return p;
	}
	struct ModifierGraph {
		Document Doc;
		Plan Compiled;
		Diagnostic Error;
		ModifierGraph(std::string type, std::vector<AuthoredValue> controls = {}) {
			Doc.FormatVersion = 9;
			Doc.Nodes = {{"operation", type, "", {}, std::move(controls)}};
			Doc.Outputs = {{"value", "operation", type == "pc.path_to_curve" ? "curve" : "path"}};
		}
		void CompileNow() {
			const auto status = Compile(Doc, Compiled, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
		}
		Value Run() {
			EvaluatedValue out;
			const auto status = EvaluateValue(Doc, Compiled, "value", {}, out, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			return out.Data;
		}
		Vector2 Sample(double ratio = .5) {
			Doc.Nodes.push_back({"sample", "pc.path_sample", "", {}, {{"ratio", ratio}}});
			Doc.Links.push_back({"operation", "path", "sample", "path"});
			Doc.Outputs.push_back({"point", "sample", "position"});
			CompileNow();
			EvaluatedValue out;
			const auto status = EvaluateValue(Doc, Compiled, "point", {}, out, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			return std::get<Vector2>(out.Data);
		}
		void Roundtrip() {
			Document restored;
			REQUIRE(Read(Write(Doc), restored, Error) == Status::Ok);
			Doc = std::move(restored);
			CompileNow();
		}
	};
}
TEST_CASE(
	"Path to Curve source normalized anchors and six-field header", "[imagegraph][source_path_modifier]"
) {
	ModifierGraph g(
		"pc.path_to_curve",
		{{"path", ModifierLine({10, 20}, {30, 60})},
		 {"resolution", int64_t(2)},
		 {"shift", .25},
		 {"scale", 2.},
		 {"type", EnumValue{1}},
		 {"output_range", Vector2{-2, 3}},
		 {"y_range", Vector2{1, -1}}}
	);
	g.CompileNow();
	g.Roundtrip();
	const auto value = g.Run();
	const auto &curve = std::get<Curve>(value);
	REQUIRE(curve.Anchors.size() == 3);
	CHECK(curve.Header == std::array<double, 6>{.25, 2, 1, -2, 3, 0});
	CHECK(curve.Anchors[1] == std::array<double, 6>{0, 0, .5, 0, 0, 0});
	CHECK(curve.Anchors.back()[2] == Catch::Approx(.9999));
	CHECK(curve.Anchors.back()[3] == Catch::Approx(-.9998));
}
TEST_CASE("Path to Curve minimum-one bounds and loop endpoint", "[imagegraph][source_path_modifier]") {
	auto path = ModifierLine({5, 7}, {5.5, 7.25});
	path.Loop = true;
	ModifierGraph g("pc.path_to_curve", {{"path", path}, {"resolution", int64_t(2)}});
	g.CompileNow();
	const auto value = g.Run();
	const auto &curve = std::get<Curve>(value);
	CHECK(curve.Anchors[1][2] == Catch::Approx(.5));
	CHECK(curve.Anchors[1][3] == Catch::Approx(.25));
	CHECK(curve.Anchors.back()[2] == 0);
	CHECK(curve.Anchors.back()[3] == 0);
}
TEST_CASE("Path to Curve source initial noone default", "[imagegraph][source_path_modifier]") {
	ModifierGraph g("pc.path_to_curve");
	g.CompileNow();
	const auto value = g.Run();
	const auto &curve = std::get<Curve>(value);
	REQUIRE(curve.Anchors.size() == 2);
	CHECK(curve.Anchors[0][3] == 0);
	CHECK(curve.Anchors[1][3] == 1);
}
TEST_CASE("Path Skew shear preserves source metadata and weights", "[imagegraph][source_path_modifier]") {
	ModifierGraph g(
		"pc.path_skew",
		{{"path", ModifierLine({2, 4}, {12, 14})},
		 {"center", Vector2{1, 3}},
		 {"center_unit", EnumValue{0}},
		 {"strength", 2.}}
	);
	g.CompileNow();
	g.Roundtrip();
	const auto value = g.Run();
	const auto &path = std::get<Path2D>(value);
	EvaluationRequest request;
	detail::NodeContext context{g.Doc.Nodes[0], *FindCatalogueEntry("pc.path_skew"), request};
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, path));
	CHECK(runtime.Length() == Catch::Approx(std::sqrt(200.)));
	CHECK(runtime.MinX == 2);
	CHECK(runtime.MaxY == 14);
	CHECK(runtime.PointRatio(.5).Weight == 1);
	const auto p = g.Sample();
	CHECK(p.X == Catch::Approx(19));
	CHECK(p.Y == Catch::Approx(9));
}
TEST_CASE("Path Skew Y axis signed strength", "[imagegraph][source_path_modifier]") {
	ModifierGraph g(
		"pc.path_skew",
		{{"path", ModifierLine({2, 4}, {12, 14})},
		 {"axis", EnumValue{1}},
		 {"center", Vector2{1, 3}},
		 {"center_unit", EnumValue{0}},
		 {"strength", -2.}}
	);
	const auto p = g.Sample();
	CHECK(p.X == Catch::Approx(7));
	CHECK(p.Y == Catch::Approx(-3));
}
TEST_CASE("Path Redistribute reverse curve and durable lazy payload", "[imagegraph][source_path_modifier]") {
	Curve curve;
	curve.Header = {0, 1, 0, 0, 1, 0};
	curve.Anchors = {{{0, 0, 0, 1, 1. / 3, -1. / 3}}, {{-1. / 3, 1. / 3, 1, 0, 0, 0}}};
	ModifierGraph g("pc.path_redistribute", {{"path", ModifierLine({0, 0}, {10, 0})}, {"curve", curve}});
	g.CompileNow();
	const auto value = g.Run();
	Document saved;
	saved.FormatVersion = 9;
	saved.Nodes = {{"saved", "pc.path_sample", "", {}, {{"path", value}, {"ratio", .25}}}};
	saved.Outputs = {{"point", "saved", "position"}};
	Document restored;
	Diagnostic d;
	REQUIRE(Read(Write(saved), restored, d) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(restored, plan, d) == Status::Ok);
	EvaluatedValue out;
	REQUIRE(EvaluateValue(restored, plan, "point", {}, out, d) == Status::Ok);
	CHECK(std::get<Vector2>(out.Data).X == Catch::Approx(7.5));
}
TEST_CASE("Path Redistribute source 32-grid step interpolation", "[imagegraph][source_path_modifier]") {
	Curve curve;
	curve.Header = {0, 1, 1, 0, 1, 0};
	curve.Anchors = {{{0, 0, 0, .2, 0, 0}}, {{0, 0, .5, .8, 0, 0}}, {{0, 0, 1, 1, 0, 0}}};
	ModifierGraph g("pc.path_redistribute", {{"path", ModifierLine({0, 0}, {10, 0})}, {"curve", curve}});
	const auto p = g.Sample(.51);
	CHECK(p.X == Catch::Approx(3.92));
}
TEST_CASE("Path modifier refusal preserves output", "[imagegraph][source_path_modifier]") {
	ModifierGraph g("pc.path_to_curve", {{"path", ModifierLine()}, {"resolution", int64_t(4096)}});
	g.CompileNow();
	EvaluatedValue out;
	out.Data = std::string("preserved");
	CHECK(EvaluateValue(g.Doc, g.Compiled, "value", {}, out, g.Error) == Status::InvalidValue);
	CHECK(std::get<std::string>(out.Data) == "preserved");
	g.Doc.Nodes[0].Values.back().Data = int64_t(16);
	g.CompileNow();
	EvaluationRequest request;

	StatefulEvaluationResult state;
	state.Output = out;
	CHECK(EvaluateStateful(g.Doc, g.Compiled, "value", request, state, g.Error, 1) == Status::LimitExceeded);
	CHECK(std::get<EvaluatedValue>(state.Output) == out);
}
TEST_CASE(
	"Path modifier no-write preserves prior curve across refresh and budget retry",
	"[imagegraph][source_path_modifier]"
) {
	ModifierGraph g("pc.path_to_curve", {{"path", ModifierLine()}, {"resolution", int64_t(2)}});
	Node base = g.Doc.Nodes[0];
	base.Id = "base";
	g.Doc.Nodes[0].Values.clear();
	g.Doc.Nodes[0].InstanceBase = "base";
	g.Doc.Nodes.insert(g.Doc.Nodes.begin(), std::move(base));
	g.CompileNow();
	StatefulEvaluationResult state;
	EvaluationRequest request;
	request.DataReplay = &state.Data;
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "value", request, state, g.Error) == Status::Ok);
	const auto previous = std::get<EvaluatedValue>(state.Output);
	const auto history = state.Data;
	g.Doc.Nodes[1].InstanceOverrides = {"path"};
	g.CompileNow();
	request.Tick = 1;
	CHECK(EvaluateStateful(g.Doc, g.Compiled, "value", request, state, g.Error, 1) == Status::LimitExceeded);
	CHECK(state.Data == history);
	CHECK(std::get<EvaluatedValue>(state.Output) == previous);
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "value", request, state, g.Error) == Status::Ok);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == previous.Data);
	CHECK(state.Data == history);
	g.Doc.Nodes[1].InstanceOverrides.clear();
	g.Doc.Nodes[0].Values[1].Data = int64_t(3);
	g.CompileNow();
	request.Tick = 2;
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "value", request, state, g.Error) == Status::Ok);
	CHECK(std::get<Curve>(std::get<EvaluatedValue>(state.Output).Data).Anchors.size() == 4);
	REQUIRE(state.Data.Entries.size() == 1);
	CHECK(state.Data.Entries[0].Values.size() == 1);
}
TEST_CASE(
	"Path Skew source line-zero distance quirk preserves independent metadata",
	"[imagegraph][source_path_modifier]"
) {
	Path2D combined;
	auto &op = combined.SourceOperation.emplace();
	op.Kind = SourcePathOperationKind::Combine;
	op.Inputs = {ModifierLine({0, 0}, {10, 0}), ModifierLine({100, 100}, {120, 100})};
	ModifierGraph g(
		"pc.path_skew",
		{{"path", combined}, {"strength", 1.}, {"center", Vector2{0, 0}}, {"center_unit", EnumValue{0}}}
	);
	g.CompileNow();
	const auto value = g.Run();
	EvaluationRequest request;
	detail::NodeContext context(g.Doc.Nodes[0], *FindCatalogueEntry("pc.path_skew"), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, std::get<Path2D>(value)));
	CHECK(runtime.LineCount() == 2);
	CHECK(runtime.Length(1) == 10);
	CHECK(runtime.PointRatio(.5, 1).X == 5);
	CHECK(runtime.PointRatio(.5, 1).Y == 0);
	CHECK(runtime.AccumulatedAt(0, 1) == 20);
}
TEST_CASE(
	"Path Redistribute batch work is refused before candidate publication",
	"[imagegraph][source_path_modifier]"
) {
	auto large = ModifierLine();
	large.Anchors.resize(1024, large.Anchors.back());
	Curve curve;
	curve.Header = {0, 1, 0, 0, 1, 0};
	curve.Anchors = {{{0, 0, 0, 0, 1. / 3, 1. / 3}}, {{-1. / 3, -1. / 3, 1, 1, 0, 0}}};
	ModifierGraph g("pc.path_redistribute", {{"path", large}});
	Node rows{"rows", "pc.array", "", {}, {}};
	for (size_t i = 0; i < 64; ++i)
		rows.DynamicInputs.push_back({"input_0_" + std::to_string(i), ValueType::Any, Value{curve}});
	g.Doc.Nodes.push_back(std::move(rows));
	g.Doc.Links = {{"rows", "array", "operation", "curve"}};
	g.CompileNow();
	EvaluatedValue out;
	out.Data = std::string("unchanged");
	CHECK(EvaluateValue(g.Doc, g.Compiled, "value", {}, out, g.Error) == Status::LimitExceeded);
	CHECK(g.Error.NodeId == "operation");
	CHECK(g.Error.Message.find("processor batch") != std::string::npos);
	CHECK(std::get<std::string>(out.Data) == "unchanged");
}
TEST_CASE(
	"Path to Curve default anchors require admitted bytes before construction",
	"[imagegraph][source_path_modifier]"
) {
	ModifierGraph g("pc.path_to_curve");
	g.CompileNow();
	StatefulEvaluationResult state;
	EvaluatedValue sentinel;
	sentinel.Data = std::string("unchanged");
	state.Output = sentinel;
	EvaluationRequest request;
	CHECK(EvaluateStateful(g.Doc, g.Compiled, "value", request, state, g.Error, 1) == Status::LimitExceeded);
	CHECK(std::get<EvaluatedValue>(state.Output) == sentinel);
	const auto *entry = FindCatalogueEntry("pc.path_to_curve");
	REQUIRE(entry);
	detail::NodeContext context(g.Doc.Nodes[0], *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.InputProvenanceResolved = true;
	context.CatalogueDefaultInputs = {"path"};
	REQUIRE(context.ReserveOutput(0, "curve"));
	context.ByteBudget = context.AllocationBudget().Used() + 2 * sizeof(std::array<double, 6>) - 1;
	const auto run = detail::FindExecutor("pc.path_to_curve");
	REQUIRE(run);
	CHECK_FALSE(run(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "curve");
	CHECK(context.OutputValues.empty());
	CHECK(context.DataUpdates.empty());
}
