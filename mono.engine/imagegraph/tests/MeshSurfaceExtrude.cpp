#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.mesh_surface_extrude")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Solid() {
		Image image;
		image.Width = 1;
		image.Height = 1;
		image.Pixels = {255, 255, 255, 255};
		return image;
	}
}
TEST_CASE(
	"Source surface extrusion retains duplicate caps and per-pixel edge stream",
	"[imagegraph][mesh_surface_extrude]"
) {
	auto image = Solid();
	auto run = RunNode("pc.3_d_mesh_extrude", {{"front_surface", &image}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = *std::get<MeshValue3D>(*run.OutputValue("mesh")).Data;
	REQUIRE(mesh.Parts.size() == 3);
	REQUIRE(mesh.Parts[0].Vertices.size() == 12);
	REQUIRE(mesh.Parts[1].Vertices.size() == 12);
	REQUIRE(mesh.Parts[2].Vertices.size() == 24);
	REQUIRE(mesh.Edges.size() == 12);
	const auto &front = mesh.Parts[0].Vertices;
	REQUIRE(front[0].Position == Vector3{.5, .5, .5});
	REQUIRE(front[0].Normal == Vector3{0, 0, 1});
	REQUIRE(front[0].UV == Vector2{1, 0});
	for (size_t i = 0; i < 6; ++i)
		REQUIRE(front[i] == front[i + 6]);
	REQUIRE(mesh.Parts[2].Vertices[0].Normal == Vector3{0, 1, 0});
	REQUIRE(mesh.Materials[0].Get().Surface == image);
	image.Pixels[3] = 0;
	auto empty = RunNode("pc.3_d_mesh_extrude", {{"front_surface", &image}});
	REQUIRE(empty.Ok);
	const auto &clear = *std::get<MeshValue3D>(*empty.OutputValue("mesh")).Data;
	REQUIRE(clear.Parts[0].Vertices.empty());
	REQUIRE(clear.Edges.empty());
}
TEST_CASE(
	"Source voxel extrusion keeps source face-part assignment and voxel dimensions",
	"[imagegraph][mesh_surface_extrude]"
) {
	auto image = Solid();
	auto run = RunNode(
		"pc.3_d_mesh_extrude", {{"front_surface", &image}}, {{"voxel_scale", true}, {"voxel_size", 2.0}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = *std::get<MeshValue3D>(*run.OutputValue("mesh")).Data;
	REQUIRE(mesh.Parts[0].Vertices.size() == 18);
	REQUIRE(mesh.Parts[1].Vertices.size() == 6);
	REQUIRE(mesh.Parts[2].Vertices.size() == 12);
	REQUIRE(mesh.Parts[0].Vertices[0].Position == Vector3{1, 1, 1});
	REQUIRE(mesh.Parts[0].Vertices[6].Normal == Vector3{1, 0, 0});
	REQUIRE(mesh.Parts[0].Vertices[12].Normal == Vector3{0, 1, 0});
}
TEST_CASE(
	"Surface height uses source shader arithmetic followed by R16 float storage",
	"[imagegraph][mesh_surface_extrude]"
) {
	auto image = Solid(), height = Solid();
	height.Pixels = {128, 128, 128, 255};
	auto run = RunNode(
		"pc.3_d_mesh_extrude",
		{{"front_surface", &image}, {"front_height", &height}},
		{{"double_side", true}, {"front_height_level", Vector2{0, 1}}, {"back_height_level", Vector2{0, .5}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = *std::get<MeshValue3D>(*run.OutputValue("mesh")).Data;
	const float p = 128 / 255.f;
	const double expected = DecodeHalf(EncodeHalf(((p + p) + p) / 3.f)) * .5;
	REQUIRE(mesh.Parts[0].Vertices[0].Position.Z == expected);
	REQUIRE(
		mesh.Parts[1].Vertices[0].Position.Z == -DecodeHalf(EncodeHalf((((p + p) + p) / 3.f) / .5f)) * .5
	);
	REQUIRE(mesh.Parts[2].Vertices.size() == 48);
	auto invalid = RunNode(
		"pc.3_d_mesh_extrude",
		{{"front_surface", &image}, {"front_height", &height}},
		{{"front_height_level", Vector2{1, 1}}}
	);
	REQUIRE(invalid.Code == Status::InvalidValue);
}
