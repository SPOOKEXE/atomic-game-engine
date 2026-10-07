#include "../src/nodes/SourceShape3DAxial.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_SUITE_ID("engine.imagegraph.source_shape_3d_axial")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

TEST_CASE("Source cylinder keeps side first and hidden cap edges", "[imagegraph][shape3d]") {
	SourceShape3DRecipe recipe;
	recipe.Shape = SourceShape3DKind::Cylinder;
	recipe.Side = 4;
	recipe.Smooth = true;
	MeshData3D mesh;
	mesh.Materials.resize(2);
	FillSourceShape3DAxial(recipe, mesh);
	REQUIRE(mesh.Parts.size() == 3);
	CHECK(mesh.Parts[0].Vertices.size() == 24);
	CHECK(mesh.Parts[1].Vertices.size() == 12);
	CHECK(mesh.Parts[2].Vertices.size() == 12);
	CHECK(mesh.Parts[2].MaterialIndex == 0);
	CHECK((mesh.Parts[0].Vertices[0].Position == Vector3{.5, 0, .5}));
	CHECK((mesh.Parts[0].Vertices[0].Normal == Vector3{1, 0, 0}));
	CHECK((mesh.Parts[0].Vertices[0].UV == Vector2{0, 1}));
	CHECK(mesh.Parts[1].Vertices[2].Position.Y == Catch::Approx(-.5));
	CHECK(mesh.Parts[2].Vertices[1].Position.Y == Catch::Approx(-.5));
	REQUIRE(mesh.Edges.size() == 16);
	mesh = {};
	recipe.Caps = false;
	FillSourceShape3DAxial(recipe, mesh);
	CHECK(mesh.Parts.size() == 1);
	CHECK(mesh.Edges.size() == 16);
}

TEST_CASE("Source cone ignores caps and retains unnormalized normals", "[imagegraph][shape3d]") {
	SourceShape3DRecipe recipe;
	recipe.Shape = SourceShape3DKind::Cone;
	recipe.Side = 4;
	recipe.Caps = false;
	recipe.Smooth = true;
	MeshData3D mesh;
	FillSourceShape3DAxial(recipe, mesh);
	REQUIRE(mesh.Parts.size() == 2);
	CHECK(mesh.Parts[0].Vertices.size() == 12);
	REQUIRE(mesh.Parts[1].Vertices.size() == 12);
	CHECK(mesh.Edges.size() == 8);
	CHECK((mesh.Parts[1].Vertices[0].Position == Vector3{0, 0, .5}));
	CHECK((mesh.Parts[1].Vertices[0].UV == Vector2{.25, 0}));
	CHECK((mesh.Parts[1].Vertices[1].Normal == Vector3{1, 0, .2}));
	CHECK((mesh.Parts[1].Vertices[1].UV == Vector2{0, 1}));
}

TEST_CASE(
	"Source capsule preserves hemisphere seam and half length smooth normals", "[imagegraph][shape3d]"
) {
	SourceShape3DRecipe recipe;
	recipe.Shape = SourceShape3DKind::Capsule;
	recipe.Side = 4;
	recipe.HeightControl = .5;
	recipe.Smooth = true;
	MeshData3D mesh;
	FillSourceShape3DAxial(recipe, mesh);
	REQUIRE(mesh.Parts.size() == 3);
	CHECK(mesh.Parts[0].Vertices.size() == 24);
	REQUIRE(mesh.Parts[1].Vertices.size() == 96);
	REQUIRE(mesh.Parts[2].Vertices.size() == 96);
	CHECK(mesh.Edges.size() == 8);
	CHECK((mesh.Parts[1].Vertices[0].Position == Vector3{.5, 0, .25}));
	CHECK((mesh.Parts[1].Vertices[0].Normal == Vector3{.5, 0, 0}));
	CHECK(mesh.Parts[1].Vertices[2].Position.Y == Catch::Approx(.5));
	CHECK(mesh.Parts[0].Vertices[2].Position.Y == Catch::Approx(-.5));
	CHECK((mesh.Parts[2].Vertices[0].Position == Vector3{.5, 0, -.25}));
	CHECK(mesh.Parts[1].Vertices[1].UV.Y == Catch::Approx(.5 - .5 * std::sin(std::acos(-1.) / 8)));
	mesh = {};
	recipe.Smooth = false;
	FillSourceShape3DAxial(recipe, mesh);
	const auto normal = mesh.Parts[1].Vertices[0].Normal;
	CHECK(normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z == Catch::Approx(1));
	CHECK(mesh.Parts[1].Vertices[0].Normal == mesh.Parts[1].Vertices[5].Normal);
}
