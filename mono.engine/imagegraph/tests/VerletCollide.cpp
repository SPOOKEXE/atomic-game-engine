#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
TEST_SUITE_ID("engine.imagegraph.verlet_collide")
using namespace engine::imagegraph;
namespace {
	Document ColliderGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"owner",
			 "pc.verlet_sim_inline",
			 "",
			 {},
			 {{"dimension_unit", EnumValue{0}},
			  {"dimension", Vector2{4, 4}},
			  {"gravity", Vector2{0, 10}},
			  {"substep", int64_t{1}},
			  {"wall", int64_t{0}}}},
			{"step", "pc.verlet_sim_step", "scope"},
			{"grid",
			 "pc.verlet_sim_mesh_grid",
			 "scope",
			 {},
			 {{"subdivision", Vector2{1, 1}}, {"area_unit", EnumValue{0}}, {"area", Area{2, 2, 2, 2}}}},
			{"tear",
			 "pc.verlet_sim_mesh_tear",
			 "scope",
			 {},
			 {{"chance", 1.0}, {"break_mesh", false}, {"source", EnumValue{0}}}},
			{"a",
			 "pc.verlet_sim_collide",
			 "scope",
			 {},
			 {{"area_unit", EnumValue{0}}, {"area", Area{0, 1, .25, .25}}, {"shape", EnumValue{0}}}},
			{"b",
			 "pc.verlet_sim_collide",
			 "scope",
			 {},
			 {{"area_unit", EnumValue{0}}, {"area", Area{4, 5, .25, .25}}, {"shape", EnumValue{0}}}}
		};
		Group group{"scope", "scope"};
		group.OwnerNodeId = "owner";
		document.Groups = {group};
		document.Links = {{"grid", "mesh", "tear", "mesh"}, {"tear", "mesh", "step", "mesh"}};
		document.Outputs = {{"mesh", "step", "mesh"}};
		return document;
	}
}
TEST_CASE("Inline source Collide schedules unconnected members and preserves cursor skips", "[imagegraph]") {
	const auto document = ColliderGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 4) <
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 5)
	);
	CHECK(
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 5) <
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 1)
	);
	SimulationEvaluationResult result;
	const auto status = EvaluateSimulation(document, plan, "mesh", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &mesh = *std::get<MeshValue2D>(std::get<EvaluatedValue>(result.Output).Data).Data;
	REQUIRE(mesh.Simulation.Points.size() == 4);
	CHECK(mesh.Simulation.Points[0].Position == Vector2{0, 0});
	CHECK(mesh.Simulation.Points[1].Position == Vector2{4, 1});
	CHECK(mesh.Simulation.Points[2].Position == Vector2{0, 5});
	CHECK(mesh.Simulation.Points[3].Position == Vector2{4, 5});
	// B would roll point3 back to (4,4) under an idealized all-collider loop; the
	// source skips B.
}

TEST_CASE("Verlet Collide semantic cycles and source cursor overrun refuse atomically", "[imagegraph]") {
	auto document = ColliderGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", {}, result, diagnostic) == Status::Ok);
	const auto previous = result.Replay;
	const auto previousValue = std::get<EvaluatedValue>(result.Output).Data;
	document.Links.push_back({"step", "mesh", "a", "mesh"});
	CHECK(Compile(document, plan, diagnostic) == Status::Cycle);
	document.Links.pop_back();
	for (int index = 0; index < 4; ++index)
		document.Nodes.push_back({"extra" + std::to_string(index), "pc.verlet_sim_collide", "scope"});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateSimulation(document, plan, "mesh", {}, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Message.find("collider cursor") != std::string::npos);
	CHECK(result.Replay == previous);
	CHECK(std::get<EvaluatedValue>(result.Output).Data == previousValue);
}
TEST_CASE(
	"Current collider dependency closure excludes nested owners and retained old registrations",
	"[imagegraph]"
) {
	auto document = ColliderGraph();
	document.Nodes[5].Values[1].Data = Area{4, 7, .25, .25};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", {}, result, diagnostic) == Status::Ok);
	const auto previousA =
		*std::find_if(result.Replay.Entries.begin(), result.Replay.Entries.end(), [](const auto &e) {
			return e.NodeId == "a";
		});
	document.Nodes.push_back({"nested", "pc.verlet_sim_inline", "scope"});
	Group nested{"inner", "inner"};
	nested.OwnerNodeId = "nested";
	nested.ParentId = "scope";
	document.Groups.push_back(nested);
	document.Nodes[4].GroupId = "inner";
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 1;
	request.SimulationReplay = &result.Replay;
	const auto status = EvaluateSimulation(document, plan, "mesh", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &mesh = *std::get<MeshValue2D>(std::get<EvaluatedValue>(result.Output).Data).Data;
	CHECK(mesh.Simulation.Points[3].Position == Vector2{4, 4});
	const auto found =
		std::find_if(result.Replay.Entries.begin(), result.Replay.Entries.end(), [](const auto &e) {
			return e.NodeId == "a";
		});
	REQUIRE(found != result.Replay.Entries.end());
	CHECK(*found == previousA);
}
