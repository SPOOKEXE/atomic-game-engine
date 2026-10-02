#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.mesh_path_nodes")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
TEST_CASE(
	"Path revolve retains source extra sample seam caps and texture steps", "[imagegraph][mesh_path_nodes]"
) {
	Path2D path;
	path.Anchors = {{{1, 0, 0, 0, 0, 0}, 0}, {{1, 4, 0, 0, 0, 0}, 0}};
	auto run = RunNode(
		"pc.3_d_mesh_path_revolve",
		{},
		{{"path", path},
		 {"path_scale", 1.0},
		 {"path_sample", int64_t(2)},
		 {"revolve_sample", int64_t(3)},
		 {"caps", int64_t(3)}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Parts.size() == 3);
	CHECK(mesh.Data->Parts[0].Vertices.size() == 36);
	CHECK(mesh.Data->Parts[1].Vertices.size() == 9);
	CHECK(mesh.Data->Parts[2].Vertices.size() == 9);
	CHECK(mesh.Data->Edges.size() == 24);
	CHECK(mesh.Data->Materials.size() == 3);
	const auto &vertices = mesh.Data->Parts[0].Vertices;
	CHECK((vertices[0].Position == Vector3{1, 0, 0}));
	CHECK(std::abs(vertices[1].Position.Z + 3.9996) < 1e-12);
	CHECK(vertices[1].UV.Y == 1.0 / 3);
	CHECK(vertices[7].UV.Y == 2.0 / 3);
	CHECK((vertices[6].Normal == Vector3{}));
	CHECK((mesh.Data->Parts[1].Vertices[0].Normal == Vector3{0, 0, 1}));
	CHECK((mesh.Data->Parts[2].Vertices[0].Normal == Vector3{0, 0, -1}));
	CHECK(mesh.Data->Parts[2].MaterialIndex == 2);
	auto reversed = RunNode(
		"pc.3_d_mesh_path_revolve",
		{},
		{{"path", path},
		 {"path_scale", 1.0},
		 {"path_sample", int64_t(2)},
		 {"revolve_sample", int64_t(3)},
		 {"invert_normal", true}}
	);
	INFO(reversed.Message);
	REQUIRE(reversed.Ok);
	const auto &reverse = std::get<MeshValue3D>(*reversed.OutputValue("mesh"));
	CHECK(std::abs(reverse.Data->Parts[0].Vertices[0].Position.Z + 3.9996) < 1e-12);
}
TEST_CASE(
	"Path revolve projects spatial path components and admits geometry before allocation",
	"[imagegraph][mesh_path_nodes]"
) {
	PathValue3D path;
	auto &data = path.Data.emplace();
	data.Anchors = {{{0, 2, 0, 0, 0, 0, 0, 0, 0}, 0}, {{0, 2, 4, 0, 0, 0, 0, 0, 0}, 0}};
	auto run = RunNode(
		"pc.3_d_mesh_path_revolve",
		{},
		{{"path", path},
		 {"project_normal", EnumValue{0}},
		 {"path_scale", 1.0},
		 {"path_sample", int64_t(2)},
		 {"revolve_sample", int64_t(3)}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	CHECK(mesh.Data->Parts[0].Vertices[0].Position.X == 2);
	CHECK(std::abs(mesh.Data->Parts[0].Vertices[1].Position.Z + 3.96) < 1e-12);
	auto huge = RunNode(
		"pc.3_d_mesh_path_revolve",
		{},
		{{"path", path}, {"path_sample", int64_t(4096)}, {"revolve_sample", int64_t(4096)}}
	);
	CHECK_FALSE(huge.Ok);
	CHECK(huge.Code == Status::LimitExceeded);
}
TEST_CASE(
	"Path extrusion retains source winding radial normals and cap normal convention",
	"[imagegraph][mesh_path_nodes]"
) {
	PathValue3D path;
	auto &data = path.Data.emplace();
	data.Anchors = {{{0, 0, 0, 0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0, 0, 0, 0}, 0}};
	auto run = RunNode(
		"pc.3_d_mesh_path_extrude",
		{},
		{{"path", path},
		 {"path_scale", 1.0},
		 {"subdivision", int64_t(2)},
		 {"side", int64_t(3)},
		 {"radius", 1.0}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Parts.size() == 3);
	CHECK(mesh.Data->Parts[0].Vertices.size() == 18);
	CHECK(mesh.Data->Parts[1].Vertices.size() == 9);
	CHECK(mesh.Data->Parts[2].Vertices.size() == 9);
	CHECK(mesh.Data->Edges.size() == 18);
	const auto &side = mesh.Data->Parts[0].Vertices;
	CHECK((side[0].Position == Vector3{0, -1, 0}));
	CHECK(std::abs(side[1].Position.X - 9.9) < 1e-12);
	CHECK((side[0].UV == Vector2{1, 1}));
	CHECK((side[1].UV == Vector2{0, 1}));
	CHECK(side[0].Normal == side[1].Normal);
	CHECK(std::abs(side[0].Normal.Y - .5) < 1e-12);
	CHECK(std::abs(side[0].Normal.Z + std::sqrt(3.0) / 2) < 1e-12);
	CHECK((mesh.Data->Parts[1].Vertices[0].Normal == Vector3{-1, 0, 0}));
	CHECK((mesh.Data->Parts[2].Vertices[0].Normal == Vector3{-1, 0, 0}));
	CHECK(mesh.Data->Parts[1].MaterialIndex == 1);
	CHECK(mesh.Data->Parts[2].MaterialIndex == 1);
	auto inverted = RunNode(
		"pc.3_d_mesh_path_extrude",
		{},
		{{"path", path},
		 {"path_scale", 1.0},
		 {"subdivision", int64_t(2)},
		 {"side", int64_t(3)},
		 {"radius", 1.0},
		 {"inverted", true}}
	);
	REQUIRE(inverted.Ok);
	const auto &inverse = std::get<MeshValue3D>(*inverted.OutputValue("mesh"));
	CHECK(inverse.Data->Parts[0].Vertices[1].Position == side[2].Position);
	CHECK(inverse.Data->Parts[0].Vertices[2].Position == side[1].Position);
}
TEST_CASE(
	"Compiled spatial path to revolve uses generic path source getter transport",
	"[imagegraph][mesh_path_nodes]"
) {
	ArrayValue first{ValueType::Scalar, {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
		second{ValueType::Scalar, {1.0, 4.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"path",
		 "pc.path_3_d",
		 "",
		 {},
		 {},
		 {{"anchor_0", ValueType::Array, first}, {"anchor_1", ValueType::Array, second}}},
		{"revolve",
		 "pc.3_d_mesh_path_revolve",
		 "",
		 {},
		 {{"path_sample", int64_t(2)}, {"revolve_sample", int64_t(3)}, {"path_scale", 1.0}}}
	};
	document.Links = {{"path", "path_data", "revolve", "path"}};
	document.Outputs = {{"mesh", "revolve", "mesh"}};
	Plan plan;
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	REQUIRE(EvaluateValue(document, plan, "mesh", {}, output, diagnostic) == Status::Ok);
	const auto &mesh = std::get<MeshValue3D>(output.Data);
	REQUIRE(mesh.Data);
	CHECK(mesh.Data->Parts[0].Vertices.size() == 36);
	CHECK(std::abs(mesh.Data->Parts[0].Vertices[1].Position.Z + 3.96) < 1e-12);
}
