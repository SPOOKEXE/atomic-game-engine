#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.inline_scope")
TEST_DEPENDS("engine.imagegraph.simulation_replay")

namespace {
	using namespace engine::imagegraph;
	Document InlineScene() {
		Document document;
		document.FormatVersion = 9;
		document.Project = ProjectSettings{};
		document.Project->SurfaceWidth = 100;
		document.Project->SurfaceHeight = 100;
		document.Nodes = {
			{"step", "pc.verlet_sim_step", "inner", {}, {}},
			{"grid", "pc.verlet_sim_mesh_grid", "inner", {}, {{"subdivision", Vector2{1, 1}}}},
			{"inner-owner",
			 "pc.verlet_sim_inline",
			 "outer",
			 {},
			 {{"substep", int64_t{1}}, {"dimension_unit", EnumValue{1}}}},
			{"outer-owner", "pc.verlet_sim_inline", "", {}, {{"gravity", Vector2{0, 100}}}},
			{"gravity", "pc.vector2", "", {}, {{"x", 0.0}, {"y", .5}}},
			{"dimension", "pc.vector2", "", {}, {{"x", 2.0}, {"y", 3.0}}}
		};
		Group outer{"outer", "outer"};
		outer.OwnerNodeId = "outer-owner";
		Group inner{"inner", "inner"};
		inner.ParentId = "outer";
		inner.OwnerNodeId = "inner-owner";
		document.Groups = {outer, inner};
		document.Links = {
			{"grid", "mesh", "step", "mesh"},
			{"gravity", "vector", "inner-owner", "gravity"},
			{"dimension", "vector", "inner-owner", "dimension"}
		};
		document.Outputs = {{"mesh", "step", "mesh"}};
		document.Keyframes = {{"gravity", "y", 0, .5, "linear"}, {"gravity", "y", 1, 1.5, "linear"}};
		return document;
	}
}
TEST_CASE(
	"Nested inline scope schedules linked controls and retains source values for replay",
	"[imagegraph][inline]"
) {
	auto document = InlineScene();
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored.Groups == document.Groups);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	const auto stepRoute = std::find_if(
		plan.InlineOwnerDependencies.begin(), plan.InlineOwnerDependencies.end(), [](const auto &route) {
			return route.Consumer == 0;
		}
	);
	REQUIRE(stepRoute != plan.InlineOwnerDependencies.end());
	CHECK(stepRoute->Owner == 2);
	CHECK(
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 2) <
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 0)
	);
	SimulationEvaluationResult initial;
	auto status = EvaluateSimulation(restored, plan, "mesh", {}, initial, diagnostic);
	INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &mesh = std::get<MeshValue2D>(std::get<EvaluatedValue>(initial.Output).Data);
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Simulation.Points.size() == 4);
	CHECK(mesh.Data->Simulation.Points[0].Position.Y == Catch::Approx(.05));
	CHECK(mesh.Data->Simulation.Points[3].Position.X == Catch::Approx(2));
	CHECK(mesh.Data->Simulation.Points[3].Position.Y == Catch::Approx(3.05));
	EvaluationRequest nextRequest;
	nextRequest.Tick = 1;
	nextRequest.SimulationReplay = &initial.Replay;
	SimulationEvaluationResult next;
	REQUIRE(EvaluateSimulation(restored, plan, "mesh", nextRequest, next, diagnostic) == Status::Ok);
	const auto &nextMesh = std::get<MeshValue2D>(std::get<EvaluatedValue>(next.Output).Data);
	CHECK(nextMesh.Data->Simulation.Points[0].Position.Y == Catch::Approx(.25));
}
TEST_CASE(
	"Inline controls depending on their own child are rejected as an implicit cycle", "[imagegraph][inline]"
) {
	auto document = InlineScene();
	document.Nodes.back().GroupId = "inner";
	Plan plan;
	Diagnostic diagnostic;
	const auto status = Compile(document, plan, diagnostic);
	CHECK(status == Status::Cycle);
}

