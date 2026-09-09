// Host conversion and device upload checks for editable resource ownership.

#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.editablemeshes")
TEST_DEPENDS("engine.scene.editablemesh")

using Catch::Approx;
using engine::core::Color3;
using engine::core::Vector2;
using engine::core::Vector3;
using engine::scene::EditableMesh;

TEST_CASE("editable mesh uploads follow store and owner lifetimes", "[render][gpu][editable-owner][.]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ecs::Store first("editable.first"), second("editable.second");
	const auto firstMesh = first.CreateInstance(scene::EditableMeshClass(), "mesh");
	const auto secondMesh = second.CreateInstance(scene::EditableMeshClass(), "mesh");
	REQUIRE(firstMesh == secondMesh);
	const auto triangle = [](ecs::Store &store, ecs::Entity mesh, float size) {
		REQUIRE(scene::AddVertex(store, mesh, {0, 0, 0}));
		REQUIRE(scene::AddVertex(store, mesh, {size, 0, 0}));
		REQUIRE(scene::AddVertex(store, mesh, {0, size, 0}));
		REQUIRE(scene::AddTriangle(store, mesh, 0, 1, 2));
	};
	triangle(first, firstMesh, 2);
	triangle(second, secondMesh, 4);
	const core::Name firstOwner("editable:first"), secondOwner("editable:second");
	const auto name = scene::EditableMeshContentName(first, firstMesh);
	CHECK(name == scene::EditableMeshContentName(second, secondMesh));
	render::EditableMeshUploader uploader;
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 1);
	CHECK(uploader.Refresh(second, fixture.Render, secondOwner) == 1);
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 0);
	CHECK(uploader.Refresh(second, fixture.Render, secondOwner) == 0);
	core::Vector3 extent;
	REQUIRE(fixture.Render.MeshExtentOf(name, extent, firstOwner));
	CHECK(extent.X == Approx(1));
	REQUIRE(fixture.Render.MeshExtentOf(name, extent, secondOwner));
	CHECK(extent.X == Approx(2));
	CHECK_FALSE(fixture.Render.MeshExtentOf(name, extent));
	uploader.ForgetWorld(first.Identity());
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 1);
	CHECK(uploader.Refresh(second, fixture.Render, secondOwner) == 0);
	fixture.Render.DropContentOwner(firstOwner);
	uploader.ForgetOwner(firstOwner);
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 1);
	CHECK(uploader.Refresh(second, fixture.Render, secondOwner) == 0);
	CHECK(uploader.Refresh(first, fixture.Render, core::Name("editable:rebound")) == 1);
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 0);
	REQUIRE(fixture.Render.FlushMeshes());
}

TEST_CASE("a mesh with vertices and no triangle is not yet valid to draw", "[render][editablemeshes]") {
	EditableMesh mesh;
	mesh.Positions.push_back(Vector3{0.0f, 0.0f, 0.0f});
	mesh.Normals.push_back(Vector3{0.0f, 1.0f, 0.0f});
	mesh.UVs.push_back(Vector2{});
	mesh.Colours.push_back(Color3{1.0f, 1.0f, 1.0f});
	mesh.Alphas.push_back(0.0f);

	const engine::assets::MeshData built = engine::render::BuildMeshData(mesh);

	// `Instance.new("EditableMesh")` produces exactly this state, and it must
	// not be reported as a broken mesh - it is one nobody has finished yet.
	CHECK_FALSE(built.IsValid());
}

TEST_CASE("a triangle converts position, normal and UV without touching them", "[render][editablemeshes]") {
	EditableMesh mesh;
	mesh.Positions = {
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 1.0f, 0.0f},
	};
	mesh.Normals = {
		Vector3{0.0f, 0.0f, 1.0f},
		Vector3{0.0f, 0.0f, 1.0f},
		Vector3{0.0f, 0.0f, 1.0f},
	};
	mesh.UVs = {Vector2{0.0f, 0.0f}, Vector2{1.0f, 0.0f}, Vector2{0.0f, 1.0f}};
	mesh.Colours = {
		Color3{1.0f, 1.0f, 1.0f},
		Color3{1.0f, 1.0f, 1.0f},
		Color3{1.0f, 1.0f, 1.0f},
	};
	mesh.Alphas = {0.0f, 0.0f, 0.0f};
	mesh.Indices = {0, 1, 2};

	const engine::assets::MeshData built = engine::render::BuildMeshData(mesh);

	REQUIRE(built.IsValid());
	REQUIRE(built.Vertices.size() == 3);
	CHECK(built.Vertices[1].Position[0] == Approx(1.0f));
	CHECK(built.Vertices[2].Position[1] == Approx(1.0f));
	CHECK(built.Vertices[0].Normal[2] == Approx(1.0f));
	CHECK(built.Vertices[1].TexCoord[0] == Approx(1.0f));
	REQUIRE(built.Indices.size() == 3);
	CHECK(built.Indices[0] == 0);
	CHECK(built.Indices[2] == 2);
}

