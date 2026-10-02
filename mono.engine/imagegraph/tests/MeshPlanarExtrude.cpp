#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.mesh_planar_extrude")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	MeshValue2D Triangle() {
		MeshValue2D value;
		auto &data = value.Data.emplace();
		data.Bounds = {0, 0, 2, 1};
		data.Triangles = {{0, 1, 2}};
		data.Simulation.Points.resize(3);
		data.Simulation.Points[0].Position = {0, 0};
		data.Simulation.Points[1].Position = {2, 0};
		data.Simulation.Points[2].Position = {0, 1};
		return value;
	}
}
TEST_CASE(
	"Planar extrusion preserves ordered faces taper and boundary closing sentinel",
	"[imagegraph][mesh_planar_extrude]"
) {
	auto run =
		RunNode("pc.3_d_mesh_extrude_mesh", {}, {{"mesh", Triangle()}, {"thickness", 2.0}, {"taper", .5}});
	REQUIRE(run.Ok);
	const auto &mesh = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	REQUIRE(mesh.Data);
	const auto &data = *mesh.Data;
	REQUIRE(data.Parts.size() == 3);
	REQUIRE(data.Parts[0].Vertices.size() == 3);
	REQUIRE(data.Parts[1].Vertices.size() == 3);
	REQUIRE(data.Parts[2].Vertices.size() == 24);
	REQUIRE(data.Edges.size() == 14);
	const auto &front = data.Parts[0].Vertices;
	REQUIRE(front[0].Position == Vector3{-.5, -.5, 1});
	REQUIRE(front[1].Position == Vector3{.5, -.5, 1});
	REQUIRE(front[2].Normal == Vector3{0, 0, 1});
	const auto &back = data.Parts[1].Vertices;
	REQUIRE(back[1].Position == Vector3{-1, 1, -1});
	REQUIRE(back[2].Position == Vector3{1, -1, -1});
	const auto &side = data.Parts[2].Vertices;
	REQUIRE(side[0].Normal == Vector3{0, 1, 0});
	REQUIRE(side[0].UV == Vector2{0, 0});
	REQUIRE(side[2].UV == Vector2{.25, 0});
	for (size_t i = 18; i < 24; ++i)
		REQUIRE(side[i].Normal == Vector3{});
	REQUIRE(side[18].Position == side[20].Position);
	REQUIRE(side[19].Position == side[23].Position);
	auto input = Triangle();
	input.Data->Bounds = {0, 0, 0, 1};
	auto invalid = RunNode("pc.3_d_mesh_extrude_mesh", {}, {{"mesh", input}});
	REQUIRE(invalid.Code == Status::InvalidValue);
}
TEST_CASE(
	"Smooth planar extrusion uses neighboring boundary points including source closing duplicate",
	"[imagegraph][mesh_planar_extrude]"
) {
	auto run = RunNode("pc.3_d_mesh_extrude_mesh", {}, {{"mesh", Triangle()}, {"smooth", true}});
	REQUIRE(run.Ok);
	const auto &side = std::get<MeshValue3D>(*run.OutputValue("mesh")).Data->Parts[2].Vertices;
	REQUIRE(side.front().Normal == Vector3{0, 1, 0});
	REQUIRE(side[18].Normal == Vector3{1, 0, 0});
	REQUIRE(side[20].Normal == Vector3{0, 1, 0});
}
