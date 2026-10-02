#include "../src/SimulationAliases.hpp"
#include "../src/SourceVerletPathCodec.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <iomanip>
#include <limits>
#include <sstream>

TEST_SUITE_ID("engine.imagegraph.simulation_replay")
using namespace engine::imagegraph;
TEST_CASE("source verlet grid preserves x-fast points and vertical-first edge chains", "[imagegraph]") {
	const auto run = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_grid",
		{},
		{{"subdivision", Vector2{1, 1}},
		 {"quad", true},
		 {"area_unit", EnumValue{0}},
		 {"area", Area{2, 3, 2, 3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto *value = run.OutputValue("mesh");
	REQUIRE(value);
	const auto &mesh = std::get<MeshValue2D>(*value);
	REQUIRE(mesh.Data);
	CHECK(mesh.Data->Simulation.Points.size() == 4);
	CHECK(mesh.Data->Simulation.Points[0].Position == Vector2{0, 0});
	CHECK(mesh.Data->Simulation.Points[1].Position == Vector2{4, 0});
	CHECK(mesh.Data->Simulation.Points[2].Position == Vector2{0, 6});
	CHECK(mesh.Data->Simulation.Points[3].Position == Vector2{4, 6});
	CHECK(mesh.Data->Simulation.Points[3].UV == Vector2{1, 1});
	const auto &edges = mesh.Data->Simulation.Edges;
	REQUIRE(edges.size() == 4);
	CHECK(edges[0].First == 0);
	CHECK(edges[0].Second == 2);
	CHECK(edges[1].First == 1);
	CHECK(edges[1].Second == 3);
	CHECK(edges[2].First == 0);
	CHECK(edges[2].Second == 1);
	CHECK(edges[3].First == 2);
	CHECK(edges[3].Second == 3);
	CHECK(mesh.Data->Triangles == std::vector<std::array<uint32_t, 3>>{{0, 1, 2}, {2, 1, 3}});
	CHECK(mesh.Data->Quads == std::vector<std::array<uint32_t, 2>>{{0, 1}});
}
TEST_CASE("source verlet grid units and edge neighbors survive native graph evaluation", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"grid", "pc.verlet_sim_mesh_grid", "", {}, {{"subdivision", Vector2{1, 2}}}}};
	document.Outputs = {{"mesh", "grid", "mesh"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	const auto evaluated = EvaluateValue(document, plan, "mesh", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	const auto &mesh = std::get<MeshValue2D>(result.Data);
	REQUIRE(mesh.Data);
	CHECK(mesh.Data->Bounds == std::array<double, 4>{0, 0, 32, 32});
	CHECK(mesh.Data->Simulation.Edges[0].NextEdge == 1);
	CHECK(mesh.Data->Simulation.Edges[1].PreviousEdge == 0);
	CHECK(mesh.Data->Simulation.Edges[2].PreviousEdge == -1);
	CHECK(mesh.Data->Simulation.Edges[2].NextEdge == 3);
}
TEST_CASE(
	"source verlet grid clamps nonpositive subdivisions and rejects oversized allocations", "[imagegraph]"
) {
	const auto clamped =
		imagegraph_test::RunNode("pc.verlet_sim_mesh_grid", {}, {{"subdivision", Vector2{0, -5}}});
	REQUIRE(clamped.Ok);
	CHECK(std::get<MeshValue2D>(*clamped.OutputValue("mesh")).Data->Simulation.Points.size() == 4);
	const auto large =
		imagegraph_test::RunNode("pc.verlet_sim_mesh_grid", {}, {{"subdivision", Vector2{4096, 4096}}});
	CHECK_FALSE(large.Ok);
	CHECK(large.Code == Status::LimitExceeded);
	const auto fractional =
		imagegraph_test::RunNode("pc.verlet_sim_mesh_grid", {}, {{"subdivision", Vector2{1.5, 2}}});
	CHECK_FALSE(fractional.Ok);
	CHECK(fractional.Code == Status::UnsupportedExecution);
}
TEST_CASE("simulation ledgers validate identities and all retained point fields", "[imagegraph]") {
	const std::array points{VerletPoint{{0, 0}, {0, 0}}};
	VerletReplayState state;
	Diagnostic diagnostic;
	REQUIRE(
		ResetVerletReplay(points, {}, 0, 3, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
	);
	SimulationReplayState ledger{{{"node", state}}};
	REQUIRE(ValidateSimulationReplay(ledger, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	CHECK(RetainedSimulationReplayBytes(ledger) >= sizeof(SimulationReplayEntry) + sizeof(VerletPoint));
	CHECK(ValidateSimulationReplay(ledger, 0, diagnostic) == Status::LimitExceeded);
	ledger.Entries.push_back(ledger.Entries[0]);
	CHECK(
		ValidateSimulationReplay(ledger, Limits::MaximumEvaluationBytes, diagnostic) == Status::DuplicateId
	);
	ledger.Entries.pop_back();
	ledger.Entries[0].State.Mesh.Points[0].UV.X = std::numeric_limits<double>::infinity();
	CHECK(
		ValidateSimulationReplay(ledger, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue
	);
	CHECK(diagnostic.NodeId == "node");
}

namespace {
	Document SteppingGraph(bool image = false) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"grid", "pc.verlet_sim_mesh_grid", "", {}, {{"subdivision", Vector2{1, 1}}}},
			{"step", "image.verlet_simple", "", {}, {{"substep", int64_t{1}}, {"gravity", Vector2{0, 1}}}}
		};
		document.Links = {{"grid", "mesh", "step", "mesh"}};
		document.Outputs = {{"mesh", "step", "mesh"}};
		if (image) {
			Node array{"array", "pc.array", "", {}, {}};
			array.DynamicInputs = {{"input_0", ValueType::Any, std::nullopt}};
			document.Nodes.push_back(std::move(array));
			document.Nodes.push_back({"length", "pc.array_length", "", {}, {}});
			document.Nodes.push_back({"solid", "pc.solid", "", {}, {{"dimension_unit", EnumValue{0}}}});
			document.Links.push_back({"step", "mesh", "array", "input_0"});
			document.Links.push_back({"array", "array", "length", "array"});
			document.Links.push_back({"length", "size", "solid", "dimension"});
			document.Outputs.push_back({"image", "solid", "surface_out"});
		}
		return document;
	}
}
TEST_CASE("simulation graph returns internal fixed tick state transactionally", "[imagegraph]") {
	const auto document = SteppingGraph();
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationRequest request;
	request.SimulationAuthoringRevision = 11;
	SimulationEvaluationResult result;
	request.ReuseSimulationFrame = true;
	CHECK(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::InvalidValue);
	CHECK(result.Replay.Entries.empty());
	request.ReuseSimulationFrame = false;
	const auto first = EvaluateSimulation(document, plan, "mesh", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(first == Status::Ok);
	REQUIRE(result.Replay.Entries.size() == 1);
	CHECK(result.Replay.Entries[0].NodeId == "grid");
	CHECK(result.Replay.Entries[0].State.Tick == 0);
	CHECK(result.Replay.Entries[0].State.Mesh.Points[0].Position.Y == Catch::Approx(1));
	request.SimulationReplay = &result.Replay;
	request.Tick = 1;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::Ok);
	CHECK(result.Replay.Entries[0].State.Mesh.Points[0].Position.Y == Catch::Approx(3));
	const auto before = result.Replay;
	const auto pose = std::get<MeshValue2D>(std::get<EvaluatedValue>(result.Output).Data);
	CHECK_FALSE(request.ReuseSimulationFrame);
	CHECK(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::InvalidValue);
	CHECK(result.Replay == before);
	request.ReuseSimulationFrame = true;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::Ok);
	CHECK(result.Replay == before);
	CHECK(std::get<MeshValue2D>(std::get<EvaluatedValue>(result.Output).Data) == pose);
	request.SimulationAuthoringRevision = 12;
	CHECK(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::InvalidValue);
	CHECK(result.Replay == before);
	request.SimulationAuthoringRevision = 11;
	request.Tick = 2;
	CHECK(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::InvalidValue);
	CHECK(result.Replay == before);
	request.ReuseSimulationFrame = false;
	request.Tick = 3;
	CHECK(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::InvalidValue);
	CHECK(result.Replay == before);
	request.Tick = 2;
	request.SimulationAuthoringRevision = 12;
	CHECK(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::InvalidValue);
	CHECK(result.Replay == before);
}
TEST_CASE("image selection captures simulation state from its complete dependency closure", "[imagegraph]") {
	const auto document = SteppingGraph(true);
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationRequest request;
	SimulationEvaluationResult result;
	const auto evaluated = EvaluateSimulation(document, plan, "image", request, result, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	REQUIRE(std::holds_alternative<Image>(result.Output));
	CHECK(std::get<Image>(result.Output).Width == 1);
	CHECK(std::get<Image>(result.Output).Height == 1);
	REQUIRE(result.Replay.Entries.size() == 1);
	CHECK(result.Replay.Entries[0].NodeId == "grid");
	const auto before = result.Replay;
	request.SimulationReplay = &result.Replay;
	request.Tick = 1;
	CHECK(
		EvaluateSimulation(document, plan, "image", request, result, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(result.Replay == before);
}
TEST_CASE("simulation graph preserves unvisited previous nodes", "[imagegraph]") {
	auto document = SteppingGraph();
	document.Nodes.push_back({"unvisited", "pc.verlet_sim_mesh_grid"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::array points{VerletPoint{{4, 5}, {4, 5}}};
	VerletReplayState unvisited;
	REQUIRE(
		ResetVerletReplay(points, {}, 17, 99, Limits::MaximumEvaluationBytes, unvisited, diagnostic) ==
		Status::Ok
	);
	const SimulationReplayState previous{{{"unvisited", unvisited}}};
	EvaluationRequest request;
	request.SimulationReplay = &previous;
	SimulationEvaluationResult result;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::Ok);
	REQUIRE(result.Replay.Entries.size() == 2);
	CHECK(result.Replay.Entries[0] == previous.Entries[0]);
	CHECK(result.Replay.Entries[1].NodeId == "grid");
}

TEST_CASE("source pin mesh area preserves override add and source unpin intersection", "[imagegraph]") {
	const auto generated =
		imagegraph_test::RunNode("pc.verlet_sim_mesh_grid", {}, {{"subdivision", Vector2{1, 1}}});
	REQUIRE(generated.Ok);
	auto mesh = std::get<MeshValue2D>(*generated.OutputValue("mesh"));
	mesh.Data->Simulation.Points[1].Pin = true;
	const Area left{0, 16, 0, 16};
	const auto overridden = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_pin", {}, {{"mesh", mesh}, {"area", left}, {"area_unit", EnumValue{0}}}
	);
	INFO(overridden.Message);
	REQUIRE(overridden.Ok);
	const auto &points = std::get<MeshValue2D>(*overridden.OutputValue("mesh")).Data->Simulation.Points;
	CHECK(points[0].Pin);
	CHECK_FALSE(points[1].Pin);
	CHECK(points[2].Pin);
	CHECK_FALSE(points[3].Pin);
	const auto added = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_pin",
		{},
		{{"mesh", mesh}, {"area", left}, {"area_unit", EnumValue{0}}, {"mode", EnumValue{1}}}
	);
	REQUIRE(added.Ok);
	CHECK(std::get<MeshValue2D>(*added.OutputValue("mesh")).Data->Simulation.Points[1].Pin);
	mesh.Data->Simulation.Points[0].Pin = true;
	const auto intersected = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_pin",
		{},
		{{"mesh", mesh}, {"area", left}, {"area_unit", EnumValue{0}}, {"mode", EnumValue{2}}}
	);
	REQUIRE(intersected.Ok);
	CHECK(std::get<MeshValue2D>(*intersected.OutputValue("mesh")).Data->Simulation.Points[0].Pin);
	CHECK_FALSE(std::get<MeshValue2D>(*intersected.OutputValue("mesh")).Data->Simulation.Points[1].Pin);
}
TEST_CASE("source pin mesh edge loop resets other pins and follows edge neighbors", "[imagegraph]") {
	const auto generated =
		imagegraph_test::RunNode("pc.verlet_sim_mesh_grid", {}, {{"subdivision", Vector2{1, 2}}});
	REQUIRE(generated.Ok);
	auto mesh = std::get<MeshValue2D>(*generated.OutputValue("mesh"));
	mesh.Data->Simulation.Points[1].Pin = true;
	const auto run = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_pin", {}, {{"mesh", mesh}, {"source", EnumValue{2}}, {"edge_index", int64_t{0}}}
	);
	REQUIRE(run.Ok);
	const auto &points = std::get<MeshValue2D>(*run.OutputValue("mesh")).Data->Simulation.Points;
	CHECK(points[0].Pin);
	CHECK(points[2].Pin);
	CHECK(points[4].Pin);
	CHECK_FALSE(points[1].Pin);
	mesh.Data->Simulation.Edges[0].NextEdge = 0;
	const auto cyclic = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_pin", {}, {{"mesh", mesh}, {"source", EnumValue{2}}, {"edge_index", int64_t{0}}}
	);
	CHECK_FALSE(cyclic.Ok);
	CHECK(cyclic.Code == Status::InvalidValue);
}
TEST_CASE("source pin mask ignores alpha and uses ties to even coordinate rounding", "[imagegraph]") {
	const auto generated =
		imagegraph_test::RunNode("pc.verlet_sim_mesh_grid", {}, {{"subdivision", Vector2{1, 1}}});
	REQUIRE(generated.Ok);
	auto mesh = std::get<MeshValue2D>(*generated.OutputValue("mesh"));
	mesh.Data->Simulation.Points[0].Position = {.5, 0};
	mesh.Data->Simulation.Points[1].Position = {1.5, 0};
	const auto mask = imagegraph_test::MakeImage(3, 1, {255, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 255});
	const auto run = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_pin", {{"surface", &mask}}, {{"mesh", mesh}, {"source", EnumValue{1}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &points = std::get<MeshValue2D>(*run.OutputValue("mesh")).Data->Simulation.Points;
	CHECK(points[0].Pin);
	CHECK_FALSE(points[1].Pin);
}

TEST_CASE("mesh origin aliases receive downstream pin changes on the next fixed tick", "[imagegraph]") {
	auto document = SteppingGraph();
	document.Nodes.push_back(
		{"pin", "pc.verlet_sim_mesh_pin", "", {}, {{"area_unit", EnumValue{0}}, {"area", Area{16, 1, 16, 0}}}}
	);
	document.Links.push_back({"step", "mesh", "pin", "mesh"});
	document.Outputs.push_back({"pinned", "pin", "mesh"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	EvaluationRequest request;
	REQUIRE(EvaluateSimulation(document, plan, "pinned", request, result, diagnostic) == Status::Ok);
	REQUIRE(result.Replay.Entries.size() == 1);
	CHECK(result.Replay.Entries[0].NodeId == "grid");
	CHECK(result.Replay.Entries[0].State.Mesh.Points[0].Pin);
	request.SimulationReplay = &result.Replay;
	request.Tick = 1;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::Ok);
	CHECK(result.Replay.Entries[0].State.Mesh.Points[0].Position.Y == Catch::Approx(1));
}
TEST_CASE("constructor origin preserves initial mesh topology when controls animate later", "[imagegraph]") {
	auto document = SteppingGraph();
	document.Timeline = TimelineSettings{2, 0, 1, "loop", 60};
	document.Keyframes = {
		{"grid", "subdivision", 0, Vector2{1, 1}}, {"grid", "subdivision", 1, Vector2{2, 2}}
	};
	// grug same revision source mesh constructor evaluates only the first frame controls.
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	EvaluationRequest request;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::Ok);
	const auto prior = result.Replay;
	request.SimulationReplay = &result.Replay;
	request.Tick = 1;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::Ok);
	CHECK(result.Replay.Entries[0].Topology == prior.Entries[0].Topology);
	CHECK(result.Replay.Entries[0].State.Mesh.Points.size() == 4);
}
TEST_CASE("source push mesh samples rectangular falloff and preserves pinned histories", "[imagegraph]") {
	MeshValue2D input;
	auto &mesh = input.Data.emplace();
	mesh.Verlet = true;
	mesh.Simulation.Points = {
		VerletPoint{{0, 0}, {0, 0}},
		VerletPoint{{1, 0}, {1, 0}},
		VerletPoint{{2, 0}, {2, 0}},
		VerletPoint{{0, 0}, {0, 0}, {}, 0, true}
	};
	Curve linear;
	linear.Header = {0, 1, 0, 0, 1, 0};
	linear.Anchors = {{0, 0, 0, 0, 1.0 / 3, 1.0 / 3}, {-1.0 / 3, -1.0 / 3, 1, 1, 0, 0}};
	const auto result = imagegraph_test::RunNode(
		"pc.verlet_sim_force",
		{},
		{{"mesh", input},
		 {"area_unit", EnumValue{0}},
		 {"area", Area{0, 0, 1, 1}},
		 {"falloff", 1.0},
		 {"falloff_curve", linear},
		 {"push", Vector2{10, 20}},
		 {"strength", 2.0}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	const auto &points = std::get<MeshValue2D>(*result.OutputValue("mesh")).Data->Simulation.Points;
	CHECK(points[0].Position == Vector2{2, 4});
	CHECK(points[1].Position == Vector2{2, 2});
	CHECK(points[2].Position == Vector2{2, 0});
	CHECK(points[3].Position == Vector2{0, 0});
	CHECK(points[0].Previous == Vector2{0, 0});
	CHECK(mesh.Simulation.Points[0].Position == Vector2{0, 0});
}
TEST_CASE("simulation constructor row identity preserves independent mesh origins", "[imagegraph]") {
	SimulationReplayState replay;
	VerletReplayState state;
	Diagnostic diagnostic;
	const std::array points{VerletPoint{{0, 0}, {0, 0}}};
	REQUIRE(
		ResetVerletReplay(points, {}, 0, 0, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
	);
	replay.Entries = {{"grid", state}, {"grid", state}};
	replay.Entries[1].ProcessorRow = 1;
	REQUIRE(ValidateSimulationReplay(replay, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	replay.Entries[1].ProcessorRow = 0;
	CHECK(
		ValidateSimulationReplay(replay, Limits::MaximumEvaluationBytes, diagnostic) == Status::DuplicateId
	);
}
TEST_CASE(
	"source wind applies strip falloff in degree direction without moving pinned points", "[imagegraph]"
) {
	MeshValue2D input;
	auto &mesh = input.Data.emplace();
	mesh.Verlet = true;
	mesh.Simulation.Points = {
		VerletPoint{{0, 0}, {0, 0}},
		VerletPoint{{0, 4}, {0, 4}},
		VerletPoint{{0, 8}, {0, 8}},
		VerletPoint{{0, 0}, {0, 0}, {}, 0, true}
	};
	const auto result = imagegraph_test::RunNode(
		"pc.verlet_sim_wind",
		{},
		{{"mesh", input},
		 {"center_unit", EnumValue{0}},
		 {"center", Vector2{0, 0}},
		 {"direction", 0.0},
		 {"width", 8.0},
		 {"falloff", 4.0},
		 {"strength", 10.0}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	const auto &points = std::get<MeshValue2D>(*result.OutputValue("mesh")).Data->Simulation.Points;
	CHECK(points[0].Position == Vector2{1, 0});
	CHECK(points[1].Position.X == Catch::Approx(.5));
	CHECK(points[1].Position.Y == 4);
	CHECK(points[2].Position == Vector2{0, 8});
	CHECK(points[3].Position == Vector2{0, 0});
}
TEST_CASE("source tear uses HTML5 seeded draws before inactive edge and area checks", "[imagegraph]") {
	MeshValue2D input;
	auto &mesh = input.Data.emplace();
	mesh.Verlet = true;
	mesh.Simulation.Points = {VerletPoint{{0, 0}, {0, 0}}, VerletPoint{{2, 0}, {2, 0}}};
	mesh.Simulation.Edges.assign(4, VerletEdge{0, 1, 2});
	mesh.Simulation.Edges[0].Active = false;
	const auto result = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_tear",
		{},
		{{"mesh", input},
		 {"seed", 12345.0},
		 {"source", EnumValue{0}},
		 {"chance", .5},
		 {"break_mesh", true},
		 {"area_unit", EnumValue{0}},
		 {"area", Area{1, 0, 1, 1}}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	const auto &output = *std::get<MeshValue2D>(*result.OutputValue("mesh")).Data;
	CHECK_FALSE(output.Simulation.Edges[0].Active);
	CHECK_FALSE(output.Simulation.Edges[1].Active);
	CHECK_FALSE(output.Simulation.Edges[2].Active);
	CHECK(output.Simulation.Edges[3].Active);
	CHECK_FALSE(output.Simulation.Points[0].Active);
	CHECK_FALSE(output.Simulation.Points[1].Active);
	CHECK(mesh.Simulation.Edges[1].Active);
}
TEST_CASE("source bloat uses immutable origin coordinates and sampled curve map", "[imagegraph]") {
	MeshValue2D input;
	auto &mesh = input.Data.emplace();
	mesh.Verlet = true;
	mesh.Simulation.Points = {
		VerletPoint{{3, 0}, {3, 0}}, VerletPoint{{2, 0}, {2, 0}}, VerletPoint{{3, 0}, {3, 0}, {}, 0, true}
	};
	mesh.Simulation.Points[0].Original = {1, 0};
	mesh.Simulation.Points[2].Original = {1, 0};
	const auto run = [&](bool origin, double falloff) {
		return imagegraph_test::RunNode(
			"pc.verlet_sim_bloat",
			{},
			{{"mesh", input},
			 {"area_unit", EnumValue{0}},
			 {"area", Area{0, 0, 2, 2}},
			 {"falloff", falloff},
			 {"strength", 2.0},
			 {"use_origin", origin}}
		);
	};
	const auto original = run(true, 2);
	INFO(original.Message);
	REQUIRE(original.Ok);
	const auto &points = std::get<MeshValue2D>(*original.OutputValue("mesh")).Data->Simulation.Points;
	CHECK(points[0].Position == Vector2{5, 0});
	CHECK(points[0].Original == Vector2{1, 0});
	CHECK(points[1].Position == Vector2{3, 0});
	CHECK(points[2].Position == Vector2{3, 0});
	const auto current = run(false, 2);
	REQUIRE(current.Ok);
	CHECK(
		std::get<MeshValue2D>(*current.OutputValue("mesh")).Data->Simulation.Points[0].Position ==
		Vector2{3, 0}
	);
	const auto zero = run(true, 0);
	REQUIRE(zero.Ok);
	CHECK(
		std::get<MeshValue2D>(*zero.OutputValue("mesh")).Data->Simulation.Points[1].Position == Vector2{2, 0}
	);
}
TEST_CASE("source mesh conversion resets constructor histories and remaps UV coordinates", "[imagegraph]") {
	MeshValue2D input;
	auto &mesh = input.Data.emplace();
	mesh.Simulation.Points = {VerletPoint{{2, 3}, {-1, -1}}, VerletPoint{{6, 3}, {-1, -1}}};
	mesh.Simulation.Edges = {VerletEdge{0, 1, 999}};
	const auto result = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh",
		{},
		{{"mesh", input},
		 {"tension", .25},
		 {"drag", .3},
		 {"stiffness", .4},
		 {"remap", true},
		 {"uv_map_unit", EnumValue{0}},
		 {"uv_map", Area{4, 3, 2, 3}}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	const auto &output = *std::get<MeshValue2D>(*result.OutputValue("mesh")).Data;
	CHECK(output.Verlet);
	CHECK(output.Simulation.Points[0].Previous == Vector2{2, 3});
	CHECK(output.Simulation.Points[0].BeforePrevious == Vector2{0, 0});
	CHECK(output.Simulation.Points[0].Original == Vector2{2, 3});
	CHECK(output.Simulation.Points[0].UV == Vector2{0, .5});
	CHECK(output.Simulation.Points[1].UV == Vector2{1, .5});
	CHECK(output.Simulation.Edges[0].Distance == 4);
	CHECK(output.Simulation.Edges[0].Flexibility == .75);
	CHECK(output.Simulation.Edges[0].AngularDrag == .4);
	CHECK_FALSE(mesh.Verlet);
	CHECK(mesh.Simulation.Edges[0].Distance == 999);
}
TEST_CASE("source drag retains per node control history and later moves only pinned points", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"grid",
		 "pc.verlet_sim_mesh_grid",
		 "",
		 {},
		 {{"subdivision", Vector2{1, 1}}, {"area_unit", EnumValue{0}}, {"area", Area{.5, .5, .5, .5}}}},
		{"pin",
		 "pc.verlet_sim_mesh_pin",
		 "",
		 {},
		 {{"area_unit", EnumValue{0}}, {"area", Area{0, 0, 0, 0}}, {"mode", EnumValue{1}}}},
		{"drag", "pc.verlet_sim_drag", "", {}, {{"drag_unit", EnumValue{0}}}}
	};
	document.Links = {{"grid", "mesh", "pin", "mesh"}, {"pin", "mesh", "drag", "mesh"}};
	document.Keyframes = {
		{"drag", "drag", 0, Vector2{3, 0}, "linear"}, {"drag", "drag", 1, Vector2{5, 0}, "linear"}
	};
	document.Timeline = {2, 0, 1, "loop", 60};
	document.Outputs = {{"mesh", "drag", "mesh"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	EvaluationRequest request;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::Ok);
	REQUIRE(result.Replay.Entries.size() == 2);
	const auto &first =
		std::get<MeshValue2D>(std::get<EvaluatedValue>(result.Output).Data).Data->Simulation.Points;
	CHECK(first[0].Position == Vector2{3, 0});
	CHECK(first[0].Previous == Vector2{3, 0});
	CHECK(first[1].Position == Vector2{4, 0});
	request.SimulationReplay = &result.Replay;
	request.Tick = 1;
	REQUIRE(EvaluateSimulation(document, plan, "mesh", request, result, diagnostic) == Status::Ok);
	const auto &next =
		std::get<MeshValue2D>(std::get<EvaluatedValue>(result.Output).Data).Data->Simulation.Points;
	CHECK(next[0].Position == Vector2{5, 0});
	CHECK(next[0].Previous == Vector2{3, 0});
	CHECK(next[1].Position == Vector2{4, 0});
	REQUIRE(result.Replay.Entries[1].Drag);
	CHECK(result.Replay.Entries[1].Drag->Move == Vector2{2, 0});
	CHECK(result.Replay.Entries[1].Drag->Previous == Vector2{5, 0});
}
TEST_CASE("source disk preserves repeated radial edges and sparse source quad slots", "[imagegraph]") {
	const auto result = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_disk",
		{},
		{{"subdivision", Vector2{4, 1}},
		 {"area_unit", EnumValue{0}},
		 {"area", Area{0, 0, 2, 3}},
		 {"quad", true},
		 {"cartesian", true}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	const auto &mesh = *std::get<MeshValue2D>(*result.OutputValue("mesh")).Data;
	CHECK(mesh.Simulation.Points.size() == 9);
	CHECK(mesh.Simulation.Points[0].UV == Vector2{.5, .5});
	CHECK(mesh.Simulation.Points[1].Position == Vector2{1, 0});
	CHECK(mesh.Simulation.Points[1].UV == Vector2{.75, .5});
	REQUIRE(mesh.Simulation.Edges.size() == 18);
	CHECK(mesh.Simulation.Edges[0].First == 0);
	CHECK(mesh.Simulation.Edges[0].Second == 1);
	CHECK(mesh.Simulation.Edges[8].First == 0);
	CHECK(mesh.Simulation.Edges[8].Second == 1);
	CHECK(mesh.Simulation.Edges[0].NextEdge == 1);
	CHECK(mesh.Simulation.Edges[8].NextEdge == 9);
	CHECK(mesh.Simulation.Edges[13].First == 4);
	CHECK(mesh.Simulation.Edges[13].Second == 1);
	CHECK(mesh.Simulation.Edges[13].NextEdge == -1);
	REQUIRE(mesh.Triangles.size() == 12);
	CHECK(mesh.Triangles[8] == std::array<uint32_t, 3>{0, 1, 2});
	CHECK(mesh.Quads.empty());
	REQUIRE(mesh.SparseQuads.size() == 11);
	REQUIRE(mesh.SparseQuads[0]);
	CHECK(*mesh.SparseQuads[0] == std::array<uint32_t, 2>{0, 1});
	CHECK_FALSE(mesh.SparseQuads[1]);
	REQUIRE(mesh.SparseQuads[10]);
	CHECK(*mesh.SparseQuads[10] == std::array<uint32_t, 2>{10, 11});
	const auto odd = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_disk", {}, {{"subdivision", Vector2{3, 1}}, {"quad", true}}
	);
	CHECK_FALSE(odd.Ok);
	CHECK(odd.Code == Status::InvalidValue);
}
TEST_CASE(
	"late simulation aliases replace selected union outputs after downstream mutation", "[imagegraph]"
) {
	auto document = SteppingGraph();
	document.Nodes.push_back(
		{"pin", "pc.verlet_sim_mesh_pin", "", {}, {{"area_unit", EnumValue{0}}, {"area", Area{0, 1, 0, 0}}}}
	);
	document.Links.push_back({"step", "mesh", "pin", "mesh"});
	document.Outputs.push_back({"pinned", "pin", "mesh"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulOutputEvaluationResult result;
	const std::array<std::string, 2> outputs{"mesh", "pinned"};
	const auto status = EvaluateStatefulOutputs(document, plan, outputs, {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Outputs.size() == 2);
	for (const auto &output : result.Outputs) {
		const auto &mesh = std::get<MeshValue2D>(std::get<EvaluatedValue>(output.Output).Data);
		CHECK(mesh.Data->Simulation.Points[0].Pin);
		CHECK(mesh.Data->Simulation.Points[0].Position == Vector2{0, 1});
	}
}
TEST_CASE(
	"simulation alias output replacement preserves nested selector values and admission", "[imagegraph]"
) {
	MeshValue2D mesh;
	auto &data = mesh.Data.emplace();
	data.Verlet = true;
	data.OriginNodeId = "grid";
	data.OriginProcessorRow = 5;
	data.Simulation.Points = {VerletPoint{{1, 0}, {1, 0}}};
	StructValue fields;
	fields.Data.emplace().Fields.push_back({"mesh", mesh});
	ArraySelectorValue selector;
	selector.Data.emplace().Values = {ValueType::Struct, {fields}};
	selector.Data->CumulativeWeights = {1};
	selector.Data->TotalWeight = 1;
	Value output = selector;
	SimulationReplayState replay;
	SimulationReplayEntry entry;
	entry.NodeId = "grid";
	entry.ProcessorRow = 5;
	entry.State.Initialized = true;
	entry.State.Mesh.Points = {VerletPoint{{8, 0}, {8, 0}}};
	replay.Entries.push_back(entry);
	Diagnostic diagnostic;
	const auto original = output;
	const uint64_t oldBytes = detail::RetainedPayloadBytes(output);
	{
		detail::EvaluationBudget budget(oldBytes);
		auto charge = budget.Reserve(oldBytes);
		REQUIRE(charge);
		CHECK(
			detail::ResolveSimulationValueAliases(output, replay, budget, *charge, diagnostic) ==
			Status::LimitExceeded
		);
		CHECK(output == original);
		CHECK(charge->Bytes() == oldBytes);
	}
	{
		detail::EvaluationBudget budget(1 << 20);
		auto charge = budget.Reserve(oldBytes);
		REQUIRE(charge);
		REQUIRE(
			detail::ResolveSimulationValueAliases(output, replay, budget, *charge, diagnostic) == Status::Ok
		);
		const auto &array = std::get<ArraySelectorValue>(output).Data->Values;
		const auto &record = std::get<StructValue>(array.Elements[0]);
		const auto &replacement = std::get<MeshValue2D>(record.Data->Fields[0].second);
		CHECK(replacement.Data->Simulation.Points[0].Position == Vector2{8, 0});
		CHECK(replacement.Data->OriginProcessorRow == 5);
		CHECK(charge->Bytes() == detail::RetainedPayloadBytes(output));
	}
}
TEST_CASE("source pleat uses immutable coordinates and distinct area and loop rules", "[imagegraph]") {
	MeshValue2D input;
	auto &data = input.Data.emplace();
	data.Verlet = true;
	for (const Vector2 original : std::array{Vector2{.25, .5}, Vector2{.75, .5}, Vector2{1, .5}}) {
		VerletPoint point;
		point.Position = {99, 99};
		point.Original = original;
		point.Pin = true;
		point.Previous = {7, 8};
		data.Simulation.Points.push_back(point);
	}
	data.Simulation.Edges = {{0, 1}, {1, 2}};
	data.Simulation.Edges[0].NextEdge = 1;
	data.Simulation.Edges[1].PreviousEdge = 0;
	const auto area = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_pleat",
		{},
		{{"mesh", input},
		 {"source", EnumValue{0}},
		 {"area_unit", EnumValue{0}},
		 {"amount", 2.0},
		 {"strength", 1.0},
		 {"offset", 2.0}}
	);
	INFO(area.Message);
	REQUIRE(area.Ok);
	const auto &areaPoints = std::get<MeshValue2D>(*area.OutputValue("mesh")).Data->Simulation.Points;
	CHECK(areaPoints[0].Position == Vector2{.25, 2.5});
	CHECK(areaPoints[1].Position == Vector2{.75, -1.5});
	CHECK(areaPoints[0].Previous == Vector2{7, 8});
	input.Data->Simulation.Points[1].Pin = false;
	const auto loop = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_pleat",
		{},
		{{"mesh", input},
		 {"source", EnumValue{1}},
		 {"edge_index", int64_t{1}},
		 {"amount", 2.0},
		 {"strength", 1.0}}
	);
	INFO(loop.Message);
	REQUIRE(loop.Ok);
	const auto &loopPoints = std::get<MeshValue2D>(*loop.OutputValue("mesh")).Data->Simulation.Points;
	CHECK(loopPoints[0].Position == Vector2{1.25, .5});
	CHECK(loopPoints[1].Position == Vector2{1.75, .5});
	CHECK(loopPoints[2].Position == Vector2{2, .5});
	CHECK(loopPoints[1].Previous == Vector2{7, 8});
	input.Data->Simulation.Edges[0].PreviousEdge = 1;
	const auto cycle = imagegraph_test::RunNode("pc.verlet_sim_mesh_pleat", {}, {{"mesh", input}});
	CHECK_FALSE(cycle.Ok);
	CHECK(cycle.Code == Status::InvalidValue);
}
TEST_CASE("source cache mix interpolates drawing coordinates without changing physics", "[imagegraph]") {
	MeshValue2D input;
	auto &data = input.Data.emplace();
	data.Verlet = true;
	data.Simulation.Points = {VerletPoint{{2, 4}, {1, 3}}, VerletPoint{{8, 12}, {7, 11}}};
	StructValue cache;
	ArrayValue positions;
	positions.ElementType = ValueType::Scalar;
	positions.Nested = {{10.0, 20.0}};
	cache.Data.emplace().Fields.emplace_back("points", positions);
	const auto mixed = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_cache_lerp", {}, {{"mesh", input}, {"cache_mesh", cache}, {"amount", .25}}
	);
	INFO(mixed.Message);
	REQUIRE(mixed.Ok);
	const auto &points = std::get<MeshValue2D>(*mixed.OutputValue("mesh")).Data->Simulation.Points;
	CHECK(points[0].DrawPosition == Vector2{4, 8});
	CHECK(points[0].Position == Vector2{2, 4});
	CHECK(points[0].Previous == Vector2{1, 3});
	CHECK_FALSE(points[1].DrawPosition);
	positions.Nested.clear();
	positions.Items = {SourceArrayItem{
		std::vector<SourceArrayItem>{SourceArrayItem{ElementValue{10.0}}, SourceArrayItem{ElementValue{20.0}}}
	}};
	cache.Data->Fields[0].second = positions;
	const auto general = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_cache_lerp", {}, {{"mesh", input}, {"cache_mesh", cache}}
	);
	REQUIRE(general.Ok);
	CHECK(
		std::get<MeshValue2D>(*general.OutputValue("mesh")).Data->Simulation.Points[0].DrawPosition ==
		Vector2{6, 12}
	);
	positions.Items[0] = SourceArrayItem{ElementValue{1.0}};
	cache.Data->Fields[0].second = positions;
	const auto malformed = imagegraph_test::RunNode(
		"pc.verlet_sim_mesh_cache_lerp", {}, {{"mesh", input}, {"cache_mesh", cache}}
	);
	CHECK_FALSE(malformed.Ok);
	CHECK(malformed.Code == Status::InvalidValue);
}
TEST_CASE("source mesh autocache persists captured points as caller owned state", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"grid",
		 "pc.verlet_sim_mesh_grid",
		 "",
		 {},
		 {{"subdivision", Vector2{1, 1}}, {"area_unit", EnumValue{0}}}},
		{"cache", "pc.verlet_sim_mesh_cache", "", {}, {{"autocache", true}, {"frame", int64_t{0}}}}
	};
	document.Links = {{"grid", "mesh", "cache", "mesh"}};
	document.Outputs = {{"cached", "cache", "cached_data"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	EvaluationRequest request;
	const auto captured = EvaluateSimulation(document, plan, "cached", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(captured == Status::Ok);
	REQUIRE(result.Replay.Entries.size() == 2);
	const auto saved = std::get<StructValue>(std::get<EvaluatedValue>(result.Output).Data);
	REQUIRE(saved.Data);
	const auto &positions = std::get<ArrayValue>(saved.Data->Fields[0].second);
	REQUIRE(positions.Nested.size() == 4);
	CHECK(std::get<double>(positions.Nested[0][0]) == 0);
	CHECK(std::get<double>(positions.Nested[3][1]) == 1);
	request.Tick = 1;
	request.SimulationReplay = &result.Replay;
	REQUIRE(EvaluateSimulation(document, plan, "cached", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<StructValue>(std::get<EvaluatedValue>(result.Output).Data) == saved);
	auto malformed = result.Replay;
	for (auto &entry : malformed.Entries)
		if (entry.Cache) entry.Cache->front().X = std::numeric_limits<double>::infinity();
	CHECK(
		ValidateSimulationReplay(malformed, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::InvalidValue
	);
}
TEST_CASE("manual cache actions validate durable IDs and capture at exact subframes", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"grid", "pc.verlet_sim_mesh_grid", "", {}, {{"subdivision", Vector2{1, 1}}}},
		{"cache", "pc.verlet_sim_mesh_cache", "", {}, {}}
	};
	document.Links = {{"grid", "mesh", "cache", "mesh"}};
	document.Outputs = {{"cached", "cache", "cached_data"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	EvaluationRequest request;
	const std::array<std::string_view, 1> action{"cache"};
	request.SimulationCacheCaptures = action;
	request.Subframe = .5;
	INFO(diagnostic.Message);
	REQUIRE(EvaluateSimulation(document, plan, "cached", request, result, diagnostic) == Status::Ok);
	CHECK(std::holds_alternative<StructValue>(std::get<EvaluatedValue>(result.Output).Data));
	const auto prior = result.Replay;
	const std::array<std::string_view, 2> duplicate{"cache", "cache"};
	request.SimulationCacheCaptures = duplicate;
	CHECK(EvaluateSimulation(document, plan, "cached", request, result, diagnostic) == Status::DuplicateId);
	CHECK(result.Replay == prior);
	const std::array<std::string_view, 1> unknown{"missing"};
	request.SimulationCacheCaptures = unknown;
	CHECK(EvaluateSimulation(document, plan, "cached", request, result, diagnostic) == Status::InvalidValue);
	const std::array<std::string_view, 1> wrongKind{"grid"};
	request.SimulationCacheCaptures = wrongKind;
	CHECK(EvaluateSimulation(document, plan, "cached", request, result, diagnostic) == Status::InvalidValue);
}
TEST_CASE(
	"source Verlet path samples clamp endpoint and preserve temporary drag assignment", "[imagegraph]"
) {
	Path2D path;
	path.Anchors = {{{0, 0, 0, 0, 0, 0}}, {{10, 0, 0, 0, 0, 0}}};
	const auto run = imagegraph_test::RunNode(
		"pc.verlet_sim_path", {}, {{"path", path}, {"samples", int64_t{2}}, {"drag", .75}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = std::get<MeshValue2D>(*run.OutputValue("mesh"));
	REQUIRE(mesh.Data);
	const auto &points = mesh.Data->Simulation.Points;
	REQUIRE(points.size() == 3);
	REQUIRE(mesh.Data->Simulation.Edges.size() == 2);
	CHECK(points[0].Position == Vector2{0, 0});
	CHECK(points[1].Position == Vector2{5, 0});
	CHECK(points[2].Position.X == Catch::Approx(9.99));
	CHECK(points[1].Previous == points[1].Position);
	CHECK(points[1].Original == points[1].Position);
	CHECK(points[1].BeforePrevious == Vector2{});
	CHECK(points[1].Drag == 0);
	CHECK(mesh.Data->Simulation.Edges[0].NextEdge == -1);
	CHECK(mesh.Data->Triangles.empty());
}
TEST_CASE(
	"source bridge preserves shared rows missing cross strip constraints and sparse quads", "[imagegraph]"
) {
	Document document;
	document.FormatVersion = 9;
	Node bridge{
		"bridge",
		"pc.verlet_sim_mesh_bridge",
		"",
		{},
		{{"subdivision", Vector2{1, 1}}, {"quad", true}, {"pin_first", true}}
	};
	for (int index = 0; index < 3; ++index) {
		Path2D path;
		path.Anchors = {{{0, double(index * 10), 0, 0, 0, 0}}, {{10, double(index * 10), 0, 0, 0, 0}}};
		bridge.DynamicInputs.push_back({"path_" + std::to_string(index), ValueType::Path2D, path});
	}
	document.Nodes.push_back(std::move(bridge));
	document.Outputs = {{"mesh", "bridge", "mesh"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluatedValue result;
	const auto evaluated = EvaluateValue(document, plan, "mesh", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	const auto &data = *std::get<MeshValue2D>(result.Data).Data;
	REQUIRE(data.Simulation.Points.size() == 6);
	REQUIRE(data.Simulation.Edges.size() == 5);
	CHECK(data.Simulation.Points[0].Position == Vector2{0, 0});
	CHECK(data.Simulation.Points[2].Position == Vector2{0, 10});
	CHECK(data.Simulation.Points[4].Position == Vector2{0, 20});
	CHECK(data.Simulation.Points[5].Position.X == Catch::Approx(9.99));
	CHECK(data.Simulation.Points[0].Pin);
	CHECK_FALSE(data.Simulation.Points[2].Pin);
	CHECK(data.Simulation.Points[2].UV == Vector2{0, .5});
	CHECK(data.Simulation.Edges[4].First == 4);
	CHECK(data.Simulation.Edges[4].Second == 5);
	CHECK(data.Triangles.size() == 4);
	REQUIRE(data.SparseQuads.size() == 3);
	CHECK(data.SparseQuads[0] == std::array<uint32_t, 2>{0, 1});
	CHECK_FALSE(data.SparseQuads[1]);
	CHECK(data.SparseQuads[2] == std::array<uint32_t, 2>{2, 3});
}
TEST_CASE(
	"Verlet CPU raster profile covers shared edges once and preserves source edge maps", "[imagegraph]"
) {
	MeshValue2D mesh;
	auto &data = mesh.Data.emplace();
	data.Verlet = true;
	for (const Vector2 position : std::array{Vector2{0, 0}, Vector2{2, 0}, Vector2{0, 2}, Vector2{2, 2}}) {
		VerletPoint point;
		point.Position = point.Previous = point.Original = position;
		point.UV = {position.X / 2, position.Y / 2};
		point.SourceIndex = uint32_t(data.Simulation.Points.size());
		data.Simulation.Points.push_back(point);
	}
	data.Simulation.Edges = {{0, 2}, {1, 3}, {0, 1}, {2, 3}};
	data.Triangles = {{0, 1, 2}, {2, 1, 3}};
	const Image texture = imagegraph_test::MakeImage(1, 1, {255, 0, 0, 128});
	const auto run = [&](const MeshValue2D &input, double trim = 1, bool inverse = false, bool gpu = false) {
		const auto *entry = FindCatalogueEntry("pc.verlet_sim_render");
		REQUIRE(entry);
		Node node{"render", "pc.verlet_sim_render", "", {}, {}};
		EvaluationRequest request;
		request.RequireSourceGpuRasterCoverage = gpu;
		engine::imagegraph::detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.InlineOwnerId = "inline";
		context.InlineOwnerType = "pc.verlet_sim_inline";
		const Value dimension = Vector2{2, 2}, unit = EnumValue{0};
		const std::array<std::pair<std::string_view, const Value *>, 2> owner{
			{{"dimension", &dimension}, {"dimension_unit", &unit}}
		};
		context.InlineOwnerValues = owner;
		context.Values = {
			{"mesh", input},
			{"step", false},
			{"type", EnumValue{0}},
			{"trim", trim},
			{"invert_order", inverse},
			{"interpolate", EnumValue{1}}
		};
		context.Images = {{"texture", &texture}};
		imagegraph_test::NodeRun result;
		result.Ok = engine::imagegraph::detail::FindExecutor(node.Type)(context);
		result.Code = context.FailureCode;
		result.Message = context.FailureMessage;
		result.Images = std::move(context.OutputImages);
		return result;
	};
	const auto full = run(mesh);
	INFO(full.Message);
	REQUIRE(full.Ok);
	CHECK(
		full.Output().Pixels ==
		std::vector<uint8_t>{128, 0, 0, 64, 128, 0, 0, 64, 128, 0, 0, 64, 128, 0, 0, 64}
	);
	const auto first = run(mesh, .25);
	REQUIRE(first.Ok);
	CHECK(first.Output().Pixels == std::vector<uint8_t>{128, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
	const auto last = run(mesh, .25, true);
	REQUIRE(last.Ok);
	CHECK(
		last.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 0, 128, 0, 0, 64, 128, 0, 0, 64, 128, 0, 0, 64}
	);
	mesh.Data->Simulation.Edges[0].Active = false;
	const auto torn = run(mesh);
	REQUIRE(torn.Ok);
	CHECK(torn.Output().Pixels == last.Output().Pixels);
	for (auto &point : mesh.Data->Simulation.Points)
		point.SourceIndex = 0;
	mesh.Data->Simulation.Edges.back().Active = false;
	const auto aliased = run(mesh);
	REQUIRE(aliased.Ok);
	CHECK(aliased.Output().Pixels == std::vector<uint8_t>(16, 0));
	const auto required = run(mesh, 1, false, true);
	CHECK_FALSE(required.Ok);
	CHECK(required.Code == Status::UnsupportedExecution);
	const auto excessive = run(mesh, 2);
	CHECK_FALSE(excessive.Ok);
	CHECK(excessive.Code == Status::InvalidValue);
}
TEST_CASE("source Mesh To Path keeps disconnected edges cached lengths and live endpoints", "[imagegraph]") {
	MeshValue2D mesh;
	auto &data = mesh.Data.emplace();
	data.Verlet = true;
	data.OriginNodeId = "origin";
	for (const auto position : std::array{Vector2{0, 0}, Vector2{2, 0}, Vector2{10, 5}, Vector2{14, 5}}) {
		VerletPoint point;
		point.Position = position;
		data.Simulation.Points.push_back(point);
	}
	data.Simulation.Edges = {{0, 1}, {2, 3}};
	data.Simulation.Edges[0].Active = false;
	const auto run = imagegraph_test::RunNode("pc.verlet_sim_to_path", {}, {{"mesh", mesh}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &path = std::get<Path2D>(*run.OutputValue("path"));
	REQUIRE(path.SourceOperation);
	CHECK(path.SourceOperation->CachedLengths == std::vector<std::optional<double>>{std::nullopt, 4.0});
	CHECK(
		engine::imagegraph::detail::SampleSourceVerletPath(*path.SourceOperation, .5) ==
		std::array<double, 3>{12, 5, 1}
	);
	auto moved = path;
	moved.SourceOperation->Mesh->Simulation.Points[2].Position = {20, 5};
	moved.SourceOperation->Mesh->Simulation.Points[3].Position = {40, 5};
	CHECK(
		engine::imagegraph::detail::SampleSourceVerletPath(*moved.SourceOperation, .5) ==
		std::array<double, 3>{30, 5, 1}
	);
	CHECK(
		engine::imagegraph::detail::SampleSourceVerletPath(*moved.SourceOperation, 1) ==
		std::array<double, 3>{39.8, 5, 1}
	);
	CHECK(
		engine::imagegraph::detail::DistanceSourceVerletPath(*moved.SourceOperation, -1) ==
		std::array<double, 3>{15, 5, 1}
	);
	std::stringstream encoded;
	encoded << std::setprecision(17);
	engine::imagegraph::detail::WriteSourceVerletPath(encoded, *moved.SourceOperation);
	SourcePathData2D decoded;
	decoded.Kind = SourcePathOperationKind::VerletMesh;
	uint64_t bytes = 0;
	REQUIRE(engine::imagegraph::detail::ReadSourceVerletPath(encoded, decoded, [&](uint64_t count) {
		bytes += count;
		return bytes <= Limits::MaximumEvaluationBytes;
	}));
	CHECK(decoded == *moved.SourceOperation);
	std::stringstream rejected(encoded.str());
	SourcePathData2D ignored;
	CHECK_FALSE(engine::imagegraph::detail::ReadSourceVerletPath(rejected, ignored, [](uint64_t) {
		return false;
	}));
}
TEST_CASE("same frame Cache button observes current mesh without repeating prior affectors", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"grid",
		 "pc.verlet_sim_mesh_grid",
		 "",
		 {},
		 {{"subdivision", Vector2{1, 1}}, {"area_unit", EnumValue{0}}}},
		{"push",
		 "pc.verlet_sim_force",
		 "",
		 {},
		 {{"push", Vector2{10, 0}},
		  {"strength", 1.0},
		  {"area_unit", EnumValue{0}},
		  {"area", Area{0, 0, 100, 100}},
		  {"falloff", 0.0}}},
		{"cache", "pc.verlet_sim_mesh_cache", "", {}, {}}
	};
	document.Links = {{"grid", "mesh", "push", "mesh"}, {"push", "mesh", "cache", "mesh"}};
	document.Outputs = {{"pose", "push", "mesh"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	EvaluationRequest request;
	REQUIRE(EvaluateSimulation(document, plan, "pose", request, result, diagnostic) == Status::Ok);
	const auto pose = std::get<MeshValue2D>(std::get<EvaluatedValue>(result.Output).Data);
	const std::array<std::string_view, 1> action{"cache"};
	request.SimulationCacheCaptures = action;
	request.SimulationReplay = &result.Replay;
	const auto captured = EvaluateSimulation(document, plan, "pose", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(captured == Status::Ok);
	CHECK(std::get<MeshValue2D>(std::get<EvaluatedValue>(result.Output).Data) == pose);
	const SimulationReplayEntry *cache = nullptr;
	for (const auto &entry : result.Replay.Entries)
		if (entry.NodeId == "cache") cache = &entry;
	REQUIRE(cache);
	REQUIRE(cache->Cache);
	CHECK(cache->Cache->front() == pose.Data->Simulation.Points.front().Position);
}
