#include <engine/assets/Builtin.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/GltfSceneExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
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
	CHECK(exported.Unavailable[1].Reason == "source_texture_unavailable");
}

TEST_CASE("glTF scene export retains editable material maps and names unsupported facts", "[script][gltf]") {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store store("script.gltf.textures");
	const auto makeImage = [&](std::string_view name, const std::vector<std::byte> &pixels) {
		const auto entity = store.CreateInstance(engine::scene::EditableImageClass(), name);
		REQUIRE(engine::scene::ResizeEditableImage(store, entity, 1, 1));
		REQUIRE(engine::scene::EditableImageFromBuffer(store, entity, pixels));
		return engine::scene::EditableImageContentName(store, entity);
	};
	const auto colour = makeImage("Colour", {std::byte{10}, std::byte{20}, std::byte{30}, std::byte{40}});
	const auto roughness =
		makeImage("Roughness", {std::byte{70}, std::byte{0}, std::byte{0}, std::byte{255}});
	const auto metalness =
		makeImage("Metalness", {std::byte{90}, std::byte{0}, std::byte{0}, std::byte{255}});
	for (const std::string_view id : {"scene/a", "scene/b"}) {
		const auto part = engine::scene::MakePart(store, {});
		Identify(store, part, id);
		store.Set(
			part,
			engine::scene::SurfaceAppearance{
				.ColourMap = colour,
				.RoughnessMap = roughness,
				.OcclusionMap = colour,
				.MetalnessMap = metalness,
				.AlphaCutoff = 0.4f,
				.Mode = engine::scene::AlphaMode::Transparency,
			}
		);
	}
	const auto unsupported = engine::scene::MakePart(store, {});
	Identify(store, unsupported, "scene/height");
	store.Set(
		unsupported,
		engine::scene::SurfaceAppearance{
			.HeightMap = colour,
			.Mode = engine::scene::AlphaMode::Opaque,
		}
	);

	engine::script::GltfSceneExport exported;
	std::string failure;
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure));
	REQUIRE(exported.Nodes.size() == 2);
	REQUIRE(exported.Textures.size() == 3);
	CHECK(exported.SourceTextures.empty());
	for (const auto &node : exported.Nodes) {
		CHECK(node.Material.AlphaMode == engine::script::GltfExportAlphaMode::Mask);
		CHECK(node.Material.AlphaCutoff == 0.4f);
		CHECK(node.Material.ColourTexture == 0);
		CHECK(node.Material.OcclusionTexture == 1);
		CHECK(node.Material.MetallicRoughnessTexture == 2);
		CHECK(node.Material.RoughnessFactor == 1.0f);
		CHECK(node.Material.MetalnessFactor == 1.0f);
	}
	CHECK(exported.Textures[0].Pixels == std::vector<uint8_t>{56, 79, 96, 40});
	CHECK(exported.Textures[1].Pixels == std::vector<uint8_t>{10, 20, 30, 40});
	CHECK(exported.Textures[2].Pixels == std::vector<uint8_t>{255, 70, 90, 255});
	REQUIRE(exported.Unavailable.size() == 1);
	CHECK(exported.Unavailable[0].StableId == "scene/height");
	CHECK(exported.Unavailable[0].Reason == "surface_height_or_shader_unavailable");
}

