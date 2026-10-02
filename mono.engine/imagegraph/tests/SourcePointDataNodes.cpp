#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/StatefulTemporalCone.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
TEST_SUITE_ID("engine.imagegraph.source_point_data")
using namespace engine::imagegraph;
namespace {
	struct PointGraph {
		Document Doc;
		Plan Compiled;
		Diagnostic Failure;
		EvaluationRequest Request;
		PointGraph(
			std::string_view type,
			std::vector<AuthoredValue> controls = {},
			std::string_view output = "points"
		) {
			Doc.FormatVersion = 9;
			Doc.Nodes = {{"point", std::string(type), "", {}, std::move(controls)}};
			Doc.Outputs = {{"out", "point", std::string(output)}};
			CompileNow();
		}
		void CompileNow() {
			const auto status = Compile(Doc, Compiled, Failure);
			INFO(Failure.Message);
			REQUIRE(status == Status::Ok);
		}
		ArrayValue Run() {
			EvaluatedValue out;
			const auto status = EvaluateValue(Doc, Compiled, "out", Request, out, Failure);
			INFO(Failure.Message);
			REQUIRE(status == Status::Ok);
			REQUIRE(std::holds_alternative<ArrayValue>(out.Data));
			return std::get<ArrayValue>(out.Data);
		}
	};
	std::vector<ElementValue> Points(std::initializer_list<Vector2> points) {
		std::vector<ElementValue> out;
		for (const auto &p : points)
			out.emplace_back(p);
		return out;
	}
	ArrayValue Line(std::initializer_list<Vector2> points) {
		return {ValueType::Vector2, Points(points)};
	}
}
TEST_CASE(
	"Fibonacci defaults use project center and sequential golden-angle stepping",
	"[imagegraph][source_point_data]"
) {
	PointGraph g("pc.scatter_point_fibonacci");
	const auto out = g.Run();
	REQUIRE(out.Elements.size() == 16);
	CHECK(std::get<Vector2>(out.Elements[0]) == Vector2{16, 16});
	const auto second = std::get<Vector2>(out.Elements[1]);
	const double angle = (1 + std::sqrt(5.)) / 2 * 2 * std::numbers::pi;
	CHECK(second.X == Catch::Approx(16 + std::cos(angle)));
	CHECK(second.Y == Catch::Approx(16 - std::sin(angle)));
	Document restored;
	REQUIRE(Read(Write(g.Doc), restored, g.Failure) == Status::Ok);
	g.Doc = std::move(restored);
	g.CompileNow();
	CHECK(g.Run() == out);
}
TEST_CASE(
	"Fibonacci signed step anisotropic scale and rotation preserve source Y direction",
	"[imagegraph][source_point_data]"
) {
	PointGraph g(
		"pc.scatter_point_fibonacci",
		{{"amount", int64_t{3}},
		 {"center", Vector2{4, 8}},
		 {"center_unit", EnumValue{0}},
		 {"rotation", 90.},
		 {"rotation_2", .25},
		 {"scale", Vector2{2, 3}},
		 {"step", -2.}}
	);
	const auto a = g.Run();
	REQUIRE(a.Elements.size() == 3);
	CHECK(std::get<Vector2>(a.Elements[0]) == Vector2{4, 8});
	CHECK(std::get<Vector2>(a.Elements[1]).X == Catch::Approx(8));
	CHECK(std::get<Vector2>(a.Elements[1]).Y == Catch::Approx(8));
	CHECK(std::get<Vector2>(a.Elements[2]).X == Catch::Approx(4));
	CHECK(std::get<Vector2>(a.Elements[2]).Y == Catch::Approx(-4));
	g.Doc.Nodes[0].Values.push_back({"seed", 71.});
	g.CompileNow();
	CHECK(g.Run() == a);
}
TEST_CASE("Lattice defaults include row-major endpoints and center", "[imagegraph][source_point_data]") {
	PointGraph g("pc.scatter_point_lattice");
	CHECK(
		g.Run().Elements ==
		Points({{0, 0}, {16, 0}, {32, 0}, {0, 16}, {16, 16}, {32, 16}, {0, 32}, {16, 32}, {32, 32}})
	);
}
TEST_CASE(
	"Lattice rounds subdivisions ties to even and handles empty axes", "[imagegraph][source_point_data]"
) {
	PointGraph g(
		"pc.scatter_point_lattice",
		{{"point_area", Area{10, 20, 2, 4, 0, 0}},
		 {"point_area_unit", EnumValue{0}},
		 {"subdivision", Vector2{.5, 1.5}}}
	);
	CHECK(g.Run().Elements == Points({{10, 16}, {10, 20}, {10, 24}}));
	g.Doc.Nodes[0].Values[2].Data = Vector2{-1, 1e100};
	g.CompileNow();
	CHECK(g.Run().Elements.empty());
}
TEST_CASE(
	"Lattice padding and two-point area modes are converted before units", "[imagegraph][source_point_data]"
) {
	PointGraph g(
		"pc.scatter_point_lattice",
		{{"point_area", Area{.25, .125, .125, .25, 0, 1}}, {"subdivision", Vector2{1, 1}}}
	);
	CHECK(g.Run().Elements == Points({{4, 4}, {24, 4}, {4, 24}, {24, 24}}));
	g.Doc.Nodes[0].Values[0].Data = Area{1, 2, 5, 8, 0, 2};
	g.Doc.Nodes[0].Values.push_back({"point_area_unit", EnumValue{0}});
	g.CompileNow();
	CHECK(g.Run().Elements == Points({{1, 2}, {5, 2}, {1, 8}, {5, 8}}));
}
TEST_CASE(
	"Point generators admit processor arrays and reject oversized output atomically",
	"[imagegraph][source_point_data]"
) {
	PointGraph g(
		"pc.scatter_point_fibonacci",
		{{"amount", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{2}}}}, {"rotation_2", 0.}}
	);
	const auto a = g.Run();
	REQUIRE(a.Nested.size() == 2);
	CHECK(a.Nested[0] == Points({{16, 16}}));
	CHECK(a.Nested[1] == Points({{16, 16}, {17, 16}}));
	g.Doc.Nodes[0].Values[0].Data = int64_t{4097};
	g.CompileNow();
	EvaluatedValue out;
	out.Data = 42.;
	const auto before = out;
	CHECK(EvaluateValue(g.Doc, g.Compiled, "out", g.Request, out, g.Failure) == Status::LimitExceeded);
	CHECK(out == before);
}
TEST_CASE(
	"Segment Filter splits runs uses strict spread and optional opposite direction",
	"[imagegraph][source_point_data]"
) {
	PointGraph g(
		"pc.segment_filter",
		{{"segment", Line({{0, 0}, {1, 0}, {1, 1}, {0, 1}})}, {"spread", 10.}},
		"segments"
	);
	CHECK(
		g.Run().Nested ==
		std::vector<std::vector<ElementValue>>{Points({{0, 0}, {1, 0}}), Points({{1, 1}, {0, 1}})}
	);
	g.Doc.Nodes[0].Values.push_back({"both_side", false});
	g.CompileNow();
	CHECK(g.Run().Nested == std::vector<std::vector<ElementValue>>{Points({{0, 0}, {1, 0}})});
	g.Doc.Nodes[0].Values[1].Data = 0.;
	g.CompileNow();
	CHECK(g.Run().Nested.empty());
}
TEST_CASE(
	"Segment Filter accepts multiple paths and drops singleton paths", "[imagegraph][source_point_data]"
) {
	ArrayValue paths{ValueType::Vector2, {}};
	paths.Nested = {Points({{0, 0}, {1, 0}}), {}, Points({{4, 4}}), Points({{2, 2}, {3, 2}})};
	PointGraph g("pc.segment_filter", {{"segment", paths}}, "segments");
	CHECK(g.Run().Nested == std::vector<std::vector<ElementValue>>{paths.Nested[0], paths.Nested[3]});
}
TEST_CASE(
	"Segment Filter retains latest output on empty input and retries budget failure atomically",
	"[imagegraph][source_point_data]"
) {
	PointGraph g("pc.segment_filter", {}, "segments");
	g.Doc.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
	g.Doc.Nodes[0].SourceInputExpressions = {
		{"segment",
		 "if(Project.frame==1){[[0,0],[1,0]]}else{if(Project.frame==3){[[5,5],[8,5]]}else{[]}}",
		 true}
	};
	g.CompileNow();
	const std::array<std::string, 1> ids{"out"};
	const auto cone = AnalyzeStatefulTemporalCone(g.Doc, g.Compiled, ids);
	REQUIRE(cone.Valid);
	CHECK(cone.DataProcessors == 1);
	StatefulEvaluationResult state;
	g.Request.DataReplay = &state.Data;
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "out", g.Request, state, g.Failure) == Status::Ok);
	CHECK(
		std::get<ArrayValue>(std::get<EvaluatedValue>(state.Output).Data).Nested ==
		std::vector<std::vector<ElementValue>>{{}}
	);
	g.Request.Tick = 1;
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "out", g.Request, state, g.Failure) == Status::Ok);
	const auto retained = std::get<EvaluatedValue>(state.Output).Data;
	g.Request.Tick = 2;
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "out", g.Request, state, g.Failure) == Status::Ok);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == retained);
	g.Request.Tick = 3;
	const auto history = state.Data;
	CHECK(
		EvaluateStateful(g.Doc, g.Compiled, "out", g.Request, state, g.Failure, 1) == Status::LimitExceeded
	);
	CHECK(state.Data == history);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == retained);
	REQUIRE(EvaluateStateful(g.Doc, g.Compiled, "out", g.Request, state, g.Failure) == Status::Ok);
	CHECK(
		std::get<ArrayValue>(std::get<EvaluatedValue>(state.Output).Data).Nested ==
		std::vector<std::vector<ElementValue>>{Points({{5, 5}, {8, 5}})}
	);
	REQUIRE(state.Data.Entries.size() == 1);
	CHECK(state.Data.Entries[0].Values.size() == 1);
}
