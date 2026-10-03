#include "nodes/Path.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_path_geometry")
using namespace engine::imagegraph;
namespace {
	Path2D GeometryLine(Vector2 a = {0, 0}, Vector2 b = {10, 10}) {
		Path2D p;
		p.Anchors = {{{a.X, a.Y, 0, 0, 0, 0}, 0}, {{b.X, b.Y, 0, 0, 0, 0}, 0}};
		p.Weights = {{0, 1}, {100, 1}};
		return p;
	}
	struct GeometryGraph {
		Document Doc;
		Plan Compiled;
		Diagnostic Error;
		GeometryGraph(std::string type, std::vector<AuthoredValue> controls = {}) {
			Doc.FormatVersion = 9;
			Doc.Nodes = {{"operation", type, "", {}, std::move(controls)}};
			Doc.Outputs = {{"value", "operation", "path"}};
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
	"Path Transform source pivot scale rotation position and unchanged length",
	"[imagegraph][source_path_geometry]"
) {
	GeometryGraph g(
		"pc.path_transform",
		{{"path", GeometryLine({2, 4}, {12, 14})},
		 {"position", Vector2{2, 5}},
		 {"position_unit", EnumValue{0}},
		 {"anchor", Vector2{1, 3}},
		 {"anchor_unit", EnumValue{0}},
		 {"scale", Vector2{2, -1}},
		 {"rotation", 90.}}
	);
	g.CompileNow();
	g.Roundtrip();
	const auto value = g.Run();
	EvaluationRequest request;
	detail::NodeContext context(g.Doc.Nodes[0], *FindCatalogueEntry("pc.path_transform"), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime path;
	REQUIRE(path.Init(context, std::get<Path2D>(value)));
	CHECK(path.Length() == Catch::Approx(std::sqrt(200.)));
	CHECK(path.MinX == Catch::Approx(-8));
	CHECK(path.MaxX == Catch::Approx(2));
	CHECK(path.MinY == Catch::Approx(-14));
	CHECK(path.MaxY == Catch::Approx(6));
	const auto p = g.Sample();
	CHECK(p.X == Catch::Approx(-3));
	CHECK(p.Y == Catch::Approx(-4));
}
TEST_CASE(
	"Path Transform preserves source two-corner bounds rather than enclosing rectangle",
	"[imagegraph][source_path_geometry]"
) {
	auto input = GeometryLine();
	input.Anchors = {
		{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}, {{10, 10, 0, 0, 0, 0}, 0}, {{0, 10, 0, 0, 0, 0}, 0}
	};
	input.Loop = true;
	GeometryGraph g("pc.path_transform", {{"path", input}, {"rotation", 45.}});
	g.CompileNow();
	const auto value = g.Run();
	EvaluationRequest request;
	detail::NodeContext context(g.Doc.Nodes[0], *FindCatalogueEntry("pc.path_transform"), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime path;
	REQUIRE(path.Init(context, std::get<Path2D>(value)));
	CHECK(path.MinY == Catch::Approx(0).margin(1e-12));
	CHECK(path.MaxY == Catch::Approx(0).margin(1e-12));
	CHECK(path.PointRatio(.125).Y == Catch::Approx(-std::sqrt(12.5)));
}
TEST_CASE(
	"Path Transform missing source input is a usable empty wrapper", "[imagegraph][source_path_geometry]"
) {
	GeometryGraph g("pc.path_transform", {{"position", Vector2{7, 9}}, {"position_unit", EnumValue{0}}});
	const auto p = g.Sample();
	CHECK(p == Vector2{0, 0});
}
TEST_CASE("Path Map Area covers all source from and to choices", "[imagegraph][source_path_geometry]") {
	for (int64_t from = 0; from < 3; ++from)
		for (int64_t to = 0; to < 3; ++to) {
			GeometryGraph g(
				"pc.path_map_area",
				{{"path", GeometryLine({10, 20}, {30, 60})},
				 {"map_from", EnumValue{from}},
				 {"map_to", EnumValue{to}},
				 {"dimension_from", Vector2{20, 40}},
				 {"dimension_from_unit", EnumValue{0}},
				 {"bbox_from", Vector4{10, 20, 30, 60}},
				 {"area", Area{100, 200, 10, 20, 1, 0}},
				 {"dimension_to", Vector2{20, 40}},
				 {"dimension_to_unit", EnumValue{0}},
				 {"bbox_to", Vector4{90, 180, 110, 220}}}
			);
			const auto p = g.Sample();
			const double x = from == 1 ? 1. : .5, y = from == 1 ? 1. : .5;
			CHECK(p.X == Catch::Approx((to == 1 ? 0 : 90) + x * 20));
			CHECK(p.Y == Catch::Approx((to == 1 ? 0 : 180) + y * 40));
		}
}
TEST_CASE(
	"Path Map Area ignores authored shape and padding mode fields without a unit binding",
	"[imagegraph][source_path_geometry]"
) {
	GeometryGraph g(
		"pc.path_map_area",
		{{"path", GeometryLine({0, 0}, {10, 10})}, {"area", Area{100, 200, -10, 20, 1, 1}}}
	);
	g.CompileNow();
	g.Roundtrip();
	const auto p = g.Sample(.25);
	CHECK(p.X == Catch::Approx(105));
	CHECK(p.Y == Catch::Approx(190));
}
TEST_CASE(
	"Path Map Area source default uses project physical dimensions", "[imagegraph][source_path_geometry]"
) {
	GeometryGraph g("pc.path_map_area", {{"path", GeometryLine({0, 0}, {10, 10})}});
	g.Doc.Project.emplace();
	g.Doc.Project->SurfaceWidth = 20;
	g.Doc.Project->SurfaceHeight = 40;
	const auto p = g.Sample();
	CHECK(p == Vector2{10, 20});
}
TEST_CASE(
	"Path Map Area initial missing path retains constructor bounds and zero position",
	"[imagegraph][source_path_geometry]"
) {
	GeometryGraph g(
		"pc.path_map_area",
		{{"map_to", EnumValue{1}}, {"dimension_to", Vector2{20, 40}}, {"dimension_to_unit", EnumValue{0}}}
	);
	g.CompileNow();
	const auto value = g.Run();
	EvaluationRequest request;
	detail::NodeContext context(g.Doc.Nodes[0], *FindCatalogueEntry("pc.path_map_area"), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime path;
	REQUIRE(path.Init(context, std::get<Path2D>(value)));
	CHECK(path.Length() == 0);
	CHECK(path.MinX == -1);
	CHECK(path.MaxY == 1);
	CHECK(path.PointRatio(.5).X == 0);
}
TEST_CASE(
	"Path Map Area zero source extent refuses atomic native nonfinite result",
	"[imagegraph][source_path_geometry]"
) {
	GeometryGraph g("pc.path_map_area", {{"path", GeometryLine({0, 0}, {0, 10})}});
	g.CompileNow();
	EvaluatedValue out;
	out.Data = std::string("preserved");
	CHECK(EvaluateValue(g.Doc, g.Compiled, "value", {}, out, g.Error) == Status::UnsupportedExecution);
	CHECK(std::get<std::string>(out.Data) == "preserved");
	CHECK(g.Error.Message.find("nonfinite") != std::string::npos);
}
TEST_CASE(
	"Path Map Area retains complete wrapper when source path disappears", "[imagegraph][source_path_geometry]"
) {
	GeometryGraph g(
		"pc.path_map_area",
		{{"path", GeometryLine()}, {"map_to", EnumValue{2}}, {"bbox_to", Vector4{90, 180, 110, 220}}}
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
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "value", request, state, g.Error) == Status::Ok);
	const auto prior = std::get<EvaluatedValue>(state.Output);
	const auto history = state.Data;
	g.Doc.Nodes[1].InstanceOverrides = {"path"};
	g.Doc.Nodes[0].Values[2].Data = Vector4{0, 0, 2, 2};
	g.CompileNow();
	request.Tick = 1;
	CHECK(EvaluateStateful(g.Doc, g.Compiled, "value", request, state, g.Error, 1) == Status::LimitExceeded);
	CHECK(std::get<EvaluatedValue>(state.Output) == prior);
	CHECK(state.Data == history);
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "value", request, state, g.Error) == Status::Ok);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == prior.Data);
	Document saved;
	saved.FormatVersion = 9;
	saved.Nodes = {{"saved", "pc.path_sample", "", {}, {{"path", prior.Data}, {"ratio", .5}}}};
	saved.Outputs = {{"point", "saved", "position"}};
	Document restored;
	REQUIRE(Read(Write(saved), restored, g.Error) == Status::Ok);
	Plan compiled;
	REQUIRE(Compile(restored, compiled, g.Error) == Status::Ok);
	EvaluatedValue out;
	REQUIRE(EvaluateValue(restored, compiled, "point", {}, out, g.Error) == Status::Ok);
	CHECK(std::get<Vector2>(out.Data) == Vector2{100, 200});
}
TEST_CASE(
	"Path Transform admits entire processor batch before owned clones", "[imagegraph][source_path_geometry]"
) {
	auto dense = GeometryLine();
	dense.Anchors.resize(1024, dense.Anchors.back());
	ArrayValue scales{ValueType::Vector2, {}};
	scales.Elements.assign(64, Vector2{1, 1});
	GeometryGraph g("pc.path_transform", {{"path", dense}, {"scale", scales}});
	g.CompileNow();
	EvaluatedValue out;
	out.Data = std::string("retained");
	CHECK(EvaluateValue(g.Doc, g.Compiled, "value", {}, out, g.Error) == Status::LimitExceeded);
	CHECK(std::get<std::string>(out.Data) == "retained");
	CHECK(g.Error.Message.find("processor batch") != std::string::npos);
}
TEST_CASE("Path Transform source exact half-turn and signed scale", "[imagegraph][source_path_geometry]") {
	GeometryGraph g(
		"pc.path_transform",
		{{"path", GeometryLine({2, 4}, {12, 14})}, {"rotation", 180.}, {"scale", Vector2{-1, 2}}}
	);
	const auto p = g.Sample();
	CHECK(p == Vector2{7, -18});
}

TEST_CASE("Path Map Area accepts the context-independent Area output", "[imagegraph][source_path_geometry]") {
	GeometryGraph g("pc.path_map_area", {{"path", GeometryLine()}});
	g.Doc.Nodes.push_back(
		{"area", "pc.area", "", {}, {{"position", Vector2{100, 200}}, {"span", Vector2{10, 20}}}}
	);
	g.Doc.Links.push_back({"area", "area", "operation", "area"});
	g.CompileNow();
	g.Roundtrip();
	CHECK(g.Sample(.25) == Vector2{95, 190});
}
TEST_CASE(
	"Path Map Area keeps raw context-independent Area bypass fields", "[imagegraph][source_path_geometry]"
) {
	GeometryGraph g("pc.path_map_area", {{"path", GeometryLine()}});
	g.Doc.Nodes.push_back({"area", "pc.path_map_area", "", {}, {{"area", Area{100, 200, -10, 20, 1, 1}}}});
	g.Doc.Links.push_back({"area", "area.bypass", "operation", "area"});
	g.CompileNow();
	g.Roundtrip();
	CHECK(g.Sample(.25) == Vector2{105, 190});
}
TEST_CASE(
	"Path Map Area refuses unobserved context-dependent Area bypass atomically",
	"[imagegraph][source_path_geometry]"
) {
	GeometryGraph g("pc.path_map_area", {{"path", GeometryLine()}});
	g.Doc.Nodes.push_back(
		{"area",
		 "pc.scatter_point_lattice",
		 "",
		 {},
		 {{"point_area", Area{1, 2, 3, 4, 0, 1}},
		  {"point_area_unit", EnumValue{0}},
		  {"subdivision", Vector2{0, 0}}}}
	);
	g.Doc.Links.push_back({"area", "point_area.bypass", "operation", "area"});
	g.CompileNow();
	g.Roundtrip();
	EvaluatedValue out;
	out.Data = std::string("retained");
	CHECK(EvaluateValue(g.Doc, g.Compiled, "value", {}, out, g.Error) == Status::UnsupportedExecution);
	INFO(g.Error.Message);
	CHECK(g.Error.NodeId == "operation");
	CHECK(g.Error.Port == "area");
	CHECK(g.Error.Message.find("surface-context getter") != std::string::npos);
	CHECK(std::get<std::string>(out.Data) == "retained");
}
