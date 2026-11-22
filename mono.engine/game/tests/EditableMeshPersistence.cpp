// A world saves EditableMesh geometry, EditableImage pixels, and the parts that name them.

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.game.editablemeshpersistence")

namespace {
	using namespace engine;

	bool SetByName(
		ecs::Store &store, ecs::Entity instance, std::string_view name, const game::PropertyValue &value
	) {
		for (const auto &descriptor : ecs::Classes::Describe(store.ClassOf(instance)).Properties)
			if (descriptor.Name.Text() == name)
				return game::WriteProperty(store, instance, descriptor, value);
		return false;
	}
}

TEST_CASE(
	"an editable mesh, image and the part using them survive a world document", "[game][editable-mesh]"
) {
	game::RegisterGameClasses();
	world::Universe universe;
	world::WorldSettings settings;
	settings.Name = core::Name("Meshes");
	const world::WorldId original = universe.Create(settings);
	REQUIRE(original.IsValid());

	scene::EditableMeshGeometry geometry;
	geometry.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
	geometry.Normals = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
	geometry.UVs = {{0, 0}, {1, 0}, {0, 1}};
	geometry.Colours = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	geometry.Alphas = {1, 1, 0.5f};
	geometry.Indices = {0, 2, 1};
	geometry.Joints = {{0, 1, 0, 0}, {1, 0, 0, 0}, {2, 3, 0, 0}};
	geometry.Weights = {{0.5f, 0.5f, 0, 0}, {1, 0, 0, 0}, {0.25f, 0.75f, 0, 0}};
	geometry.Attributes[1] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
	geometry.Attributes[3] = {{-1, 0, 1, 2}, {3, 4, 5, 6}, {7, 8, 9, 10}};

	universe.Enter(original, [&](ecs::Store &store) {
		const ecs::Entity workspace = scene::InstallServices(store);
		// Spend some entity ids first, so the mesh is not the same id after a load.
		for (int index = 0; index < 5; ++index)
			store.CreateInstance(scene::PartClass(), "Filler");
		const ecs::Entity part = store.CreateInstance(ecs::Classes::Find(core::Name("MeshPart")), "Ground");
		REQUIRE(store.SetParent(part, workspace));
		const ecs::Entity mesh = store.CreateInstance(scene::EditableMeshClass(), "GroundMesh");
		REQUIRE(store.SetParent(mesh, part));
		REQUIRE(scene::ReplaceEditableMesh(store, mesh, geometry) == scene::EditableMeshCommit::Applied);
		const std::array<uint32_t, 3> welds{0, 1, 0};
		REQUIRE(
			scene::SetEditableVertexStream(
				store, mesh, scene::EditableVertexStream::Weld, std::as_bytes(std::span(welds))
			)
		);
		game::PropertyValue content;
		content.Type = ecs::PropertyType::Name;
		content.Name = scene::EditableMeshContentName(store, mesh);
		REQUIRE(SetByName(store, part, "MeshId", content));
		REQUIRE(SetByName(store, part, "CollisionGeometry", content));

		const ecs::Entity image = store.CreateInstance(scene::EditableImageClass(), "GroundImage");
		REQUIRE(store.SetParent(image, part));
		const std::vector<std::byte> pixels{
			std::byte{10},
			std::byte{20},
			std::byte{30},
			std::byte{255},
			std::byte{40},
			std::byte{50},
			std::byte{60},
			std::byte{128}
		};
		REQUIRE(scene::SetEditableImagePixels(store, image, 2, 1, pixels, scene::EditableImageSpace::SRGB));
		game::PropertyValue texture;
		texture.Type = ecs::PropertyType::Name;
		texture.Name = scene::EditableImageContentName(store, image);
		REQUIRE(SetByName(store, part, "TextureID", texture));
	});

	std::string error;
	const std::string document = game::WriteWorldDocument(universe, original, error);
	INFO(error);
	REQUIRE_FALSE(document.empty());
	// Saved by item, never by the entity id this process happened to mint.
	CHECK(document.find("editable-mesh://#") != std::string::npos);

	REQUIRE(universe.Destroy(original) == world::WorldStatus::Ok);
	// A busier world, so ids differ from the original's.
	world::WorldSettings other;
	other.Name = core::Name("Other");
	const world::WorldId busy = universe.Create(other);
	universe.Enter(busy, [](ecs::Store &store) {
		for (int index = 0; index < 40; ++index)
			store.CreateInstance(scene::PartClass(), "Noise");
	});
	const world::WorldId restored = game::ReadWorldDocument(universe, document, core::Name{}, error);
	INFO(error);
	REQUIRE(restored.IsValid());

	universe.Enter(restored, [&](ecs::Store &store) {
		const ecs::Entity part = store.FindFirstChild(scene::WorkspaceOf(store), "Ground");
		REQUIRE(part != ecs::NULL_ENTITY);
		const ecs::Entity mesh = store.FindFirstChild(part, "GroundMesh");
		REQUIRE(mesh != ecs::NULL_ENTITY);
		const auto *held = store.Get<scene::EditableMesh>(mesh);
		REQUIRE(held != nullptr);
		CHECK(held->Positions == geometry.Positions);
		CHECK(held->Colours == geometry.Colours);
		CHECK(held->Alphas == geometry.Alphas);
		CHECK(held->Indices == geometry.Indices);
		CHECK(held->Joints == geometry.Joints);
		CHECK(held->Weights == geometry.Weights);
		CHECK(held->Attributes == geometry.Attributes);
		CHECK(held->Welds == std::vector<uint32_t>{0, 1, 0});
		const core::Name expected = scene::EditableMeshContentName(store, mesh);
		CHECK(store.Get<scene::Visual>(part)->Mesh == expected);
		CHECK(store.Get<scene::Collider>(part)->Geometry == expected);

		const ecs::Entity image = store.FindFirstChild(part, "GroundImage");
		REQUIRE(image != ecs::NULL_ENTITY);
		const auto *heldImage = store.Get<scene::EditableImage>(image);
		REQUIRE(heldImage != nullptr);
		CHECK(heldImage->Width == 2);
		CHECK(heldImage->Height == 1);
		CHECK(heldImage->Space == scene::EditableImageSpace::SRGB);
		CHECK(heldImage->Pixels == std::vector<uint8_t>{10, 20, 30, 255, 40, 50, 60, 128});
		engine::core::Name texture;
		REQUIRE(store.GetProperty(part, engine::core::Name("TextureID"), &texture, sizeof(texture)));
		CHECK(texture == scene::EditableImageContentName(store, image));
	});
}