TEST_CASE("glTF scene export swizzles packed PBR channels into glTF's fixed layout", "[script][gltf]") {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store store("script.gltf.packed-pbr");
	const auto image = store.CreateInstance(engine::scene::EditableImageClass(), "Packed");
	REQUIRE(engine::scene::ResizeEditableImage(store, image, 1, 1));
	const std::array packedPixels{std::byte{90}, std::byte{30}, std::byte{70}, std::byte{200}};
	REQUIRE(engine::scene::EditableImageFromBuffer(store, image, packedPixels));
	const auto part = engine::scene::MakePart(store, {});
	Identify(store, part, "scene/packed-pbr");
	store.Set(
		part,
		engine::scene::SurfaceAppearance{
			.PackedPbrMap = engine::scene::EditableImageContentName(store, image),
			.RoughnessChannel = 2,
			.OcclusionChannel = 1,
			.MetalnessChannel = 3,
		}
	);

	const auto alternate = engine::scene::MakePart(store, {});
	Identify(store, alternate, "scene/packed-pbr-alternate");
	store.Set(
		alternate,
		engine::scene::SurfaceAppearance{
			.PackedPbrMap = engine::scene::EditableImageContentName(store, image),
			.RoughnessChannel = 0,
			.MetalnessChannel = 1,
		}
	);

	engine::script::GltfSceneExport exported;
	std::string failure;
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure));
	REQUIRE(exported.Nodes.size() == 2);
	bool selected = false;
	bool alternateSelected = false;
	for (const auto &node : exported.Nodes) {
		REQUIRE(node.Material.MetallicRoughnessTexture);
		const auto &pixels = exported.Textures[*node.Material.MetallicRoughnessTexture].Pixels;
		if (pixels == std::vector<uint8_t>{255, 70, 200, 255}) {
			selected = true;
			REQUIRE(node.Material.OcclusionTexture);
			CHECK(
				exported.Textures[*node.Material.OcclusionTexture].Pixels ==
				std::vector<uint8_t>{30, 0, 0, 255}
			);
		} else if (pixels == std::vector<uint8_t>{255, 90, 30, 255}) {
			alternateSelected = true;
		} else {
			FAIL("unexpected packed metallic-roughness texture");
		}
	}
	CHECK(selected);
	CHECK(alternateSelected);
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

TEST_CASE("glTF scene export accepts bounded geometry from its host source", "[script][gltf]") {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store store("script.gltf.delivered");
	const auto part = engine::scene::MakePart(store, {});
	Identify(store, part, "scene/delivered");
	auto visual = *store.Get<engine::scene::Visual>(part);
	visual.Mesh = engine::core::Name("content/delivered.amesh");
	store.Set(part, visual);
	bool called = false;
	const engine::script::GltfMeshSource source =
		[&](std::string_view world, std::string_view name, engine::assets::MeshData &out) {
			called = true;
			CHECK(world == "script.gltf.delivered");
			CHECK(name == "content/delivered.amesh");
			out = engine::assets::MakeBuiltin(engine::assets::BuiltinMesh::Sphere);
			return engine::script::GltfMeshSourceStatus::Available;
		};
	engine::script::GltfSceneExport exported;
	std::string failure;
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure, source));
	CHECK(called);
	REQUIRE(exported.Nodes.size() == 1);
	CHECK(exported.Unavailable.empty());
	REQUIRE(exported.Meshes.size() == 1);
	CHECK(
		exported.Meshes[0].Data.Indices.size() ==
		engine::assets::MakeBuiltin(engine::assets::BuiltinMesh::Sphere).Indices.size()
	);
	const engine::script::GltfMeshSource larger =
		[](std::string_view, std::string_view, engine::assets::MeshData &out) {
			out = engine::assets::MakeBuiltin(engine::assets::BuiltinMesh::Cube);
			out.Vertices.resize(4097, out.Vertices.front());
			return engine::script::GltfMeshSourceStatus::Available;
		};
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure, larger));
	REQUIRE(exported.Nodes.size() == 1);
	CHECK(exported.Meshes[0].Data.Vertices.size() == 4097);
	const engine::script::GltfMeshSource oversized =
		[](std::string_view, std::string_view, engine::assets::MeshData &) {
			return engine::script::GltfMeshSourceStatus::OverLimit;
		};
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure, oversized));
	CHECK(exported.Nodes.empty());
	REQUIRE(exported.Unavailable.size() == 1);
	CHECK(exported.Unavailable[0].Reason == "source_geometry_over_limit");
	const engine::script::GltfMeshSource textured =
		[](std::string_view, std::string_view, engine::assets::MeshData &out) {
			out = engine::assets::MakeBuiltin(engine::assets::BuiltinMesh::Cube);
			engine::assets::Submesh run;
			run.FirstIndex = 0;
			run.IndexCount = static_cast<uint32_t>(out.Indices.size());
			run.Texture = "content/delivered.atex";
			out.Submeshes.push_back(run);
			return engine::script::GltfMeshSourceStatus::Available;
		};
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure, textured));
	REQUIRE(exported.Nodes.size() == 1);
	REQUIRE(exported.Unavailable.size() == 1);
	CHECK(exported.Unavailable[0].Reason == "submesh_texture_unavailable");
}

