#include "../src/nodes/SourceShape3DRound.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_SUITE_ID("engine.imagegraph.source_shape_3d_round")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

TEST_CASE("source sphere keeps swapped subdivision axes and unnormalized smooth normals") {
	SourceShape3DRecipe recipe;
	recipe.Shape = SourceShape3DKind::Sphere;
	recipe.Sides = {4, 3};
	recipe.Smooth = true;
	MeshData3D mesh;
	FillSourceShape3DRound(recipe, mesh);
	REQUIRE(mesh.Parts.size() == 1);
	REQUIRE(mesh.Parts[0].Vertices.size() == 72);
	const auto &vertices = mesh.Parts[0].Vertices;
	REQUIRE(vertices[0].Position == vertices[0].Normal);
	REQUIRE(vertices[0].Position.Z == .5);
	REQUIRE(vertices[0].UV == Vector2{0, 0});
	REQUIRE(std::abs(vertices[2].Position.X - std::sqrt(.125)) < 1e-12);
	REQUIRE(std::abs(vertices[1].UV.X - 2.0 / 3) < 1e-12);
}

TEST_CASE("source cut sphere keeps rounded partial row and cap material order") {
	SourceShape3DRecipe recipe;
	recipe.Shape = SourceShape3DKind::CutSphere;
	recipe.Sides = {4, 3};
	recipe.Ratio = .3;
	recipe.Smooth = true;
	std::array<uint64_t, 2> counts{};
	REQUIRE(SourceShape3DRoundCounts(recipe, counts));
	REQUIRE(counts == std::array<uint64_t, 2>{36, 9});
	MeshData3D mesh;
	FillSourceShape3DRound(recipe, mesh);
	REQUIRE(mesh.Parts.size() == 2);
	REQUIRE(mesh.Parts[0].MaterialIndex == 0);
	REQUIRE(mesh.Parts[1].MaterialIndex == 1);
	REQUIRE(mesh.Parts[1].Vertices.size() == 9);
	REQUIRE(mesh.Parts[1].Vertices[2].UV == Vector2{.5, .5});
	REQUIRE(mesh.Parts[1].Vertices[0].Normal == Vector3{0, 0, 1});
	recipe.Ratio = 0;
	REQUIRE_FALSE(SourceShape3DRoundCounts(recipe, counts));
}

TEST_CASE("source torus retains constructor side defaults and radial flat normals") {
	SourceShape3DRecipe recipe;
	recipe.Shape = SourceShape3DKind::Torus;
	recipe.Sides = {5, 3};
	recipe.Radius = {.75, .25};
	MeshData3D mesh;
	FillSourceShape3DRound(recipe, mesh);
	REQUIRE(mesh.Parts.size() == 1);
	REQUIRE(mesh.Parts[0].Vertices.size() == 768);
	const auto &vertices = mesh.Parts[0].Vertices;
	REQUIRE(vertices[0].Position == Vector3{1, 0, 0});
	REQUIRE(vertices[0].UV == Vector2{2, 1});
	REQUIRE(vertices[0].Normal == vertices[1].Normal);
	REQUIRE(vertices[1].Position.Y < 0);
	REQUIRE(vertices[1].Position.Z < 0);
	recipe.Smooth = true;
	FillSourceShape3DRound(recipe, mesh);
	REQUIRE(mesh.Parts[0].Vertices[0].Normal == Vector3{.25, 0, 0});
}
