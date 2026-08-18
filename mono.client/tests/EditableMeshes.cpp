// Converting `scene::EditableMesh`'s raw arrays into `assets::MeshData`.
//
// **The device-free half, and the only half this codebase unit-tests.**
// `client::EditableMeshUploader::Refresh` calls into `render::Renderer`
// itself, which nothing here can assert against without a GPU -
// `render::ShaderLibrary`'s tests draw the identical line for the identical
// reason. This pins the conversion: what a mesh built one triangle at a time
// looks like once it is in the format `render::MeshTable::Add` takes.

#include <client/EditableMeshes.hpp>

#include <engine/assets/Mesh.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("client.editablemeshes")
TEST_DEPENDS("engine.scene.editablemesh")

using Catch::Approx;
using engine::core::Color3;
using engine::core::Vector2;
using engine::core::Vector3;
using engine::scene::EditableMesh;

TEST_CASE("a mesh with vertices and no triangle is not yet valid to draw", "[client][editablemeshes]") {
	EditableMesh mesh;
	mesh.Positions.push_back(Vector3{0.0f, 0.0f, 0.0f});
	mesh.Normals.push_back(Vector3{0.0f, 1.0f, 0.0f});
	mesh.UVs.push_back(Vector2{});
	mesh.Colours.push_back(Color3{1.0f, 1.0f, 1.0f});
	mesh.Alphas.push_back(0.0f);

	const engine::assets::MeshData built = client::BuildMeshData(mesh);

	// `Instance.new("EditableMesh")` produces exactly this state, and it must
	// not be reported as a broken mesh - it is one nobody has finished yet.
	CHECK_FALSE(built.IsValid());
}

TEST_CASE("a triangle converts position, normal and UV without touching them", "[client][editablemeshes]") {
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

	const engine::assets::MeshData built = client::BuildMeshData(mesh);

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

TEST_CASE("vertex colours average into the one submesh's flat colour", "[client][editablemeshes]") {
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

	const engine::assets::MeshData built = client::BuildMeshData(mesh);

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

TEST_CASE("an empty mesh converts to an empty, invalid MeshData", "[client][editablemeshes]") {
	const engine::assets::MeshData built = client::BuildMeshData(EditableMesh{});
	CHECK(built.Vertices.empty());
	CHECK(built.Indices.empty());
	CHECK(built.Submeshes.empty());
	CHECK_FALSE(built.IsValid());
}