TEST_CASE("one triangle's vertex colours average into its run's flat colour", "[render][editablemeshes]") {
	EditableMesh mesh;
	mesh.Positions = {Vector3{}, Vector3{1.0f, 0.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f}};
	mesh.Normals = {Vector3{0.0f, 1.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f}};
	mesh.UVs = {Vector2{}, Vector2{}, Vector2{}};
	mesh.Colours = {
		Color3{1.0f, 0.0f, 0.0f},
		Color3{0.0f, 1.0f, 0.0f},
		Color3{0.0f, 0.0f, 1.0f},
	};
	mesh.Alphas = {0.0f, 0.3f, 0.6f};
	mesh.Indices = {0, 1, 2};

	const engine::assets::MeshData built = engine::render::BuildMeshData(mesh);

	REQUIRE(built.Submeshes.size() == 1);
	const engine::assets::Submesh &submesh = built.Submeshes.front();
	CHECK(submesh.BaseColour[0] == Approx(1.0f / 3.0f));
	CHECK(submesh.BaseColour[1] == Approx(1.0f / 3.0f));
	CHECK(submesh.BaseColour[2] == Approx(1.0f / 3.0f));

	// Alpha is carried as one minus the average, matching `Visual::
	// Transparency`'s sense rather than an opacity's.
	CHECK(submesh.BaseColour[3] == Approx(1.0f - 0.3f));
	CHECK(submesh.IndexCount == 3);
}

// **The case the single-triangle test above cannot see**, and the one every
// script-built scene actually hits: a mesh whose faces are painted different
// colours. `assets::MeshVertex` carries no colour, so the only place a second
// colour can live is a second submesh run - and the conversion collapsing them
// into one average is what made a coloured terrain read as uniform mud.
TEST_CASE("faces of different colours become separate submesh runs", "[render][editablemeshes]") {
	EditableMesh mesh;

	// Two triangles sharing no vertex, so each face's three corners agree and
	// the average is exact.
	mesh.Positions = {
		Vector3{},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 0.0f, 1.0f},
		Vector3{4.0f, 0.0f, 0.0f},
		Vector3{5.0f, 0.0f, 0.0f},
		Vector3{4.0f, 0.0f, 1.0f},
	};
	mesh.Normals.assign(6, Vector3{0.0f, 1.0f, 0.0f});
	mesh.UVs.assign(6, Vector2{});
	mesh.Colours = {
		Color3{1.0f, 0.0f, 0.0f},
		Color3{1.0f, 0.0f, 0.0f},
		Color3{1.0f, 0.0f, 0.0f},
		Color3{0.0f, 0.0f, 1.0f},
		Color3{0.0f, 0.0f, 1.0f},
		Color3{0.0f, 0.0f, 1.0f},
	};
	mesh.Alphas.assign(6, 0.0f);
	mesh.Indices = {0, 1, 2, 3, 4, 5};

	const engine::assets::MeshData built = engine::render::BuildMeshData(mesh);

	REQUIRE(built.IsValid());
	REQUIRE(built.Submeshes.size() == 2);

	// First-appearance order, so a second conversion of the same mesh produces
	// the same file.
	CHECK(built.Submeshes[0].BaseColour[0] == Approx(1.0f));
	CHECK(built.Submeshes[0].BaseColour[2] == Approx(0.0f));
	CHECK(built.Submeshes[1].BaseColour[0] == Approx(0.0f));
	CHECK(built.Submeshes[1].BaseColour[2] == Approx(1.0f));

	// Contiguous and covering, which is what a run means.
	CHECK(built.Submeshes[0].FirstIndex == 0);
	CHECK(built.Submeshes[0].IndexCount == 3);
	CHECK(built.Submeshes[1].FirstIndex == 3);
	CHECK(built.Submeshes[1].IndexCount == 3);
	CHECK(built.Indices.size() == 6);
}

// Two colours a display cannot tell apart must not cost two draw runs.
TEST_CASE("colours within a quantisation step share one run", "[render][editablemeshes]") {
	EditableMesh mesh;
	mesh.Positions = {
		Vector3{},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 0.0f, 1.0f},
		Vector3{4.0f, 0.0f, 0.0f},
		Vector3{5.0f, 0.0f, 0.0f},
		Vector3{4.0f, 0.0f, 1.0f},
	};
	mesh.Normals.assign(6, Vector3{0.0f, 1.0f, 0.0f});
	mesh.UVs.assign(6, Vector2{});
	mesh.Colours.assign(3, Color3{0.5f, 0.5f, 0.5f});
	mesh.Colours.resize(6, Color3{0.5f + 1.0f / 2000.0f, 0.5f, 0.5f});
	mesh.Alphas.assign(6, 0.0f);
	mesh.Indices = {0, 1, 2, 3, 4, 5};

	const engine::assets::MeshData built = engine::render::BuildMeshData(mesh);

	REQUIRE(built.Submeshes.size() == 1);
	CHECK(built.Submeshes.front().IndexCount == 6);
}

TEST_CASE("an empty mesh converts to an empty, invalid MeshData", "[render][editablemeshes]") {
	const engine::assets::MeshData built = engine::render::BuildMeshData(EditableMesh{});
	CHECK(built.Vertices.empty());
	CHECK(built.Indices.empty());
	CHECK(built.Submeshes.empty());
	CHECK_FALSE(built.IsValid());
}