TEST_CASE("glTF scene export reads delivered image bytes in their declared colour space", "[script][gltf]") {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store store("script.gltf.source-textures");
	const auto part = engine::scene::MakePart(store, {});
	Identify(store, part, "scene/source-textures");
	store.Set(
		part,
		engine::scene::SurfaceAppearance{
			.ColourMap = engine::core::Name("content/srgb.atex"),
			.RoughnessMap = engine::core::Name("content/roughness.atex"),
			.Mode = engine::scene::AlphaMode::Opaque,
		}
	);
	const engine::script::GltfTextureSource source =
		[](std::string_view world, std::string_view name, engine::assets::TextureData &out) {
			CHECK(world == "script.gltf.source-textures");
			out.Width = out.Height = 1;
			if (name == "content/srgb.atex") {
				out.Format = engine::assets::TextureFormat::RGBA8;
				out.Pixels = {std::byte{128}, std::byte{64}, std::byte{32}, std::byte{200}};
			} else if (name == "content/linear.atex") {
				out.Format = engine::assets::TextureFormat::RGBA8_LINEAR;
				out.Pixels = {std::byte{10}, std::byte{20}, std::byte{30}, std::byte{40}};
			} else {
				CHECK(name == "content/roughness.atex");
				out.Format = engine::assets::TextureFormat::R8;
				out.Pixels = {std::byte{64}};
			}
			return engine::script::GltfTextureSourceStatus::Available;
		};
	engine::script::GltfSceneExport exported;
	std::string failure;
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure, {}, source));
	REQUIRE(exported.Nodes.size() == 1);
	CHECK(exported.Unavailable.empty());
	REQUIRE(exported.Textures.size() == 2);
	CHECK(exported.Textures[0].Pixels == std::vector<uint8_t>{128, 64, 32, 200});
	CHECK(exported.Textures[1].Pixels == std::vector<uint8_t>{255, 64, 255, 255});
	CHECK(exported.Nodes[0].Material.ColourTexture == 0);
	CHECK(exported.Nodes[0].Material.MetallicRoughnessTexture == 1);
	auto appearance = *store.Get<engine::scene::SurfaceAppearance>(part);
	appearance.ColourMap = engine::core::Name("content/linear.atex");
	store.Set(part, appearance);
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure, {}, source));
	REQUIRE(exported.Textures.size() == 2);
	CHECK(exported.Textures[0].Pixels == std::vector<uint8_t>{56, 79, 96, 40});
	appearance.ColourMap = engine::core::Name("content/srgb.atex");
	appearance.NormalMap = appearance.ColourMap;
	store.Set(part, appearance);
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure, {}, source));
	REQUIRE(exported.Textures.size() == 3);
	CHECK(exported.Textures[0].Pixels == std::vector<uint8_t>{128, 64, 32, 200});
	CHECK(exported.Textures[1].Pixels == std::vector<uint8_t>{55, 13, 4, 200});
	CHECK(exported.Nodes[0].Material.ColourTexture == 0);
	CHECK(exported.Nodes[0].Material.NormalTexture == 1);
}

TEST_CASE("glTF scene export bounds repeated node geometry before GLB assembly", "[script][gltf]") {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store store("script.gltf.geometry-budget");
	for (size_t index = 0; index < 5; ++index) {
		const auto part = engine::scene::MakePart(store, {});
		Identify(store, part, "scene/large-" + std::to_string(index));
		auto visual = *store.Get<engine::scene::Visual>(part);
		visual.Mesh = engine::core::Name("content/large.amesh");
		store.Set(part, visual);
	}
	engine::assets::MeshData large = engine::assets::MakeBuiltin(engine::assets::BuiltinMesh::Cube);
	large.Vertices.resize(engine::script::MAX_GLTF_EXPORT_VERTICES, large.Vertices.front());
	large.Indices.resize(engine::script::MAX_GLTF_EXPORT_INDICES, 0);
	large.ComputeBounds();
	const engine::script::GltfMeshSource source =
		[&](std::string_view, std::string_view, engine::assets::MeshData &out) {
			out = large;
			return engine::script::GltfMeshSourceStatus::Available;
		};
	engine::script::GltfSceneExport exported;
	std::string failure;
	REQUIRE(engine::script::CaptureGltfSceneExport(store, exported, failure, source));
	CHECK(exported.Nodes.size() == 4);
	REQUIRE(exported.Unavailable.size() == 1);
	CHECK(exported.Unavailable[0].Reason == "scene_geometry_over_limit");
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
