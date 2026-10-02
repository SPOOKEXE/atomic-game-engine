#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.mesh_modify")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

namespace {
	MeshValue3D Triangle() {
		MeshValue3D mesh;
		auto &data = mesh.Data.emplace();
		data.LocalTransforms.push_back({});
		data.Materials.emplace_back();
		data.Parts.push_back(
			{{{{.25, 0, 0}, {0, 0, 1}, {0, 0}, {10, 20, 30, 40}},
			  {{.75, 0, 0}, {0, 1, 0}, {1, 0}, {50, 60, 70, 80}},
			  {{0, 1, 0}, {1, 0, 0}, {0, 1}, {90, 100, 110, 120}}},
			 0,
			 std::nullopt}
		);
		return mesh;
	}
}

TEST_CASE(
	"Mesh discretize uses half even snapping and keeps normals UV and tint", "[imagegraph][mesh_modify]"
) {
	const auto input = Triangle();
	const auto run = RunNode("pc.3_d_round_vertex", {}, {{"mesh", input}, {"step", .5}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &output = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	REQUIRE(output.Data);
	const auto &vertices = output.Data->Parts.front().Vertices;
	CHECK(vertices[0].Position.X == 0);
	CHECK(vertices[1].Position.X == 1);
	CHECK(vertices[0].Normal == input.Data->Parts[0].Vertices[0].Normal);
	CHECK(vertices[0].UV == input.Data->Parts[0].Vertices[0].UV);
	CHECK(vertices[0].Tint == input.Data->Parts[0].Vertices[0].Tint);
	CHECK(input.Data->Parts[0].Vertices[0].Position.X == .25);
	const auto zero = RunNode("pc.3_d_round_vertex", {}, {{"mesh", input}, {"step", 0.0}});
	REQUIRE(zero.Ok);
	const auto &zeroMesh = std::get<MeshValue3D>(*zero.OutputValue("mesh"));
	CHECK(zeroMesh.Data->Parts == input.Data->Parts);
	CHECK(zeroMesh.Data->CpuVerticesPresent);
	CHECK_FALSE(zeroMesh.Data->CpuEdgesPresent);
}

TEST_CASE(
	"Mesh subdivision keeps source ordered triangles and default midpoint tint", "[imagegraph][mesh_modify]"
) {
	const auto input = Triangle();
	const auto run = RunNode("pc.3_d_subdivide", {}, {{"mesh", input}, {"level", int64_t(1)}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &output = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	const auto &vertices = output.Data->Parts.front().Vertices;
	REQUIRE(vertices.size() == 12);
	CHECK(vertices[0] == input.Data->Parts[0].Vertices[0]);
	CHECK((vertices[1].Position == Vector3{.5, 0, 0}));
	CHECK((vertices[1].Normal == Vector3{0, .5, .5}));
	CHECK((vertices[1].UV == Vector2{.5, 0}));
	CHECK((vertices[1].Tint == Colour{255, 255, 255, 255}));
	CHECK(vertices[4] == input.Data->Parts[0].Vertices[1]);
	CHECK(vertices[8] == input.Data->Parts[0].Vertices[2]);
	CHECK(vertices[9] == vertices[1]);
	CHECK(vertices[10] == vertices[5]);
	CHECK(vertices[11] == vertices[2]);
	const auto excessive = RunNode("pc.3_d_subdivide", {}, {{"mesh", input}, {"level", int64_t(20)}});
	CHECK_FALSE(excessive.Ok);
	CHECK(excessive.Code == Status::LimitExceeded);
	CHECK(excessive.Port == "level");
}

TEST_CASE(
	"Mesh origin retains wrapper semantics and source vertex weighted center", "[imagegraph][mesh_modify]"
) {
	auto input = Triangle();
	input.Data->LocalTransforms[0].Position = {3, 4, 5};
	const auto run = RunNode(
		"pc.3_d_set_origin", {}, {{"mesh", input}, {"type", EnumValue{0}}, {"point", Vector3{7, 8, 9}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK((std::get<Vector3>(*run.OutputValue("origin")) == Vector3{7, 8, 9}));
	const auto &output = std::get<MeshValue3D>(*run.OutputValue("scene"));
	REQUIRE(output.Data->LocalTransforms.size() == 2);
	CHECK((output.Data->LocalTransforms[0].Position == Vector3{4, 4, 4}));
	CHECK((output.Data->LocalTransforms[0].Anchor == Vector3{7, 8, 9}));
	CHECK(output.Data->LocalTransforms[1] == input.Data->LocalTransforms[0]);
	CHECK(output.Data->Parts == input.Data->Parts);
	const auto center = RunNode("pc.3_d_set_origin", {}, {{"mesh", input}});
	REQUIRE(center.Ok);
	CHECK((std::get<Vector3>(*center.OutputValue("origin")) == Vector3{1.0 / 3, 1.0 / 3, 0}));
}

TEST_CASE(
	"Mesh displacement reads wrapped material UV and source RGB luminance", "[imagegraph][mesh_modify]"
) {
	auto input = Triangle();
	for (auto &vertex : input.Data->Parts[0].Vertices)
		vertex.Normal = {0, 0, 1};
	MaterialValue3D material;
	material.Edit().Surface = Image{1, 1, {255, 0, 0, 255}, 0};
	const auto run = RunNode(
		"pc.3_d_displace",
		{},
		{{"mesh", input}, {"displace_texture", material}, {"height", 2.0}, {"recalculate_normal", false}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &output = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	for (const auto &vertex : output.Data->Parts[0].Vertices) {
		CHECK(vertex.Position.Z == .598);
		CHECK((vertex.Normal == Vector3{0, 0, 1}));
	}
	CHECK(output.Data->Edges == input.Data->Edges);
	const auto unchanged = RunNode("pc.3_d_displace", {}, {{"mesh", input}});
	REQUIRE(unchanged.Ok);
	CHECK(std::get<MeshValue3D>(*unchanged.OutputValue("mesh")) == input);
}

TEST_CASE(
	"Mesh vertex extraction removes duplicates before transforming positions and normals",
	"[imagegraph][mesh_modify]"
) {
	auto input = Triangle();
	input.Data->Parts[0].Vertices[1] = input.Data->Parts[0].Vertices[0];
	input.Data->LocalTransforms[0].Position = {1, 2, 3};
	input.Data->LocalTransforms[0].Scale = {2, 3, 4};
	const auto run = RunNode("pc.3_d_mesh_vertex_points", {}, {{"mesh", input}, {"apply_transform", true}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &positions = std::get<ArrayValue>(*run.OutputValue("positions"));
	const auto &normals = std::get<ArrayValue>(*run.OutputValue("normals"));
	REQUIRE(positions.Nested.size() == 2);
	REQUIRE(normals.Nested.size() == 2);
	CHECK(std::get<double>(positions.Nested[0][0]) == 1.5);
	CHECK(std::get<double>(positions.Nested[0][1]) == 2);
	CHECK(std::get<double>(positions.Nested[0][2]) == 3);
	CHECK(std::get<double>(normals.Nested[0][2]) == 4);
	CHECK(input.Data->Parts[0].Vertices[0].Position.X == .25);
}

TEST_CASE(
	"Mesh UV remap projects source world positions with negative Y map basis", "[imagegraph][mesh_modify]"
) {
	auto input = Triangle();
	input.Data->LocalTransforms[0].Position = {1, 2, 3};
	const auto run = RunNode(
		"pc.3_d_uv_remap", {}, {{"mesh", input}, {"position", Vector3{1, 2, 0}}, {"scale", Vector3{2, 2, 1}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &vertices = std::get<MeshValue3D>(*run.OutputValue("mesh")).Data->Parts[0].Vertices;
	CHECK((vertices[0].UV == Vector2{.625, .5}));
	CHECK((vertices[1].UV == Vector2{.875, .5}));
	CHECK((vertices[2].UV == Vector2{.5, 0}));
	CHECK(vertices[0].Position == input.Data->Parts[0].Vertices[0].Position);
	const auto singular = RunNode("pc.3_d_uv_remap", {}, {{"mesh", input}, {"scale", Vector3{0, 1, 1}}});
	CHECK_FALSE(singular.Ok);
	CHECK(singular.Port == "scale");
}

TEST_CASE(
	"UV remap traverses source groups in local then parent transform order", "[imagegraph][mesh_modify]"
) {
	const auto plane = RunNode("pc.3_d_mesh_plane", {}, {{"position", Vector3{2, 0, 0}}});
	REQUIRE(plane.Ok);
	const auto &mesh = std::get<MeshValue3D>(*plane.OutputValue("mesh"));
	SceneValue3D source;
	auto &group = source.Data.emplace();
	group.Transform.Position = {4, 0, 0};
	group.Objects.push_back({mesh});
	const auto remapped = RunNode("pc.3_d_uv_remap", {}, {{"mesh", source}});
	INFO(remapped.Message);
	REQUIRE(remapped.Ok);
	const auto &result = std::get<SceneValue3D>(*remapped.OutputValue("mesh"));
	REQUIRE(result.Data);
	const auto &output = std::get<MeshValue3D>(result.Data->Objects.front().Data);
	REQUIRE(output.Data);
	CHECK(
		output.Data->Parts.front().Vertices.front().UV.X ==
		float(mesh.Data->Parts.front().Vertices.front().Position.X + 6 + .5)
	);
	CHECK(output.Data->LocalTransforms == mesh.Data->LocalTransforms);
	CHECK(result.Data->Transform == group.Transform);
}

TEST_CASE("Set material assigns one row per direct source group object", "[imagegraph][mesh_modify]") {
	const auto plane = RunNode("pc.3_d_mesh_plane", {});
	REQUIRE(plane.Ok);
	const auto &mesh = std::get<MeshValue3D>(*plane.OutputValue("mesh"));
	SceneValue3D scene;
	auto &group = scene.Data.emplace();
	group.Objects = {{mesh}, {mesh}};
	MaterialValue3D first, second;
	first.Data.emplace().Diffuse = .2;
	second.Data.emplace().Diffuse = .8;
	ArrayValue materials;
	materials.ElementType = ValueType::Material3D;
	materials.Elements = {first, second};
	const auto result = RunNode(
		"pc.3_d_set_material", {}, {{"mesh", scene}, {"materials", materials}, {"single_material", false}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	const auto &output = std::get<SceneValue3D>(*result.OutputValue("mesh"));
	REQUIRE(output.Data);
	CHECK(
		std::get<MeshValue3D>(output.Data->Objects[0].Data).Data->Materials.front().Get().Diffuse ==
		first.Get().Diffuse
	);
	CHECK(
		std::get<MeshValue3D>(output.Data->Objects[1].Data).Data->Materials.front().Get().Diffuse ==
		second.Get().Diffuse
	);
}
TEST_CASE("Displacement keeps source zero normals for a degenerate triangle", "[imagegraph][mesh_modify]") {
	auto mesh = Triangle();
	for (auto &vertex : mesh.Data->Parts[0].Vertices)
		vertex.Position = {};
	const auto surface = imagegraph_test::MakeImage(1, 1, {0, 0, 0, 255});
	auto run = RunNode("pc.3_d_displace", {{"displace_texture", &surface}}, {{"mesh", mesh}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &output = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	for (const auto &vertex : output.Data->Parts[0].Vertices)
		CHECK((vertex.Normal == Vector3{}));
}
