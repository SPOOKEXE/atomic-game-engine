#include "../src/nodes/SourceShape3DGeometry.hpp"

#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_shape_3d_geometry")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

namespace {
	struct GeometryRun {
		Node Authored{"shape", "pc.shape_3_d", "", {}, {}};
		EvaluationRequest Request;
		NodeContext Context{Authored, *FindCatalogueEntry("pc.shape_3_d"), Request};
		MeshValue3D Mesh;
		AllocationReservation Charge;
		GeometryRun() {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
		}
	};
}

TEST_CASE("Draw Shape 3D builds all pinned local geometry families", "[imagegraph][shape3d]") {
	struct Fixture {
		SourceShape3DKind Shape;
		size_t Parts;
		size_t Vertices;
		size_t Edges;
	};
	for (const Fixture &fixture :
		 {Fixture{SourceShape3DKind::Plane, 1, 6, 4},
		  {SourceShape3DKind::Cube, 6, 36, 12},
		  {SourceShape3DKind::Octahedron, 8, 24, 0},
		  {SourceShape3DKind::Cylinder, 3, 96, 32},
		  {SourceShape3DKind::Cone, 2, 48, 16},
		  {SourceShape3DKind::Capsule, 3, 816, 16},
		  {SourceShape3DKind::Sphere, 1, 768, 0},
		  {SourceShape3DKind::CutSphere, 2, 408, 0},
		  {SourceShape3DKind::Torus, 1, 768, 0}}) {
		GeometryRun run;
		SourceShape3DRecipe recipe;
		recipe.Shape = fixture.Shape;
		INFO(static_cast<int>(fixture.Shape));
		const bool prepared = BuildSourceShape3DGeometry(run.Context, recipe, run.Mesh, run.Charge);
		INFO(run.Context.FailureMessage);
		REQUIRE(prepared);
		REQUIRE(run.Mesh.Data);
		CHECK(run.Mesh.Data->Parts.size() == fixture.Parts);
		CHECK(run.Mesh.Data->Edges.size() == fixture.Edges);
		size_t vertices = 0;
		for (size_t part = 0; part < fixture.Parts; ++part) {
			vertices += run.Mesh.Data->Parts[part].Vertices.size();
			CHECK(run.Mesh.Data->Parts[part].MaterialIndex == part);
		}
		CHECK(vertices == fixture.Vertices);
		CHECK(run.Context.OutputValues.empty());
		CHECK(run.Context.OutputImages.empty());
	}
}

TEST_CASE("Draw Shape 3D preserves source Cube VB reorder and plane UV", "[imagegraph][shape3d]") {
	GeometryRun cube;
	SourceShape3DRecipe recipe;
	const bool cubePrepared = BuildSourceShape3DGeometry(cube.Context, recipe, cube.Mesh, cube.Charge);
	INFO(cube.Context.FailureMessage);
	REQUIRE(cubePrepared);
	const auto &parts = cube.Mesh.Data->Parts;
	constexpr std::array<Vector3, 6> NORMALS{
		{{0, 0, -1}, {1, 0, 0}, {0, -1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 1, 0}}
	};
	for (size_t part = 0; part < parts.size(); ++part)
		CHECK(parts[part].Vertices[0].Normal == NORMALS[part]);
	CHECK((parts[0].Vertices[0].Position == Vector3{-.5, -.5, -.5}));
	CHECK((parts[0].Vertices[0].UV == Vector2{1, 1}));
	GeometryRun plane;
	recipe.Shape = SourceShape3DKind::Plane;
	const bool planePrepared = BuildSourceShape3DGeometry(plane.Context, recipe, plane.Mesh, plane.Charge);
	INFO(plane.Context.FailureMessage);
	REQUIRE(planePrepared);
	CHECK((plane.Mesh.Data->Parts[0].Vertices[2].UV == Vector2{0, 1}));
	GeometryRun octahedron;
	recipe.Shape = SourceShape3DKind::Octahedron;
	const bool octahedronPrepared =
		BuildSourceShape3DGeometry(octahedron.Context, recipe, octahedron.Mesh, octahedron.Charge);
	INFO(octahedron.Context.FailureMessage);
	REQUIRE(octahedronPrepared);
	CHECK((octahedron.Mesh.Data->Parts[3].Vertices[1].UV == Vector2{1, .5}));
	CHECK((octahedron.Mesh.Data->Parts[3].Vertices[1].Normal == Vector3{}));
}

TEST_CASE(
	"Draw Shape 3D geometry refuses cached or oversized source states atomically", "[imagegraph][shape3d]"
) {
	for (const SourceShape3DRecipe &recipe : [] {
			 std::array<SourceShape3DRecipe, 4> recipes;
			 recipes[0].Shape = SourceShape3DKind::CutSphere;
			 recipes[0].Ratio = 0;
			 recipes[1].Shape = SourceShape3DKind::Sphere;
			 recipes[1].Sides = {4096, 4096};
			 recipes[2].Shape = SourceShape3DKind::Capsule;
			 recipes[2].HeightControl = 0;
			 recipes[3].Shape = static_cast<SourceShape3DKind>(999);
			 return recipes;
		 }()) {
		GeometryRun run;
		CHECK_FALSE(BuildSourceShape3DGeometry(run.Context, recipe, run.Mesh, run.Charge));
		CHECK_FALSE(run.Mesh.Data);
		CHECK(run.Context.OutputValues.empty());
	}
	GeometryRun budget;
	budget.Context.ByteBudget = 1;
	CHECK_FALSE(BuildSourceShape3DGeometry(budget.Context, {}, budget.Mesh, budget.Charge));
	CHECK(budget.Context.FailureCode == Status::LimitExceeded);
	CHECK_FALSE(budget.Mesh.Data);
}
