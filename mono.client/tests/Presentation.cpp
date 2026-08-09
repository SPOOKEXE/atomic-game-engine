// The client's half of a world, and what a snapshot restore does to it.
//
// **Written because the studio's Stop broke the viewport and nothing said so.**
// Press Play, press Stop, and the explorer still showed every instance while the
// screen went black — which is the worst shape a bug can have, because the thing
// that is wrong and the thing that looks wrong are in different modules. A
// headless test over the same sequence is where that gets cornered.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

// **A mirror, in a world built the way every host except `--scene` builds
// one.** This is the regression that made the studio's mirror world a plain
// white rectangle: `aim-surface-cameras` was registered only by
// `BuildScriptedWorld`, so the studio, `--game` and an imported world all
// presented a mirror that was never aimed.
//
// The visible half of that is not the camera. Aiming is also what writes
// `Visual::Surface` onto the pane — step 4 of `scene/SurfaceCameras.hpp` — so
// without the system the pane keeps the default of -1, samples no texture, and
// draws as its own flat tint. A white pane looks like a broken surface pass,
// which is why it went to the renderer twice before it came here.
//
// Asserted through `Universe::Present` rather than by calling
// `AimSurfaceCameras` directly, because what was wrong was the registration and
// nothing else: `scene/tests/SurfaceCameras.cpp` already proves the arithmetic,
// and it passed the whole time the mirror was white.
TEST_CASE("a world that only presents still aims its mirrors", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.mirror");
	AddPart(universe, world, "Pane");

	universe.Enter(world, [](Store &store) {
		const Entity workspace = engine::scene::WorkspaceOf(store);
		const Entity pane = store.FindFirstChild(workspace, "Pane");

		// The viewer, which is what there is a reflection *of*. Without one a
		// mirror has nothing to compute rather than a default.
		const Entity eye = store.CreateInstance(engine::scene::CameraClass(), "Eye");
		store.SetParent(eye, workspace);
		store.Set(eye, engine::scene::Transform{engine::core::CFrame(Vector3{0.0f, 0.0f, 20.0f})});
		store.SetResource(engine::scene::ActiveCamera{eye, 16.0f / 9.0f});

		// Parented to the pane and given a face, which is the whole of the
		// setup this feature exists to make sufficient.
		const Entity reflection =
			store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Reflection");
		store.Set(reflection, engine::scene::SurfaceCamera{});
		REQUIRE(store.SetParent(reflection, pane));

		REQUIRE(store.Get<engine::scene::Visual>(pane)->Surface == -1);
	});

	// The pane, plus the marker the surface camera now draws on the face it
	// projects off. The `Camera` is not a part and publishes nothing.
	CHECK(Drawn(universe, world) == 2);

	universe.Enter(world, [](Store &store) {
		const Entity pane = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Pane");

		// The assertion the white rectangle was: a pane told which texture it
		// shows, by nothing more than a camera being parented to it.
		CHECK(store.Get<engine::scene::Visual>(pane)->Surface == 0);

		const auto *list = store.Resource<client::DrawList>();
		REQUIRE(list != nullptr);

		// And it reaches the draw list, which is the half `Visual` alone does
		// not prove — the renderer reads the copy, not the component.
		const auto pane_drawn = std::find_if(
			list->Instances.begin(), list->Instances.end(), [](const engine::scene::DrawInstance &instance) {
				return instance.Surface == 0;
			}
		);
		CHECK(pane_drawn != list->Instances.end());

		// The marker is blended and shows no surface of its own, which is what
		// keeps it out of the surface pass and therefore out of every mirror.
		const auto marker = std::find_if(
			list->Instances.begin(), list->Instances.end(), [](const engine::scene::DrawInstance &instance) {
				return instance.Surface < 0 && instance.Transparency > 0.0f;
			}
		);
		REQUIRE(marker != list->Instances.end());
		CHECK_FALSE(marker->CastShadow);
	});
}

// --- authoring a transform without a tick ------------------------------------

TEST_CASE("a part moved without a tick is drawn where it was moved to", "[client][presentation]") {
	// **The editor's whole case, and it was drawing every part in the wrong
	// place.** `World::Present` runs `PreRender` alone; `capture-previous` is a
	// `PreSimulation` system, so a *suspended* world — which is what the studio
	// shows while you author — never updates `PreviousTransform`. The draw list
	// interpolates from that stale value toward the current one, and a suspended
	// world's alpha does not advance either, so a part dragged in the properties
	// panel stayed exactly where it started while its selection outline — which
	// reads `Transform` directly — moved away from it.
	//
	// Two parts of the frame disagreeing about where something is reads as a
	// renderer fault, which is the most expensive kind of wrong place to look.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.authored");
	AddPart(universe, world, "Dragged");

	// A first frame, so the entity has been presented at least once.
	universe.Present(world, 1.0f / 60.0f, 1.0f);

	universe.Enter(world, [](Store &store) {
		const Entity part = InScene(store, "Dragged");
		REQUIRE(part != engine::ecs::NULL_ENTITY);

		const Vector3 moved{12.0f, 3.0f, -5.0f};
		REQUIRE(store.SetProperty(part, Name("Position"), &moved, sizeof(moved)));
	});

	// **Alpha one, which is what a world that is not simulating has to be
	// presented at.** There is nothing to interpolate *towards* when no tick is
	// coming; the current transform is the whole truth. `Editor::Present` passes
	// this for a suspended world.
	universe.Present(world, 1.0f / 60.0f, 1.0f);

	universe.Enter(world, [](Store &store) {
		const auto *list = store.Resource<client::DrawList>();
		REQUIRE(list != nullptr);
		REQUIRE(list->Instances.size() == 1);
		CHECK(list->Instances[0].Frame.Position.X == Catch::Approx(12.0f));
		CHECK(list->Instances[0].Frame.Position.Y == Catch::Approx(3.0f));
		CHECK(list->Instances[0].Frame.Position.Z == Catch::Approx(-5.0f));
	});
}

