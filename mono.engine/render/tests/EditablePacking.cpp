#include <engine/assets/Mesh.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.editablepacking")
TEST_DEPENDS("engine.scene.editablepacking")

TEST_CASE("mesh packing quantizes only selected presentation attributes", "[render][editablepacking]") {
	using namespace engine;
	scene::EditableMesh mesh;
	mesh.Positions = {{0.46f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
	mesh.Normals = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
	mesh.UVs = {{0.12345f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
	mesh.Indices = {0, 1, 2};
	mesh.Packing.Attributes = static_cast<uint8_t>(scene::EditablePackingAttribute::Position);
	mesh.Packing.Format = scene::EditablePackingFormat::Unsigned4;
	mesh.Packing.Minimum = 0.0f;
	mesh.Packing.Maximum = 1.0f;

	const assets::MeshData built = render::BuildMeshData(mesh);
	REQUIRE(built.IsValid());
	CHECK(built.Vertices[0].Position[0] == Catch::Approx(7.0f / 15.0f).margin(0.0001f));
	CHECK(built.Vertices[0].TexCoord[0] == Catch::Approx(0.12345f));
	CHECK(mesh.Positions[0].X == Catch::Approx(0.46f));
}
