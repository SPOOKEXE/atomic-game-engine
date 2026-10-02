#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.mesh_terrain")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

TEST_CASE(
	"Terrain retains source grid ordering and blue brightness coefficient", "[imagegraph][mesh_terrain]"
) {
	Image blue{1, 1, {0, 0, 255, 255}, 0};
	const auto run = RunNode("pc.3_d_mesh_terrain", {{"height_map", &blue}}, {{"subdivision", int64_t(1)}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Parts.size() == 1);
	const auto &vertices = mesh.Data->Parts[0].Vertices;
	REQUIRE(vertices.size() == 6);
	CHECK((vertices[0].Position == Vector3{-.5, -.5, .224}));
	CHECK((vertices[1].Position == Vector3{.5, .5, .224}));
	CHECK((vertices[2].Position == Vector3{.5, -.5, .224}));
	CHECK((vertices[4].Position == Vector3{-.5, .5, .224}));
	for (const auto &vertex : vertices)
		CHECK((vertex.Normal == Vector3{0, 0, 1}));
	REQUIRE(mesh.Data->Edges.size() == 4);
	CHECK(mesh.Data->Edges[0].From == vertices[0].Position);
	CHECK(mesh.Data->Edges[0].To == vertices[4].Position);
	CHECK(mesh.Data->Edges[1].From == vertices[4].Position);
	CHECK(mesh.Data->Edges[1].To == vertices[1].Position);
}

TEST_CASE(
	"Terrain height rows preserve source flat normals and zero filled tail", "[imagegraph][mesh_terrain]"
) {
	ArrayValue heights;
	heights.ElementType = ValueType::Scalar;
	heights.Nested = {{0.0, 1.0}, {2.0, 3.0}};
	const auto run = RunNode(
		"pc.3_d_mesh_terrain",
		{},
		{{"subdivision", int64_t(1)}, {"input_type", EnumValue{1}}, {"height_array", heights}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &vertices = std::get<MeshValue3D>(*run.OutputValue("mesh")).Data->Parts[0].Vertices;
	CHECK(vertices[0].Position.Z == 0);
	CHECK(vertices[1].Position.Z == 3);
	CHECK(vertices[2].Position.Z == 1);
	CHECK(vertices[4].Position.Z == 2);
	CHECK(std::abs(vertices[0].Normal.X + 1 / std::sqrt(6.0)) < 1e-12);
	CHECK(std::abs(vertices[0].Normal.Y + 2 / std::sqrt(6.0)) < 1e-12);
	CHECK(std::abs(vertices[0].Normal.Z - 1 / std::sqrt(6.0)) < 1e-12);
	heights.Nested = {{4.0}};
	const auto partial = RunNode(
		"pc.3_d_mesh_terrain",
		{},
		{{"subdivision", int64_t(1)}, {"input_type", EnumValue{1}}, {"height_array", heights}}
	);
	REQUIRE(partial.Ok);
	const auto &tail = std::get<MeshValue3D>(*partial.OutputValue("mesh")).Data->Parts[0].Vertices;
	CHECK(tail[0].Position.Z == 4);
	CHECK(tail[1].Position.Z == 0);
	CHECK(tail[2].Position.Z == 0);
}

TEST_CASE("Terrain rejects excess subdivision before geometry allocation", "[imagegraph][mesh_terrain]") {
	const auto run = RunNode("pc.3_d_mesh_terrain", {}, {{"subdivision", int64_t(1000000)}});
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::LimitExceeded);
	CHECK(run.Port == "subdivision");
}