TEST_CASE("a scripted move still interpolates across a tick", "[client][presentation]") {
	// **The case that refuted the obvious fix.** Making an authored write clear
	// `PreviousTransform` — "a teleport, not a simulation step" — reads well and
	// breaks every scripted animation in the engine: `examples/Rings.luau` sets
	// `CFrame` once a tick and relies on the draw list interpolating between
	// ticks, which is what buys smooth motion at 300 frames a second over a
	// 60 Hz simulation. Clearing it turns all of that into stepped motion at the
	// tick rate.
	//
	// So a property write moves `Transform` and nothing else, and the editor's
	// problem — a suspended world whose previous frame is never captured — is
	// fixed by presenting such a world at alpha one instead. `PlaceInstance`
	// carries both halves.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.interpolated");
	AddPart(universe, world, "Animated");

	universe.Present(world, 1.0f / 60.0f, 1.0f);

	universe.Enter(world, [](Store &store) {
		const Entity part = InScene(store, "Animated");
		const Vector3 moved{40.0f, 0.0f, 0.0f};
		REQUIRE(store.SetProperty(part, Name("Position"), &moved, sizeof(moved)));
	});

	// Halfway between the frame it was at and the frame it was put at.
	universe.Present(world, 1.0f / 60.0f, 0.5f);

	universe.Enter(world, [](Store &store) {
		const auto *list = store.Resource<client::DrawList>();
		REQUIRE(list != nullptr);
		REQUIRE(list->Instances.size() == 1);
		CHECK(list->Instances[0].Frame.Position.X == Catch::Approx(20.0f));
	});
}

// --- derived state a world that never ticks still needs -----------------------
//
// **Three passes had the same bug and one of them shipped as a visible fault.**
// A studio in Edit mode never ticks: `Editor::Simulate` returns before
// `Universe::Tick`, and `World::Present` runs `PreRender` alone. So a pass
// registered in `PreSimulation` does not run at all while somebody is
// authoring — and everything downstream of it reads whatever the component was
// last left holding, which for something the editor just made is the type's
// default.
//
// `PreviousTransform` was the one that got noticed, because "the part draws at
// the origin" is impossible to miss. These two are the same shape and were
// quieter: a material that does nothing until you press Play, and a lamp that
// lights the origin instead of the part it hangs off.
//
// The tests present without ever ticking, which is exactly what the studio does.

TEST_CASE("a material assigned without a tick reaches the draw list", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.material");
	AddPart(universe, world, "Crate");

	const Name asset("materials/oak.amat");
	const Name colour("materials/oak_Color.atex");

	universe.Enter(world, [&asset, &colour](Store &store) {
		REQUIRE(engine::scene::RecordMaterial(store, asset, engine::scene::MaterialMaps{.Colour = colour}));

		const Entity crate = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Crate");
		const Entity material = store.CreateInstance(engine::scene::MaterialClass(), "Oak");
		REQUIRE(store.SetParent(material, crate));

		auto *ref = store.GetMutable<engine::scene::MaterialRef>(material);
		REQUIRE(ref != nullptr);
		ref->Asset = asset;
	});

	REQUIRE(Drawn(universe, world) == 1);

	universe.Enter(world, [&colour](Store &store) {
		// The component the resolve pass writes...
		const Entity crate = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Crate");
		CHECK(store.Get<engine::scene::SurfaceAppearance>(crate)->ColourMap == colour);

		// ...and the copy of it the renderer actually samples, which is the half
		// that decides whether anything looks different on screen.
		const auto *list = store.Resource<client::DrawList>();
		REQUIRE(list != nullptr);
		REQUIRE(list->Instances.size() == 1);
		CHECK(list->Instances[0].Texture == colour);
	});
}

TEST_CASE("a light on an attachment is placed without a tick", "[client][presentation]") {
	// `Attachment::WorldFrame` is a cache with one writer, and `CollectLights`
	// reads it rather than walking the hierarchy per lamp. Unresolved, it is the
	// identity — so every lamp in an edited world lit the origin, whatever it
	// was actually parented to.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.lamp");
	AddPart(universe, world, "Post");

	const Vector3 stood{10.0f, 0.0f, -4.0f};
	const Vector3 raised{0.0f, 6.0f, 0.0f};

	universe.Enter(world, [&stood, &raised](Store &store) {
		const Entity post = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Post");
		REQUIRE(store.SetProperty(post, Name("Position"), &stood, sizeof(stood)));

		const Entity point = store.CreateInstance(engine::ecs::Classes::Find(Name("Attachment")), "Top");
		REQUIRE(store.SetParent(point, post));
		store.GetMutable<engine::scene::Attachment>(point)->Frame = engine::core::CFrame(raised);

		const Entity bulb = store.CreateInstance(engine::ecs::Classes::Find(Name("PointLight")), "Bulb");
		REQUIRE(store.SetParent(bulb, point));
	});

	universe.Present(world, 1.0f / 60.0f, 1.0f);

	std::vector<engine::render::SceneLight> lights;
	universe.Enter(world, [&lights](Store &store) {
		CHECK(client::CollectLights(store, Vector3{}, lights) == 1);
	});

	REQUIRE(lights.size() == 1);
	CHECK(lights[0].Position.X == Catch::Approx(stood.X + raised.X));
	CHECK(lights[0].Position.Y == Catch::Approx(stood.Y + raised.Y));
	CHECK(lights[0].Position.Z == Catch::Approx(stood.Z + raised.Z));
}
