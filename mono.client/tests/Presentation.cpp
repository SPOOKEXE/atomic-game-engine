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
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>
#include <string_view>
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

	// Where a script's content lives now.
	//
	// **`part.Parent = workspace` used to make a root and now makes a child of
	// the `Workspace` service**, so a lookup by root finds nothing. See
	// `script/Bindings.hpp`'s `OpenWorkspace` for why the two notions of "the
	// workspace" were collapsed, and `scene/Visibility.hpp` for what the tree
	// now decides.
	//
	// Falls back to a root, because some of these scripts deliberately leave an
	// instance unparented — an orphan is still reachable from C++ through
	// `EachRoot`, and only a *script* is unable to list one. A test about
	// signals or tasks should not have to care which of the two its fixture is.
	Entity InScene(Store &store, std::string_view name) {
		const Entity workspace = engine::scene::WorkspaceOf(store);
		if (workspace != engine::ecs::NULL_ENTITY) {
			if (const Entity child = store.FindFirstChild(workspace, name);
				child != engine::ecs::NULL_ENTITY) {
				return child;
			}
		}
		return store.FindFirstRoot(name);
	}
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

	// A part, **in the scene** — which since v0.7 means under `Workspace`
	// rather than merely alive in the world.
	//
	// The parenting is the fixture's job and not a detail of it: a draw list is
	// the `Workspace` subtree now, so a part created and left unparented is one
	// the world is entitled to publish nothing for. See
	// `scene/Visibility.hpp`; `AddOrphan` below is the other half of the same
	// statement.
	void AddPart(Universe &universe, WorldId world, std::string_view name) {
		universe.Enter(world, [name](Store &store) {
			const Entity part = store.CreateInstance(engine::scene::PartClass(), name);

			const Vector3 size{8.0f, 2.0f, 4.0f};
			store.SetProperty(part, Name("Size"), &size, sizeof(size));

			store.SetParent(part, engine::scene::InstallServices(store));
		});
	}

	// A part that is complete and belongs to nothing.
	void AddOrphan(Universe &universe, WorldId world, std::string_view name) {
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

// **The rule the renderer had never been told**, and the reason
// `scene/Visibility.hpp` exists: a draw list is the `Workspace` subtree, not
// every entity that happens to carry the right components.
//
// Before v0.7 both of these drew. A part in `ReplicatedStorage`, a template
// under `StarterGui` and an orphan a script had made and not yet parented were
// all complete parts by a component test, so all of them were on screen.
TEST_CASE("only the Workspace subtree is published", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.gate");

	AddPart(universe, world, "InScene");
	AddOrphan(universe, world, "Orphan");

	CHECK(Drawn(universe, world) == 1);

	// Parenting it in is what makes it appear, and nothing else has to happen:
	// no component is added, no flag is set by the caller, and the part was
	// complete the whole time.
	universe.Enter(world, [](Store &store) {
		const Entity orphan = store.FindFirstRoot("Orphan");
		REQUIRE(orphan != engine::ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(orphan, engine::scene::WorkspaceOf(store)));
	});

	CHECK(Drawn(universe, world) == 2);

	// And taking it out again removes it, which is the half a set of hooks on
	// the *parenting* side would most easily get wrong — the gate is derived
	// from the tree every pass rather than maintained by whoever moved
	// something.
	universe.Enter(world, [](Store &store) {
		const Entity orphan = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Orphan");
		REQUIRE(orphan != engine::ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(orphan, engine::ecs::NULL_ENTITY));
	});

	CHECK(Drawn(universe, world) == 1);
}

// A whole model moving is the case a per-instance hook cannot answer: nothing
// reparented the parts, and every one of them changed scene.
TEST_CASE("a subtree follows its ancestor in and out of the scene", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.subtree");

	universe.Enter(world, [](Store &store) {
		const Entity model = store.CreateInstance(engine::scene::PartClass(), "Model");
		for (int index = 0; index < 3; index++) {
			const Entity child = store.CreateInstance(engine::scene::PartClass(), "Limb");
			REQUIRE(store.SetParent(child, model));
		}
		REQUIRE(store.SetParent(model, engine::scene::InstallServices(store)));
	});

	// The model and its three limbs.
	CHECK(Drawn(universe, world) == 4);

	universe.Enter(world, [](Store &store) {
		const Entity model = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Model");
		REQUIRE(store.SetParent(model, engine::ecs::NULL_ENTITY));
	});

	CHECK(Drawn(universe, world) == 0);
}

// `Visible` was declared, bound, saved and reloaded from v0.4, and no draw path
// read it. It is a term of the gate now rather than a branch in a loop — see
// `scene/Visibility.hpp` on why a tag and not a boolean test.
TEST_CASE("an invisible part in the Workspace is not published", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.visible");
	AddPart(universe, world, "Seen");
	AddPart(universe, world, "Hidden");

	CHECK(Drawn(universe, world) == 2);

	universe.Enter(world, [](Store &store) {
		const Entity hidden = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Hidden");
		const bool visible = false;
		REQUIRE(store.SetProperty(hidden, Name("Visible"), &visible, sizeof(visible)));
	});

	CHECK(Drawn(universe, world) == 1);

	// **Still collides, still exists, still has its transform.** `Visible` and
	// `Transparency` are different questions and neither is "delete it" — a
	// draw path that treated one as the other would give invisible parts no
	// physics.
	universe.Enter(world, [](Store &store) {
		const Entity hidden = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Hidden");
		CHECK(hidden != engine::ecs::NULL_ENTITY);
		CHECK(store.Get<engine::scene::Collider>(hidden) != nullptr);
	});
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

		// **`CastShadow` is the next field this was written for.** It arrived at
		// v0.7 into the same hand-written serialiser, and it is off here rather
		// than on so that a reader that silently dropped it would come back
		// `true` and fail — a check against the default is no check at all.
		const bool casts = false;
		REQUIRE(store.SetProperty(part, Name("CastShadow"), &casts, sizeof(casts)));

		// Same for `Visible`, which had no coverage here either.
		const bool visible = false;
		REQUIRE(store.SetProperty(part, Name("Visible"), &visible, sizeof(visible)));
	});

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));
	std::vector<std::byte> snapshot(writer.Bytes().begin(), writer.Bytes().end());

	engine::core::ByteReader reader(snapshot);
	REQUIRE(universe.Load(reader));

	universe.Enter(universe.Find(Name("presentation.visual")), [](Store &store) {
		const Entity part = InScene(store, "Glass");
		REQUIRE(part != engine::ecs::NULL_ENTITY);

		float transparency = 0.0f;
		REQUIRE(store.GetProperty(part, Name("Transparency"), &transparency, sizeof(transparency)));
		CHECK(transparency == 0.75f);

		engine::core::Color3 tint;
		REQUIRE(store.GetProperty(part, Name("Color"), &tint, sizeof(tint)));
		CHECK(tint.R == 0.9f);

		bool casts = true;
		REQUIRE(store.GetProperty(part, Name("CastShadow"), &casts, sizeof(casts)));
		CHECK_FALSE(casts);

		bool visible = true;
		REQUIRE(store.GetProperty(part, Name("Visible"), &visible, sizeof(visible)));
		CHECK_FALSE(visible);
	});
}

// `CastShadow` reaching the renderer at all, which is a different question from
// it surviving a file: the draw list is a flat copy of the `Visual` and a field
// left out of *that* is one the shadow pass can never see. `Transparency` and
// `Surface` were both missing from the replica's copy of this list for exactly
// that reason.
TEST_CASE("what a part looks like reaches the draw list whole", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.drawfields");
	AddPart(universe, world, "Pane");

	universe.Enter(world, [](Store &store) {
		const Entity pane = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Pane");

		const float transparency = 0.5f;
		REQUIRE(store.SetProperty(pane, Name("Transparency"), &transparency, sizeof(transparency)));

		const bool casts = false;
		REQUIRE(store.SetProperty(pane, Name("CastShadow"), &casts, sizeof(casts)));
	});

	REQUIRE(Drawn(universe, world) == 1);

	universe.Enter(world, [](Store &store) {
		const auto *list = store.Resource<client::DrawList>();
		REQUIRE(list != nullptr);
		REQUIRE(list->Instances.size() == 1);

		CHECK(list->Instances[0].Transparency == 0.5f);
		CHECK_FALSE(list->Instances[0].CastShadow);
	});
}
