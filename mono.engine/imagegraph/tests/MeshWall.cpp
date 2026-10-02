#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.mesh_wall")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Path2D Line() {
		Path2D p;
		p.Anchors = {{{0, 0, 0, 0, 0, 0}}, {{4, 0, 0, 0, 0, 0}}};
		return p;
	}
}
TEST_CASE("Wall source offsets segment starts and retains four material parts", "[imagegraph][mesh_wall]") {
	auto run = RunNode(
		"pc.3_d_mesh_wall_builder",
		{},
		{{"path", Line()}, {"segments", int64_t(2)}, {"path_scale", 1.0}, {"height", 2.0}, {"thickness", .25}}
	);
	REQUIRE(run.Ok);
	const auto &mesh = *std::get<MeshValue3D>(*run.OutputValue("mesh")).Data;
	REQUIRE(mesh.Parts.size() == 4);
	REQUIRE(mesh.Parts[0].Vertices.size() == 6);
	REQUIRE(mesh.Parts[1].Vertices.size() == 6);
	REQUIRE(mesh.Parts[2].Vertices.size() == 12);
	REQUIRE(mesh.Parts[3].Vertices.size() == 6);
	REQUIRE(mesh.Edges.size() == 12);
	const auto &left = mesh.Parts[0].Vertices;
	REQUIRE(left[0].Position == Vector3{0, .25, 0});
	REQUIRE(left[1].Position == Vector3{2, .25, 0});
	REQUIRE(left[2].Position == Vector3{2, .25, 2});
	REQUIRE(left[1].Normal == Vector3{-.25, 2, 0});
	REQUIRE(left[1].UV == Vector2{1, 0});
	const auto &top = mesh.Parts[3].Vertices;
	REQUIRE(top[2].Position == Vector3{0, -.25, 2});
	REQUIRE(top[4].Position == Vector3{2, -.25, 2});
	REQUIRE(top[0].Normal == Vector3{0, 0, 1});
}
TEST_CASE(
	"Loop walls close source offset samples without end caps and reject degenerate offsets",
	"[imagegraph][mesh_wall]"
) {
	auto run = RunNode(
		"pc.3_d_mesh_wall_builder",
		{},
		{{"path", Line()}, {"segments", int64_t(2)}, {"path_scale", 1.0}, {"loop", true}}
	);
	REQUIRE(run.Ok);
	const auto &mesh = *std::get<MeshValue3D>(*run.OutputValue("mesh")).Data;
	REQUIRE(mesh.Parts[0].Vertices.size() == 12);
	REQUIRE(mesh.Parts[2].Vertices.empty());
	REQUIRE(mesh.Edges.size() == 14);
	auto bad = RunNode("pc.3_d_mesh_wall_builder", {}, {{"path", Line()}, {"path_scale", 0.0}});
	REQUIRE(bad.Code == Status::InvalidValue);
	auto large = RunNode("pc.3_d_mesh_wall_builder", {}, {{"path", Line()}, {"segments", int64_t(4096)}});
	REQUIRE(large.Code == Status::LimitExceeded);
}