TEST_CASE(
	"Pixel Builder children read control snapshots before layer composition",
	"[imagegraph][inline][pixel_builder]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = 100;
	document.Project->SurfaceHeight = 80;
	document.Nodes = {
		{"child", "pc.pb_dimension", "builder", {}, {}},
		{"builder", "pc.pixel_builder", "", {}, {{"dimension", Vector2{.5, .25}}}}
	};
	Group group{"builder", "builder"};
	group.OwnerNodeId = "builder";
	document.Groups = {group};
	document.Outputs = {{"dimension", "child", "dimension"}};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(plan.InlineOwnerDependencies.size() == 1);
	CHECK(plan.InlineOwnerDependencies.front().ControlsOnly);
	EvaluatedValue result;
	auto status = EvaluateValue(document, plan, "dimension", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(std::get<Vector2>(result.Data) == Vector2{50, 20});

	SECTION("The virtual owner retains its own animated dimension") {
		document.Keyframes = {
			{"builder", "dimension", 0, Vector2{.5, .25}, "linear"},
			{"builder", "dimension", 1, Vector2{.25, .5}, "linear"}
		};
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.Tick = 1;
		REQUIRE(EvaluateValue(document, plan, "dimension", request, result, diagnostic) == Status::Ok);
		CHECK(std::get<Vector2>(result.Data) == Vector2{25, 40});
	}

	SECTION("Linked animated dimensions remain physical pixels") {
		document.Nodes.push_back({"size", "pc.vector2", "", {}, {{"x", 2.0}, {"y", 3.0}}});
		document.Links = {{"size", "vector", "builder", "dimension"}};
		document.Keyframes = {{"size", "x", 0, 2.0, "linear"}, {"size", "x", 1, 4.0, "linear"}};
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		REQUIRE(plan.InlineControlDependencies.size() == 1);
		EvaluationRequest request;
		request.Tick = 1;
		REQUIRE(EvaluateValue(document, plan, "dimension", request, result, diagnostic) == Status::Ok);
		CHECK(std::get<Vector2>(result.Data) == Vector2{4, 3});
	}
	SECTION("An owner's dimension cannot depend on its own child") {
		document.Links = {{"child", "dimension", "builder", "dimension"}};
		CHECK(Compile(document, plan, diagnostic) == Status::Cycle);
	}
}

TEST_CASE(
	"Pixel Builder layer commands and their surfaces precede the owner", "[imagegraph][inline][pixel_builder]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"builder",
		 "pc.pixel_builder",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}}},
		{"layer", "pc.pb_output", "builder", {}, {}},
		{"surface",
		 "image.solid",
		 "builder",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 0, 0, 255}}}}
	};
	Group group{"builder", "builder"};
	group.OwnerNodeId = "builder";
	document.Groups = {group};
	document.Links = {{"surface", "image", "layer", "surface"}};
	document.Outputs = {{"image", "builder", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(plan.InlineControlDependencies.size() == 2);
	CHECK(
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 1) <
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 0)
	);
	CHECK(
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 2) <
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 1)
	);
	Image image;
	const auto evaluated = Evaluate(document, plan, "image", image, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	CHECK(image.Width == 2);
	CHECK(image.Height == 2);
	REQUIRE(image.Pixels.size() == 16);
	for (size_t offset = 0; offset < image.Pixels.size(); offset += 4) {
		CHECK(image.Pixels[offset] == 255);
		CHECK(image.Pixels[offset + 1] == 0);
		CHECK(image.Pixels[offset + 2] == 0);
		CHECK(image.Pixels[offset + 3] == 255);
	}
	SECTION("Inactive layer still schedules but publishes an empty canvas") {
		document.Nodes[1].Values = {{"active", false}};
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		REQUIRE(Evaluate(document, plan, "image", image, diagnostic) == Status::Ok);
		CHECK(std::all_of(image.Pixels.begin(), image.Pixels.end(), [](uint8_t value) {
			return value == 0;
		}));
	}
}
