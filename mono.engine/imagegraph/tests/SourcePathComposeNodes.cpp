#include "nodes/Path.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_path_compose")
using namespace engine::imagegraph;
namespace {
	Path2D Straight(Vector2 a, Vector2 b, double weight = 1) {
		Path2D p;
		p.Anchors = {{{a.X, a.Y, 0, 0, 0, 0}, 0}, {{b.X, b.Y, 0, 0, 0, 0}, 0}};
		p.Weights = {{0, weight}, {100, weight}};
		return p;
	}
	struct ComposeGraph {
		Document Doc;
		Plan Compiled;
		Diagnostic Error;
		ComposeGraph(std::string_view type, std::vector<AuthoredValue> controls = {}) {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"operation", std::string(type), "", {}, std::move(controls)},
				{"sample", "pc.path_sample", "", {}, {{"ratio", .5}}}
			};
			const auto port = type == "pc.path_join" ? "joined_path" : "path";
			Doc.Links = {{"operation", port, "sample", "path"}};
			Doc.Outputs = {
				{"path", "operation", port},
				{"position", "sample", "position"},
				{"weight", "sample", "weight"}
			};
		}
		void CompileNow() {
			const auto status = Compile(Doc, Compiled, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
		}
		Value Run(std::string_view id = "position") {
			EvaluatedValue out;
			const auto status = EvaluateValue(Doc, Compiled, std::string(id), {}, out, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			return out.Data;
		}
		void Ratio(double ratio) {
			Doc.Nodes[1].Values[0].Data = ratio;
			CompileNow();
		}
		void Roundtrip() {
			Document copy;
			REQUIRE(Read(Write(Doc), copy, Error) == Status::Ok);
			Doc = std::move(copy);
			CompileNow();
		}
	};
}
TEST_CASE(
	"Path Offset wraps positive and negative ratios and keeps length", "[imagegraph][source_path_compose]"
) {
	ComposeGraph g("pc.path_offset", {{"path", Straight({0, 0}, {10, 0})}, {"offset", .75}});
	g.CompileNow();
	CHECK(std::get<Vector2>(g.Run()).X == Catch::Approx(2.5));
	g.Doc.Nodes[0].Values[1].Data = -.75;
	g.CompileNow();
	CHECK(std::get<Vector2>(g.Run()).X == Catch::Approx(7.5));
	const auto p = std::get<Path2D>(g.Run("path"));
	REQUIRE(p.SourceOperation);
	CHECK(p.SourceOperation->Kind == SourcePathOperationKind::Offset);
	g.Roundtrip();
	CHECK(std::get<Path2D>(g.Run("path")) == p);
}
TEST_CASE(
	"Path Offset clamp delegates source endpoint instead of wrapping", "[imagegraph][source_path_compose]"
) {
	ComposeGraph g("pc.path_offset", {{"path", Straight({0, 0}, {10, 0})}, {"offset", .75}, {"clamp", true}});
	g.CompileNow();
	CHECK(std::get<Vector2>(g.Run()).X == Catch::Approx(9.999));
	g.Doc.Nodes[0].Values[1].Data = -.75;
	g.CompileNow();
	CHECK(std::get<Vector2>(g.Run()) == Vector2{0, 0});
}
TEST_CASE("Path Blend preserves all four source modes and weights", "[imagegraph][source_path_compose]") {
	const std::array<Vector2, 4> points{{{2.5, 1.5}, {5, 1.5}, {5, -1.5}, {5, -1.5}}};
	const std::array<double, 4> weights{3, 4, 0, 3};
	for (int64_t mode = 0; mode < 4; ++mode) {
		ComposeGraph g(
			"pc.path_blend",
			{{"path_1", Straight({0, 0}, {10, 0}, 2)},
			 {"path_2", Straight({0, 2}, {0, 4}, 4)},
			 {"mode", EnumValue{mode}},
			 {"amount", .5}}
		);
		g.CompileNow();
		auto p = std::get<Vector2>(g.Run());
		CHECK(p.X == Catch::Approx(points[size_t(mode)].X));
		CHECK(p.Y == Catch::Approx(points[size_t(mode)].Y));
		CHECK(std::get<double>(g.Run("weight")) == Catch::Approx(weights[size_t(mode)]));
		const auto path = std::get<Path2D>(g.Run("path"));
		const auto *entry = FindCatalogueEntry("pc.path_blend");
		REQUIRE(entry);
		EvaluationRequest request;
		Node authored{"probe", "pc.path_blend", "", {}, {}};
		detail::NodeContext context(authored, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		detail::PathRuntime runtime;
		const bool ready = runtime.Init(context, path);
		INFO(context.FailureMessage);
		REQUIRE(ready);
		CHECK(runtime.Length() == Catch::Approx(mode == 0 ? 6 : 12));
		CHECK(runtime.AccumulatedCount() == 1);
		CHECK(runtime.AccumulatedAt(0) == Catch::Approx(mode == 0 ? 6 : 10));
		CHECK(runtime.MinX == Catch::Approx(0));
		CHECK(runtime.MinY == Catch::Approx(1));
		CHECK(runtime.MaxX == Catch::Approx(5));
		CHECK(runtime.MaxY == Catch::Approx(2));
		g.Roundtrip();
		CHECK(std::get<Path2D>(g.Run("path")) == path);
	}
}
TEST_CASE(
	"Path Blend missing input forwards the other path while fresh length stays zero",
	"[imagegraph][source_path_compose]"
) {
	ComposeGraph g("pc.path_blend", {{"path_2", Straight({10, 20}, {30, 20})}, {"amount", 1.}});
	g.CompileNow();
	CHECK(std::get<Vector2>(g.Run()) == Vector2{20, 20});
	const auto path = std::get<Path2D>(g.Run("path"));
	REQUIRE(path.SourceOperation);
	CHECK(path.SourceOperation->BlendInputsValid == std::array<bool, 2>{false, true});
	const auto *entry = FindCatalogueEntry("pc.path_blend");
	REQUIRE(entry);
	EvaluationRequest request;
	Node authored{"probe", "pc.path_blend", "", {}, {}};
	detail::NodeContext c(authored, *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime runtime;
	const bool ready = runtime.Init(c, path);
	INFO(c.FailureMessage);
	REQUIRE(ready);
	CHECK(runtime.Length() == 0);
}
TEST_CASE(
	"Path Join uses source .999 endpoint translation and strict boundary selection",
	"[imagegraph][source_path_compose]"
) {
	ComposeGraph g("pc.path_join");
	g.Doc.Nodes[0].DynamicInputs = {
		{"path_0", ValueType::Path2D, Straight({0, 0}, {10, 0})},
		{"reverse_0", ValueType::Boolean, false},
		{"path_1", ValueType::Path2D, Straight({20, 20}, {30, 20})},
		{"reverse_1", ValueType::Boolean, false}
	};
	g.CompileNow();
	CHECK(std::get<Vector2>(g.Run()) == Vector2{10, 0});
	g.Ratio(.75);
	CHECK(std::get<Vector2>(g.Run()).X == Catch::Approx(14.99));
	CHECK(std::get<Vector2>(g.Run()).Y == Catch::Approx(0));
	g.Doc.Nodes[0].DynamicInputs[3].Default = true;
	g.CompileNow();
	CHECK(std::get<Vector2>(g.Run()).X == Catch::Approx(5));
	g.Roundtrip();
	CHECK(std::get<Vector2>(g.Run()).X == Catch::Approx(5));
	const auto path = std::get<Path2D>(g.Run("path"));
	const auto *entry = FindCatalogueEntry("pc.path_join");
	REQUIRE(entry);
	EvaluationRequest request;
	Node authored{"probe", "pc.path_join", "", {}, {}};
	detail::NodeContext context(authored, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime runtime;
	const bool ready = runtime.Init(context, path);
	INFO(context.FailureMessage);
	REQUIRE(ready);
	CHECK(runtime.LineCount() == 2);
	CHECK(runtime.Length() == Catch::Approx(20));
	CHECK(runtime.AccumulatedCount() == 2);
	CHECK(runtime.AccumulatedAt(0) == Catch::Approx(10));
	CHECK(runtime.AccumulatedAt(1) == Catch::Approx(10));
	CHECK(runtime.MinX == -4);
	CHECK(runtime.MinY == -4);
	CHECK(runtime.MaxX == 30);
	CHECK(runtime.MaxY == 20);
}
TEST_CASE("Nested Offset and Blend preserve independent path lines", "[imagegraph][source_path_compose]") {
	Path2D combined;
	auto &op = combined.SourceOperation.emplace();
	op.Kind = SourcePathOperationKind::Combine;
	op.Inputs = {Straight({0, 0}, {10, 0}), Straight({0, 10}, {20, 10})};
	ComposeGraph g("pc.path_offset", {{"path", combined}, {"offset", .25}});
	g.Doc.Nodes[1].Values.push_back({"path_index", int64_t{1}});
	g.CompileNow();
	CHECK(std::get<Vector2>(g.Run()) == Vector2{15, 10});
	g.Roundtrip();
	CHECK(std::get<Vector2>(g.Run()) == Vector2{15, 10});
}
TEST_CASE("Path composition processor controls create owned lazy rows", "[imagegraph][source_path_compose]") {
	ComposeGraph g(
		"pc.path_offset",
		{{"path", Straight({0, 0}, {10, 0})}, {"offset", ArrayValue{ValueType::Scalar, {0., .25}}}}
	);
	g.Doc.Links.clear();
	g.Doc.Outputs = {{"path", "operation", "path"}};
	g.CompileNow();
	const auto result = std::get<ArrayValue>(g.Run("path"));
	REQUIRE(result.Elements.size() == 2);
	const auto &a = std::get<Path2D>(result.Elements[0]);
	const auto &b = std::get<Path2D>(result.Elements[1]);
	REQUIRE(a.SourceOperation);
	REQUIRE(b.SourceOperation);
	CHECK(a.SourceOperation->Offset == 0);
	CHECK(b.SourceOperation->Offset == .25);
}
TEST_CASE(
	"Path composition bounds refuse deep owned trees without publishing", "[imagegraph][source_path_compose]"
) {
	Path2D path = Straight({0, 0}, {10, 0});
	for (size_t i = 0; i < Limits::MaximumArrayDepth; ++i) {
		Path2D outer;
		auto &op = outer.SourceOperation.emplace();
		op.Kind = SourcePathOperationKind::Reverse;
		op.Inputs.push_back(std::move(path));
		path = std::move(outer);
	}
	ComposeGraph g("pc.path_offset", {{"path", path}});
	g.CompileNow();
	EvaluatedValue out;
	out.Data = std::string{"retained"};
	CHECK(EvaluateValue(g.Doc, g.Compiled, "path", {}, out, g.Error) == Status::LimitExceeded);
	CHECK(std::get<std::string>(out.Data) == "retained");
	ComposeGraph good("pc.path_offset", {{"path", Straight({0, 0}, {10, 0})}});
	good.CompileNow();
	StatefulEvaluationResult candidate;
	candidate.Output = out;
	CHECK(
		EvaluateStateful(good.Doc, good.Compiled, "path", {}, candidate, good.Error, 1) ==
		Status::LimitExceeded
	);
	CHECK(std::get<EvaluatedValue>(candidate.Output) == out);
	CHECK(std::get<std::string>(out.Data) == "retained");
}
TEST_CASE(
	"Path Blend retains cached lengths through instance refresh seek and atomic retry",
	"[imagegraph][source_path_compose]"
) {
	ComposeGraph g(
		"pc.path_blend",
		{{"path_1", Straight({0, 0}, {10, 0})}, {"path_2", Straight({0, 2}, {0, 4})}, {"amount", .5}}
	);
	Node base = g.Doc.Nodes[0];
	base.Id = "base";
	g.Doc.Nodes[0].Values.clear();
	g.Doc.Nodes[0].InstanceBase = "base";
	g.Doc.Nodes.insert(g.Doc.Nodes.begin(), std::move(base));
	g.CompileNow();
	StatefulEvaluationResult state;
	EvaluationRequest request;
	request.DataReplay = &state.Data;
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "path", request, state, g.Error) == Status::Ok);
	const auto both = std::get<Path2D>(std::get<EvaluatedValue>(state.Output).Data);
	REQUIRE(both.SourceOperation);
	CHECK(both.SourceOperation->BlendLengths == std::vector<double>{6});
	g.Doc.Nodes[1].InstanceOverrides = {"path_2"};
	g.CompileNow();
	request.Tick = 1;
	const auto before = std::get<EvaluatedValue>(state.Output);
	const auto history = state.Data;
	CHECK(EvaluateStateful(g.Doc, g.Compiled, "path", request, state, g.Error, 1) == Status::LimitExceeded);
	CHECK(state.Data == history);
	CHECK(std::get<EvaluatedValue>(state.Output) == before);
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "path", request, state, g.Error) == Status::Ok);
	const auto one = std::get<Path2D>(std::get<EvaluatedValue>(state.Output).Data);
	REQUIRE(one.SourceOperation);
	CHECK(one.SourceOperation->BlendInputsValid == std::array<bool, 2>{true, false});
	CHECK(one.SourceOperation->BlendLengths == both.SourceOperation->BlendLengths);
	CHECK(one.SourceOperation->BlendAccumulated == both.SourceOperation->BlendAccumulated);
	request.Tick = 0;
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "path", request, state, g.Error) == Status::Ok);
	CHECK(std::get<Path2D>(std::get<EvaluatedValue>(state.Output).Data) == one);
	Document persisted;
	persisted.FormatVersion = 9;
	persisted.Nodes = {{"saved", "pc.path_offset", "", {}, {{"path", one}}}};
	persisted.Outputs = {{"out", "saved", "path"}};
	Document copy;
	REQUIRE(Read(Write(persisted), copy, g.Error) == Status::Ok);
	CHECK(copy.Nodes[0].Values[0].Data == Value{one});
	g.Doc.Nodes[1].InstanceOverrides.clear();
	g.Doc.Nodes[0].Values[2].Data = .25;
	g.CompileNow();
	request.Tick = 2;
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "path", request, state, g.Error) == Status::Ok);
	const auto replaced = std::get<Path2D>(std::get<EvaluatedValue>(state.Output).Data);
	REQUIRE(replaced.SourceOperation);
	CHECK(replaced.SourceOperation->BlendLengths == std::vector<double>{8});
	REQUIRE(state.Data.Entries.size() == 1);
	CHECK(state.Data.Entries[0].Values.size() == 1);
}
TEST_CASE(
	"Path composition admits the whole processor batch before cloning output trees",
	"[imagegraph][source_path_compose]"
) {
	Path2D dense;
	dense.Weights = {{0, 1}, {100, 1}};
	for (size_t i = 0; i < 1024; ++i)
		dense.Anchors.push_back({{double(i), 0, 0, 0, 0, 0}, 0});
	ArrayValue offsets{ValueType::Scalar, {}};
	offsets.Elements.assign(64, 0.);
	ComposeGraph g("pc.path_offset", {{"path", dense}, {"offset", offsets}});
	g.Doc.Links.clear();
	g.Doc.Outputs = {{"path", "operation", "path"}};
	g.CompileNow();
	EvaluatedValue out;
	out.Data = std::string{"retained"};
	CHECK(EvaluateValue(g.Doc, g.Compiled, "path", {}, out, g.Error) == Status::LimitExceeded);
	CHECK(g.Error.Message.find("processor batch") != std::string::npos);
	CHECK(std::get<std::string>(out.Data) == "retained");
}
TEST_CASE(
	"Native path compose codec rejects malformed cached arrays atomically",
	"[imagegraph][source_path_compose]"
) {
	Path2D cached;
	auto &op = cached.SourceOperation.emplace();
	op.Kind = SourcePathOperationKind::Blend;
	op.Inputs = {Straight({0, 0}, {10, 0}), Straight({0, 0}, {2, 0})};
	op.BlendInputsValid = {true, true};
	op.BlendAmount = .5;
	op.BlendLengths = {6};
	op.BlendAccumulated = {{6}};
	Document authored;
	authored.FormatVersion = 9;
	authored.Nodes = {{"saved", "pc.path_offset", "", {}, {{"path", cached}}}};
	authored.Outputs = {{"out", "saved", "path"}};
	Diagnostic error;
	const auto text = Write(authored);
	Document retained;
	retained.FormatVersion = 9;
	retained.Nodes = {{"retained", "pc.number_simple", "", {}, {}}};
	const auto before = Write(retained);
	auto hostile = text;
	const auto marker = hostile.find("blend 2 0 0.5 1 1 1 6 1 6");
	REQUIRE(marker != std::string::npos);
	hostile.replace(marker, std::string("blend 2 0 0.5 1 1 1 6 1 6").size(), "blend 2 0 0.5 1 1 1 6 4097");
	CHECK(Read(hostile, retained, error) != Status::Ok);
	CHECK(Write(retained) == before);
	Document restored;
	REQUIRE(Read(text, restored, error) == Status::Ok);
	CHECK(restored.Nodes[0].Values[0].Data == Value{cached});
}
TEST_CASE(
	"Path Join consumes a general array only at the source path template", "[imagegraph][source_path_compose]"
) {
	ComposeGraph g("pc.path_join");
	g.Doc.Nodes[0].DynamicInputs = {
		{"path_0", ValueType::Path2D, Path2D{}}, {"reverse_0", ValueType::Boolean, false}
	};
	Node first{"a", "pc.path_offset", "", {}, {{"path", Straight({0, 0}, {10, 0})}, {"clamp", true}}};
	first.SourceInternalName = "a";
	Node second{"b", "pc.path_offset", "", {}, {{"path", Straight({20, 20}, {30, 20})}, {"clamp", true}}};
	second.SourceInternalName = "b";
	g.Doc.Nodes.push_back(std::move(first));
	g.Doc.Nodes.push_back(std::move(second));
	g.Doc.Nodes[0].SourceInputExpressions = {{"path_0", "[a.outputs.path,b.outputs.path]", true}};
	g.Ratio(.75);
	const auto position = std::get<Vector2>(g.Run());
	CHECK(position.X == Catch::Approx(14.99));
	CHECK(position.Y == Catch::Approx(0));
	const auto path = std::get<Path2D>(g.Run("path"));
	REQUIRE(path.SourceOperation);
	REQUIRE(path.SourceOperation->Inputs.size() == 2);
	g.Roundtrip();
	CHECK(std::get<Vector2>(g.Run()).X == Catch::Approx(14.99));
}
