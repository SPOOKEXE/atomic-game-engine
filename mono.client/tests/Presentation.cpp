// The client's half of a world, and what a snapshot restore does to it.
//
// **Written because the studio's Stop broke the viewport and nothing said so.**
// Press Play, press Stop, and the explorer still showed every instance while the
// screen went black — which is the worst shape a bug can have, because the thing
// that is wrong and the thing that looks wrong are in different modules. A
// headless test over the same sequence is where that gets cornered.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>
#include <vector>

TEST_SUITE_ID("client.presentation")

using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;

namespace {
	WorldId AddWorld(Universe &universe, std::string_view name) {
		engine::scene::RegisterSceneClasses();

		WorldSettings settings;
		settings.Name = Name(name);

		const WorldId id = universe.Create(settings);
		universe.Enter(id, [](Store &store, Scheduler &systems) {
			client::InstallPresentation(store, systems, 16);
		});
		return id;
	}

	void AddPart(Universe &universe, WorldId world, std::string_view name) {
		universe.Enter(world, [name](Store &store) {
			const Entity part = store.CreateInstance(engine::scene::PartClass(), name);

			const Vector3 size{8.0f, 2.0f, 4.0f};
			store.SetProperty(part, Name("Size"), &size, sizeof(size));
		});
	}

	// How many instances the world published for its renderer, after one
	// presentation phase.
	size_t Drawn(Universe &universe, WorldId world) {
		universe.Present(world, 1.0f / 60.0f, 0.0f);

		size_t count = 0;
		universe.Enter(world, [&count](Store &store) {
			if (const auto *list = store.Resource<client::DrawList>()) {
				count = list->Instances.size();
			}
		});
		return count;
	}
}

TEST_CASE("a world with presentation installed publishes what it holds", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.basic");
	AddPart(universe, world, "One");
	AddPart(universe, world, "Two");

	CHECK(Drawn(universe, world) == 2);
}

TEST_CASE("a universe survives a snapshot with a draw list in it", "[client][presentation]") {
	// **`DrawList` had no registration at all before v0.7**, so
	// `Store::SetResource` minted one under the compiler's spelling of the type
	// and `Store::Save` refused it for having no serialisation. Nothing noticed
	// until the studio tried to snapshot a world in order to restore it later.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.snapshot");
	AddPart(universe, world, "One");

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));
	CHECK(writer.Bytes().size() > 0);
}

TEST_CASE("what a restore puts back is still drawable", "[client][presentation]") {
	// The studio's Stop, in a test: snapshot, change the world, restore, and
	// ask whether the renderer would see anything. The instances coming back is
	// half the answer and the half a tree view can show; the draw list being
	// refilled is the other half and the half a screenshot showed was missing.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.restore");
	AddPart(universe, world, "Original");

	REQUIRE(Drawn(universe, world) == 1);

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));

	std::vector<std::byte> snapshot(writer.Bytes().begin(), writer.Bytes().end());

	// What "running the game" did to the scene.
	AddPart(universe, world, "MadeWhileRunning");
	REQUIRE(Drawn(universe, world) == 2);

	engine::core::ByteReader reader(snapshot);
	REQUIRE(universe.Load(reader));

	// **The schedulers went with the worlds.** `Universe::Load` clears its
	// registry and adopts fresh worlds, so a restored world has an empty
	// scheduler and publishes nothing until presentation is installed again.
	// That is the studio's job and this is the line that says so.
	const WorldId restored = universe.Find(Name("presentation.restore"));
	REQUIRE(restored.IsValid());

	universe.Enter(restored, [](Store &store, Scheduler &systems) {
		client::InstallPresentation(store, systems, 16);
	});

	CHECK(Drawn(universe, restored) == 1);
}

TEST_CASE("a part's transparency survives a snapshot", "[client][presentation]") {
	// Found while chasing the black viewport above: `scene::Visual` has a
	// custom serialiser, and a field a custom serialiser forgets is a field
	// that silently resets on every load. This is the cheapest possible test
	// for that whole class of bug, and it applies to whatever is added to
	// `Visual` next.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.visual");

	universe.Enter(world, [](Store &store) {
		const Entity part = store.CreateInstance(engine::scene::PartClass(), "Glass");

		const float transparency = 0.75f;
		REQUIRE(store.SetProperty(part, Name("Transparency"), &transparency, sizeof(transparency)));

		const engine::core::Color3 tint{0.9f, 0.35f, 0.15f};
		REQUIRE(store.SetProperty(part, Name("Color"), &tint, sizeof(tint)));
	});

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));
	std::vector<std::byte> snapshot(writer.Bytes().begin(), writer.Bytes().end());

	engine::core::ByteReader reader(snapshot);
	REQUIRE(universe.Load(reader));

	universe.Enter(universe.Find(Name("presentation.visual")), [](Store &store) {
		const Entity part = store.FindFirstRoot("Glass");
		REQUIRE(part != engine::ecs::NULL_ENTITY);

		float transparency = 0.0f;
		REQUIRE(store.GetProperty(part, Name("Transparency"), &transparency, sizeof(transparency)));
		CHECK(transparency == 0.75f);

		engine::core::Color3 tint;
		REQUIRE(store.GetProperty(part, Name("Color"), &tint, sizeof(tint)));
		CHECK(tint.R == 0.9f);
	});
}
