#include <engine/assets/Builtin.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/GltfSceneExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.script.gltfsceneexport")
TEST_DEPENDS("engine.scene.editablemesh")

namespace {
	void Identify(engine::ecs::Store &store, engine::ecs::Entity entity, std::string_view id) {
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::String;
		value.String = id;
		REQUIRE(engine::ecs::SetAttribute(store, entity, engine::core::Name("DataFactoryId"), value));
	}
}

TEST_CASE(
	"glTF scene export preserves visual transparency and reports unsupported appearance facts",
	"[script][gltf]"
) {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store store("script.gltf.appearance");
	const auto transparent = engine::scene::MakePart(store, {});
	const auto hidden = engine::scene::MakePart(store, {});
	const auto texturedAlpha = engine::scene::MakePart(store, {});
	Identify(store, transparent, "scene/transparent");
	Identify(store, hidden, "scene/hidden");
	Identify(store, texturedAlpha, "scene/textured-alpha");

	auto transparentVisual = *store.Get<engine::scene::Visual>(transparent);
	transparentVisual.Transparency = 0.25f;
	store.Set(transparent, transparentVisual);
	auto hiddenVisual = *store.Get<engine::scene::Visual>(hidden);
	hiddenVisual.Visible = false;
	store.Set(hidden, hiddenVisual);
	store.Set(
		texturedAlpha,
		engine::scene::SurfaceAppearance{
			.ColourMap = engine::core::Name("content/alpha.png"),
			.Mode = engine::scene::AlphaMode::Transparency
		}
	);

	engine::script::GltfSceneExport exported;
	std::string failure;
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure));
	REQUIRE(exported.Nodes.size() == 1);
	CHECK(exported.Nodes[0].StableId == "scene/transparent");
	CHECK(exported.Nodes[0].Material.Alpha == 0.75f);
	CHECK(exported.Nodes[0].Material.AlphaMode == engine::script::GltfExportAlphaMode::Blend);
	REQUIRE(exported.Unavailable.size() == 2);
	CHECK(exported.Unavailable[0].StableId == "scene/hidden");
	CHECK(exported.Unavailable[0].Reason == "visual_hidden_not_representable");
	CHECK(exported.Unavailable[1].StableId == "scene/textured-alpha");
	CHECK(exported.Unavailable[1].Reason == "surface_alpha_mode_texture_dependent");
}

TEST_CASE("glTF scene export rejects invalid camera and geometry transforms", "[script][gltf]") {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store store("script.gltf.invalid-transform");
	const auto part = engine::scene::MakePart(store, {});
	Identify(store, part, "scene/invalid-geometry");
	auto transform = *store.Get<engine::scene::Transform>(part);
	transform.Frame.QuaternionW = 0.0f;
	store.Set(part, transform);
	const auto camera = store.Create();
	store.Set(camera, engine::ecs::InstanceName{engine::core::Name("Camera")});
	store.Set(camera, engine::scene::Transform{});
	store.Set(
		camera,
		engine::scene::Camera{.FarPlane = std::numeric_limits<float>::infinity(), .RenderFeatures = {}}
	);
	Identify(store, camera, "scene/invalid-camera");

	engine::script::GltfSceneExport exported;
	std::string failure;
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure));
	CHECK(exported.Nodes.empty());
	CHECK(exported.Cameras.empty());
	REQUIRE(exported.Unavailable.size() == 2);
	CHECK(exported.Unavailable[0].Reason == "camera_transform_or_projection_invalid");
	CHECK(exported.Unavailable[1].Reason == "transform_or_mesh_extent_invalid");
}

TEST_CASE("glTF scene export captures exact built-in geometry and camera facts", "[script][gltf]") {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store store("script.gltf.builtin");
	const auto part = engine::scene::MakePart(
		store,
		{
			.Frame = engine::core::CFrame({2, 3, 4}),
			.Size = {4, 6, 8},
			.Material = {},
			.Mesh = engine::core::Name(engine::assets::BuiltinName(engine::assets::BuiltinMesh::Cube)),
			.Class = {},
		}
	);
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	Identify(store, part, "scene/cube");
	const auto camera = store.Create();
	store.Set(camera, engine::ecs::InstanceName{engine::core::Name("Camera")});
	store.Set(camera, engine::scene::Transform{engine::core::CFrame({1, 2, 3})});
	store.Set(
		camera,
		engine::scene::Camera{
			.FieldOfViewRadians = 1.0f, .NearPlane = 0.25f, .FarPlane = 100.0f, .RenderFeatures = {}
		}
	);
	Identify(store, camera, "scene/camera");

	engine::script::GltfSceneExport exported;
	std::string failure;
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure));
	CHECK(exported.Tick == 0);
	REQUIRE(exported.Meshes.size() == 1);
	CHECK(exported.Meshes[0].Data.Vertices.size() == 24);
	CHECK(exported.Meshes[0].Data.Indices.size() == 36);
	REQUIRE(exported.Nodes.size() == 1);
	CHECK(exported.Nodes[0].StableId == "scene/cube");
	CHECK(exported.Nodes[0].Frame.Position.X == 2.0f);
	CHECK(exported.Nodes[0].Scale.X == 4.0f);
	CHECK(exported.Nodes[0].Scale.Y == 6.0f);
	CHECK(exported.Nodes[0].Scale.Z == 8.0f);
	REQUIRE(exported.Cameras.size() == 1);
	CHECK(exported.Cameras[0].StableId == "scene/camera");
	CHECK(exported.Cameras[0].NearPlaneMetres == 0.25f);
}

TEST_CASE(
	"glTF scene export preserves editable mesh triangles and names unavailable content", "[script][gltf]"
) {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store store("script.gltf.editable");
	const auto editable = store.CreateInstance(engine::scene::EditableMeshClass(), "Editable");
	engine::scene::EditableMesh geometry;
	geometry.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	geometry.Normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
	geometry.UVs = {{0, 0}, {1, 0}, {0, 1}};
	geometry.Indices = {0, 1, 2};
	store.Set(editable, geometry);
	const auto exact = engine::scene::MakePart(
		store,
		{
			.Frame = {},
			.Size = {1, 1, 1},
			.Material = {},
			.Mesh = engine::scene::EditableMeshContentName(store, editable),
			.Class = {},
		}
	);
	Identify(store, exact, "scene/editable");
	const auto missing = engine::scene::MakePart(
		store,
		{.Frame = {},
		 .Size = {1, 1, 1},
		 .Material = {},
		 .Mesh = engine::core::Name("content/missing.amesh"),
		 .Class = {}}
	);
	Identify(store, missing, "scene/missing");

	engine::script::GltfSceneExport exported;
	std::string failure;
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure));
	REQUIRE(exported.Meshes.size() == 1);
	CHECK(exported.Meshes[0].Data.Indices == std::vector<uint32_t>{0, 1, 2});
	REQUIRE(exported.Nodes.size() == 1);
	CHECK(exported.Nodes[0].StableId == "scene/editable");
	REQUIRE(exported.Unavailable.size() == 1);
	CHECK(exported.Unavailable[0].StableId == "scene/missing");
	CHECK(exported.Unavailable[0].Reason == "source_geometry_unavailable");
}
