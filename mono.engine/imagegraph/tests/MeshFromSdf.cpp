#include "NodeHarness.hpp"

#include <engine/imagegraph/SourceSdf.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.mesh_from_sdf")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	SdfValue Sphere(Vector3 center, double radius = .2) {
		SdfValue value;
		auto &data = value.Data.emplace();
		SourceSdfShape shape;
		shape.Identity = "sphere";
		shape.Shape = 200;
		shape.Radius = radius;
		shape.Position = center;
		data.Shapes.push_back(shape);
		data.Operations.push_back({0, 0});
		return value;
	}
}
TEST_CASE(
	"Source voxel conversion preserves occupancy-to-mesh axis permutation", "[imagegraph][mesh_from_sdf]"
) {
	auto run = RunNode("pc.rm_to_voxel", {}, {{"sdf_object", Sphere({-.5, -.5, -.5})}, {"resolution", 2.0}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = *std::get<MeshValue3D>(*run.OutputValue("mesh")).Data;
	REQUIRE(mesh.Parts.size() == 1);
	REQUIRE(mesh.Parts[0].Vertices.size() == 36);
	REQUIRE(mesh.Edges.empty());
	const auto &vertices = mesh.Parts[0].Vertices;
	REQUIRE(vertices[0].Position == Vector3{0, -.5, .5});
	REQUIRE(vertices[1].Position == Vector3{.5, 0, .5});
	REQUIRE(vertices[0].Normal == Vector3{0, 0, 1});
	REQUIRE(vertices[0].UV == Vector2{0, 0});
	auto shifted = RunNode(
		"pc.rm_to_voxel",
		{},
		{{"sdf_object", Sphere({-2.5, -.5, -.5})}, {"resolution", 2.0}, {"midpoint", Vector3{-2, 0, 0}}}
	);
	INFO(shifted.Message);
	REQUIRE(shifted.Ok);
	REQUIRE(std::get<MeshValue3D>(*shifted.OutputValue("mesh")).Data->Parts[0].Vertices == vertices);
}
TEST_CASE(
	"Source cube march retains authored case order and complementary winding", "[imagegraph][mesh_from_sdf]"
) {
	auto lower = RunNode("pc.rm_to_mesh", {}, {{"sdf_object", Sphere({-.5, -.5, -.5})}, {"resolution", 2.0}});
	INFO(lower.Message);
	REQUIRE(lower.Ok);
	const auto &a = std::get<MeshValue3D>(*lower.OutputValue("mesh")).Data->Parts[0].Vertices;
	REQUIRE(a.size() == 3);
	REQUIRE(a[0].Position == Vector3{-.25, 0, -.25});
	REQUIRE(a[1].Position == Vector3{0, -.25, -.25});
	REQUIRE(a[2].Position == Vector3{-.25, -.25, 0});
	REQUIRE(a[0].Normal == Vector3{-.0625, -.0625, -.0625});
	REQUIRE(a[2].UV == Vector2{0, 1});
	auto upper = RunNode("pc.rm_to_mesh", {}, {{"sdf_object", Sphere({.5, .5, .5})}, {"resolution", 2.0}});
	INFO(upper.Message);
	REQUIRE(upper.Ok);
	const auto &b = std::get<MeshValue3D>(*upper.OutputValue("mesh")).Data->Parts[0].Vertices;
	REQUIRE(b.size() == 3);
	REQUIRE(b[0].Position == Vector3{.25, .25, 0});
	REQUIRE(b[1].Position == Vector3{0, .25, .25});
	REQUIRE(b[2].Position == Vector3{.25, 0, .25});
	REQUIRE(b[0].Normal == Vector3{.0625, .0625, .0625});
}
TEST_CASE(
	"SDF converters bound occupancy and output geometry before allocating mesh storage",
	"[imagegraph][mesh_from_sdf]"
) {
	auto singular = RunNode("pc.rm_to_voxel", {}, {{"sdf_object", Sphere({})}, {"resolution", 1.0}});
	REQUIRE(singular.Code == Status::InvalidValue);
	auto huge = RunNode("pc.rm_to_voxel", {}, {{"sdf_object", Sphere({})}, {"resolution", 17.0}});
	REQUIRE(huge.Code == Status::LimitExceeded);
	auto dense = RunNode("pc.rm_to_voxel", {}, {{"sdf_object", Sphere({}, 10)}, {"resolution", 8.0}});
	REQUIRE(dense.Code == Status::LimitExceeded);
	auto empty = RunNode("pc.rm_to_mesh", {}, {{"sdf_object", Sphere({}, 10)}, {"resolution", 8.0}});
	INFO(empty.Message);
	REQUIRE(empty.Ok);
	REQUIRE(std::get<MeshValue3D>(*empty.OutputValue("mesh")).Data->Parts[0].Vertices.empty());
}
