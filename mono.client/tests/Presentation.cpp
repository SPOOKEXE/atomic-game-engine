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
#include <engine/scene/SurfaceCameras.hpp>
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

		// **And `Locked`, which arrived at v0.12 into the same serialiser.**
		// Set to `true` here for `CastShadow`'s reason inverted: the default is
		// `false`, so a reader that dropped the field would come back unlocked
		// and this would fail. A part somebody locked and then saved coming
		// back grabbable is the one thing locking it was for.
		const bool locked = true;
		REQUIRE(store.SetProperty(part, Name("Locked"), &locked, sizeof(locked)));
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

		bool locked = false;
		REQUIRE(store.GetProperty(part, Name("Locked"), &locked, sizeof(locked)));
		CHECK(locked);
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

TEST_CASE("a portal naming another world draws that world's instances", "[client][presentation]") {
	// **The half of a portal that a store cannot do for itself.**
	// `AimSurfaceCameras` places the camera and fits the frustum, and both are
	// arithmetic inside one world; what is drawn through that frustum is a draw
	// list, and another world's draw list is on the far side of a boundary rule
	// 3 keeps shut. The host is the only thing holding both, so the host is what
	// joins them — by appending the far world's instances and telling the
	// surface which range is its own.
	Universe universe;

	const WorldId here = AddWorld(universe, "here");
	const WorldId there = AddWorld(universe, "there");

	AddPart(universe, there, "FarThing");
	AddPart(universe, there, "OtherFarThing");

	universe.Tick(1.0f / 60.0f);
	universe.Present(there, 1.0f / 60.0f, 0.0f);

	// What the far world published, which is what should end up on the end of
	// this world's array.
	size_t published = 0;
	universe.Enter(there, [&published](Store &store) {
		if (const auto *list = store.Resource<client::DrawList>()) {
			published = list->Instances.size();
		}
	});
	REQUIRE(published == 2);

	// A pane with a portal on it, naming the other world. The destination is a
	// local stand-in — it is what the *camera* is placed against, and this test
	// is about what is *drawn*.
	universe.Enter(here, [](Store &store) {
		const Entity pane = store.CreateInstance(engine::scene::PartClass(), "Pane");
		store.SetParent(pane, engine::scene::InstallServices(store));

		const Entity stand = store.CreateInstance(engine::scene::PartClass(), "StandIn");
		store.SetParent(stand, engine::scene::InstallServices(store));

		const Entity camera = store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
		engine::scene::SurfaceCamera target;
		target.Surface = 3;
		store.Set(camera, target);

		engine::scene::Portal portal;
		portal.Destination = stand;
		portal.DestinationWorld = Name("there");
		store.Set(camera, portal);

		store.SetParent(camera, pane);
	});

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);

	std::vector<engine::scene::DrawInstance> instances;
	std::vector<engine::render::SurfaceView> views;

	universe.Enter(here, [&instances, &views](Store &store) {
		if (const auto *list = store.Resource<client::DrawList>()) {
			instances = list->Instances;
		}
		(void)client::CollectSurfaceViews(store, views);
	});

	const size_t own = instances.size();
	REQUIRE_FALSE(views.empty());

	// **Nothing is pointed anywhere until the host does it**, which is the
	// property that keeps every mirror in the engine drawing its own world.
	for (const engine::render::SurfaceView &view : views) {
		CHECK(view.InstanceCount == 0);
	}

	std::vector<engine::scene::DrawInstance> foreign;
	CHECK(client::AttachForeignSurfaces(universe, here, instances, foreign, views) == 1);

	// **The far world's rows do not join this world's**, which is the property
	// the fix turned on: joined, every one of them would be culled against this
	// camera, sorted into this scene's plan and submitted by the screen pass —
	// the two rooms drawn on top of each other.
	//
	// Nothing is appended here either, and that is a statement about this
	// fixture rather than about the pass: the far world has no pane leading
	// back, so nobody over there is standing in a hole into this room. The case
	// where somebody is, is the two-mouthed test below.
	CHECK(instances.size() == own);
	CHECK(foreign.size() == published);

	bool found = false;
	for (const engine::render::SurfaceView &view : views) {
		if (view.Index != 3) {
			CHECK(view.InstanceCount == 0);
			continue;
		}
		found = true;

		// Counted from the start of `foreign`, because nothing out here knows
		// where this world's rows will end up in the instance buffer. The
		// renderer moves the range on by that much once it does.
		CHECK(view.InstanceFirst == 0);
		CHECK(view.InstanceCount == published);
	}
	CHECK(found);
}

TEST_CASE("a portal naming a world that is not there keeps showing its own", "[client][presentation]") {
	// A name matching nothing is the same fallback an unlinked portal already
	// has: the pane shows this world, which reads as a mirror and is visible.
	// Pointing it at an empty range instead would clear the surface to the
	// pass's own colour, which reads as a hole into nothing.
	Universe universe;
	const WorldId here = AddWorld(universe, "here");

	universe.Enter(here, [](Store &store) {
		const Entity pane = store.CreateInstance(engine::scene::PartClass(), "Pane");
		store.SetParent(pane, engine::scene::InstallServices(store));

		const Entity camera = store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
		store.Set(camera, engine::scene::SurfaceCamera{});

		engine::scene::Portal portal;
		portal.DestinationWorld = Name("a world nobody made");
		store.Set(camera, portal);

		store.SetParent(camera, pane);
	});

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);

	std::vector<engine::scene::DrawInstance> instances;
	std::vector<engine::render::SurfaceView> views;
	universe.Enter(here, [&instances, &views](Store &store) {
		if (const auto *list = store.Resource<client::DrawList>()) {
			instances = list->Instances;
		}
		(void)client::CollectSurfaceViews(store, views);
	});

	const size_t own = instances.size();

	std::vector<engine::scene::DrawInstance> foreign;
	CHECK(client::AttachForeignSurfaces(universe, here, instances, foreign, views) == 0);
	CHECK(instances.size() == own);
	CHECK(foreign.empty());
	for (const engine::render::SurfaceView &view : views) {
		CHECK(view.InstanceCount == 0);
	}
}

namespace {
	// A pair of panes facing each other across a hundred units, each a portal
	// into the other. The arrangement every portal example builds.
	//
	// @param store  The world.
	// @param apart  How far the second pane is down +X from the first.
	// @param second Whether to make the far pane a portal back, which is what
	//               gives the first one a partner.
	void MakePortalPair(Store &store, float apart, bool second) {
		const Entity services = engine::scene::InstallServices(store);

		const auto pane = [&](std::string_view name, const Vector3 &at) {
			const Entity part = store.CreateInstance(engine::scene::PartClass(), name);
			store.SetParent(part, services);
			store.Set(part, engine::scene::Transform{engine::core::CFrame{at}});
			store.Set(part, engine::scene::Bounds{Vector3{2.0f, 3.0f, 0.25f}});
			return part;
		};

		const Entity near = pane("Near", Vector3::Zero);
		const Entity far = pane("Far", Vector3{apart, 0.0f, 0.0f});

		const auto hole = [&](std::string_view name, Entity on, Entity to, int8_t slot) {
			const Entity camera =
				store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), name);
			engine::scene::SurfaceCamera target;
			target.Surface = slot;
			store.Set(camera, target);

			engine::scene::Portal portal;
			portal.Destination = to;
			store.Set(camera, portal);

			store.SetParent(camera, on);
		};

		hole("NearHole", near, far, 0);
		if (second) {
			hole("FarHole", far, near, 1);
		}
	}
}

TEST_CASE("a same-world hole leaves the surface path for the recursive one", "[client][presentation]") {
	// **The pivot, stated as a test.** A `SurfaceCamera` is placed from the eye,
	// so when one surface pass draws another pane it projects that pane's image
	// with a matrix taken from the eye rather than from the camera the pass is
	// rendering from — the wrong viewpoint, not a stale one. A same-world portal
	// is therefore drawn by `render::PortalView` and must *not* also arrive as a
	// `SurfaceView`, or the pane is drawn twice and the second answer is wrong.
	Universe universe;
	const WorldId here = AddWorld(universe, "here");

	universe.Enter(here, [](Store &store) { MakePortalPair(store, 100.0f, true); });

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);

	std::vector<engine::render::PortalView> portals;
	std::vector<engine::render::SurfaceView> views;

	universe.Enter(here, [&portals, &views](Store &store) {
		CHECK(client::CollectPortalViews(store, portals) == 2);
		(void)client::CollectSurfaceViews(store, views, portals);
	});

	REQUIRE(portals.size() == 2);
	CHECK(views.empty());

	// Each hole names the other, so the level one opens can skip the pane it is
	// standing at — CodeParade's `skipPortal`.
	CHECK(portals[0].Partner == portals[1].Index);
	CHECK(portals[1].Partner == portals[0].Index);

	// **The rectangle is the seam's, so it is the pane's *face* and not its
	// box.** A quarter of a unit off the part's centre is the half-extent along
	// the face normal, which is what `FaceOf` measures and what the sub-camera's
	// clip plane is put on.
	const engine::render::PortalView &first = portals[0];
	const engine::render::PortalView &second = portals[1];
	CHECK(first.Centre.X == Catch::Approx(0.0f).margin(0.01f));
	CHECK(std::abs(first.Centre.Z) == Catch::Approx(0.25f));
	CHECK(std::abs(first.Normal.Z) == Catch::Approx(1.0f));

	// **The defining property of the map**: the source pane's centre lands on
	// the destination's. Everything the pass does — where the sub-camera stands,
	// where its near plane is skewed to — follows from it.
	const Vector3 landed = first.Warp.Point(first.Centre);
	CHECK(landed.X == Catch::Approx(second.Centre.X).margin(0.01f));
	CHECK(landed.Y == Catch::Approx(second.Centre.Y).margin(0.01f));
	CHECK(landed.Z == Catch::Approx(second.Centre.Z).margin(0.01f));

	// Two panes of one size, so the hole tells no lie about it.
	CHECK(first.Warp.Scale == Catch::Approx(1.0f));

	// **A lap through and back returns you, from either side**, which is what
	// makes it a hole rather than a one-way door. One map per pane is what buys
	// that: a pane's map and its partner's are exact inverses, so the lap closes
	// no matter which face was entered. The pair of side-picked maps this used to
	// carry both landed on the same side of the far pane and were therefore not
	// inverses — a lap that started from behind came back displaced and turned by
	// whatever angle the pair turns through.
	const Vector3 eye{0.0f, 1.0f, 5.0f};
	const Vector3 there = first.Warp.Point(eye);

	// A hundred units along X, because that is where the far room is. **Measured
	// on the axis the pair is laid out on rather than as a distance**, so the
	// case says "the other room" and not "and this far into it" — how far into it
	// is the round trip's business, checked below.
	CHECK(std::abs(there.X - eye.X) == Catch::Approx(100.0f).margin(0.5f));

	const Vector3 home = second.Warp.Point(there);
	CHECK(home.X == Catch::Approx(eye.X).margin(0.01f));
	CHECK(home.Y == Catch::Approx(eye.Y).margin(0.01f));
	CHECK(home.Z == Catch::Approx(eye.Z).margin(0.01f));

	// And the same lap from the pane's other face, which is the case the old
	// arrangement got wrong.
	const Vector3 behind{0.0f, 1.0f, -5.0f};
	const Vector3 across = first.Warp.Point(behind);
	const Vector3 back = second.Warp.Point(across);
	CHECK(back.X == Catch::Approx(behind.X).margin(0.01f));
	CHECK(back.Y == Catch::Approx(behind.Y).margin(0.01f));
	CHECK(back.Z == Catch::Approx(behind.Z).margin(0.01f));
}

TEST_CASE("a hole with no partner still recurses, and a lone pane has none", "[client][presentation]") {
	// A one-way hole is a real arrangement — a pane leading into a room with no
	// pane back — and it must still be drawn recursively. What it has no answer
	// for is which slot the level below should skip, and -1 is that answer
	// rather than a slot number that happens to be zero.
	Universe universe;
	const WorldId here = AddWorld(universe, "here");

	universe.Enter(here, [](Store &store) { MakePortalPair(store, 60.0f, false); });

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);

	std::vector<engine::render::PortalView> portals;
	universe.Enter(here, [&portals](Store &store) {
		CHECK(client::CollectPortalViews(store, portals) == 1);
	});

	REQUIRE(portals.size() == 1);
	CHECK(portals[0].Partner == -1);
}

TEST_CASE("a cross-world pane keeps its surface camera", "[client][presentation]") {
	// **The split the pivot deliberately did not close.** A `DestinationWorld` is
	// a window onto a second simulation rather than a hole in one space: the warp
	// into another world's coordinates is a stated frame and not a derived one,
	// so it does not recurse and `AttachForeignSurfaces` goes on pointing it at
	// the far world's rows.
	Universe universe;
	const WorldId here = AddWorld(universe, "here");
	(void)AddWorld(universe, "there");

	universe.Enter(here, [](Store &store) {
		const Entity services = engine::scene::InstallServices(store);

		const Entity pane = store.CreateInstance(engine::scene::PartClass(), "Pane");
		store.SetParent(pane, services);

		const Entity stand = store.CreateInstance(engine::scene::PartClass(), "StandIn");
		store.SetParent(stand, services);

		const Entity camera = store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
		store.Set(camera, engine::scene::SurfaceCamera{});

		engine::scene::Portal portal;
		portal.Destination = stand;
		portal.DestinationWorld = Name("there");
		store.Set(camera, portal);

		store.SetParent(camera, pane);
	});

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);

	std::vector<engine::render::PortalView> portals;
	std::vector<engine::render::SurfaceView> views;
	universe.Enter(here, [&portals, &views](Store &store) {
		CHECK(client::CollectPortalViews(store, portals) == 0);
		(void)client::CollectSurfaceViews(store, views, portals);
	});

	CHECK(portals.empty());
	CHECK_FALSE(views.empty());
}

TEST_CASE("a cross-world portal carries a body through both of its mouths", "[client][presentation]") {
	// **A hole has two mouths and the host used to assemble one of them.** The
	// clone that puts a body's far half into the picture the glass shows was
	// there; the one that puts the far world's body into *this* room, in front
	// of this world's pane, was not. So walking into the hole from one world
	// worked and standing in the other world watching somebody walk in showed
	// an empty block — a portal that draws from A into B and never back.
	//
	// The two panes deliberately name different surface slots. A slot numbers a
	// camera within one store, so asking the far world for "the pane on slot 2"
	// because that is what this world's pane sits on picks whichever of its
	// cameras happens to share the number. The pane that leads home is the one
	// whose `Portal::DestinationWorld` names this world, and nothing else.
	Universe universe;

	const WorldId here = AddWorld(universe, "two.mouths.here");
	const WorldId there = AddWorld(universe, "two.mouths.there");

	// One pane at the origin with its `Front` face at z = -0.2, a stand-in the
	// same size so the hole does not change scale, and a body standing in the
	// pane. The stand-ins are a hundred units apart in opposite directions, so
	// a clone's position says on its own which mouth produced it.
	const auto build = [&universe](WorldId world, std::string_view other, int8_t slot, float standAtX) {
		universe.Enter(world, [other, slot, standAtX](Store &store) {
			const Entity services = engine::scene::InstallServices(store);

			const Vector3 paneSize{10.0f, 8.0f, 0.4f};

			const Entity pane = store.CreateInstance(engine::scene::PartClass(), "Pane");
			store.SetProperty(pane, Name("Size"), &paneSize, sizeof(paneSize));
			store.SetParent(pane, services);

			const Entity stand = store.CreateInstance(engine::scene::PartClass(), "StandIn");
			const Vector3 standAt{standAtX, 0.0f, 0.0f};
			store.SetProperty(stand, Name("Size"), &paneSize, sizeof(paneSize));
			store.SetProperty(stand, Name("Position"), &standAt, sizeof(standAt));
			store.SetParent(stand, services);

			const Entity camera =
				store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
			engine::scene::SurfaceCamera target;
			target.Surface = slot;
			store.Set(camera, target);

			engine::scene::Portal portal;
			portal.Destination = stand;
			portal.DestinationWorld = Name(other);
			store.Set(camera, portal);
			store.SetParent(camera, pane);

			// **Standing in the pane, which is not a crossing.** The body has
			// not moved at all — `PreviousTransform` is where it is — so the
			// interpolation cannot move the clone whatever the frame's alpha.
			const Entity body = store.CreateInstance(engine::scene::PartClass(), "Body");
			const Vector3 bodySize{1.0f, 2.0f, 1.0f};
			const Vector3 bodyAt{0.0f, 0.0f, -0.1f};
			store.SetProperty(body, Name("Size"), &bodySize, sizeof(bodySize));
			store.SetProperty(body, Name("Position"), &bodyAt, sizeof(bodyAt));
			store.SetParent(body, services);
			store.Set(body, engine::scene::Motion{});
			store.Set(body, engine::scene::PreviousTransform{engine::core::CFrame(bodyAt)});
		});
	};

	build(here, "two.mouths.there", 2, 100.0f);
	build(there, "two.mouths.here", 5, -100.0f);

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);
	universe.Present(there, 1.0f / 60.0f, 0.0f);

	// What each world published for itself, which is what the two lists below
	// are measured against.
	const auto publishedBy = [&universe](WorldId world) {
		size_t count = 0;
		universe.Enter(world, [&count](Store &store) {
			if (const auto *list = store.Resource<client::DrawList>()) {
				count = list->Instances.size();
			}
		});
		return count;
	};

	const size_t ownHere = publishedBy(here);
	const size_t ownThere = publishedBy(there);
	REQUIRE(ownHere > 0);
	REQUIRE(ownThere > 0);

	// The far half of a clone, found by the one thing that identifies it: it is
	// the only row a hundred units out along X.
	const auto cloneAt = [](const std::vector<engine::scene::DrawInstance> &rows, float x) {
		size_t found = 0;
		for (const engine::scene::DrawInstance &row : rows) {
			if (std::abs(row.Frame.Position.X - x) > 0.001f) {
				continue;
			}
			found++;

			// **The same depth into the far pane as into the near one**, which
			// is what makes the two halves meet at the plane rather than
			// overlap or leave a gap — mirrored across it, because the map
			// carries a pane's front hemisphere to the far pane's back one.
			CHECK(row.Frame.Position.Z == Catch::Approx(-0.3f).margin(0.001f));

			// **Never a surface itself**, or the copy would claim the slot its
			// original writes and the two would fight over one texture.
			CHECK(row.Surface == -1);
		}
		return found;
	};

	SECTION("drawn from here") {
		std::vector<engine::scene::DrawInstance> drawn;
		std::vector<engine::render::SurfaceView> views;
		universe.Enter(here, [&drawn, &views](Store &store) {
			if (const auto *list = store.Resource<client::DrawList>()) {
				drawn = list->Instances;
			}
			(void)client::CollectSurfaceViews(store, views);
		});

		std::vector<engine::scene::DrawInstance> foreign;
		CHECK(client::AttachForeignSurfaces(universe, here, drawn, foreign, views) == 1);

		// The mouth that already worked: this world's body, in the picture the
		// glass shows, beyond the far world's own rows.
		CHECK(foreign.size() == ownThere + 1);
		CHECK(cloneAt(foreign, 100.0f) == 1);

		// **The mouth that did not.** The far world's body is standing in the
		// far world's pane, and the half of it that is in this room belongs on
		// the end of this room's list — where it is culled, lit and sorted with
		// everything else here rather than inside the glass.
		CHECK(drawn.size() == ownHere + 1);
		CHECK(cloneAt(drawn, -100.0f) == 1);
	}

	SECTION("drawn from there") {
		// The same claim from the other side, because "it works one way round"
		// is exactly the bug and a test that only looks one way cannot see it.
		std::vector<engine::scene::DrawInstance> drawn;
		std::vector<engine::render::SurfaceView> views;
		universe.Enter(there, [&drawn, &views](Store &store) {
			if (const auto *list = store.Resource<client::DrawList>()) {
				drawn = list->Instances;
			}
			(void)client::CollectSurfaceViews(store, views);
		});

		std::vector<engine::scene::DrawInstance> foreign;
		CHECK(client::AttachForeignSurfaces(universe, there, drawn, foreign, views) == 1);

		CHECK(foreign.size() == ownHere + 1);
		CHECK(cloneAt(foreign, -100.0f) == 1);

		CHECK(drawn.size() == ownThere + 1);
		CHECK(cloneAt(drawn, 100.0f) == 1);
	}
}

TEST_CASE("a cross-world pane is handed every row of the world it names", "[client][presentation]") {
	// **The half a screenshot cannot tell apart from a camera fault.** A pane
	// onto another world shows either what that world published or nothing, and
	// "the far room draws but its spawn pad does not" has two completely
	// different causes: the rows never crossed, or they crossed and the camera
	// did not cover them. This case answers the first, so that a report about
	// the second is about the second.
	//
	// `AttachForeignSurfaces` is the whole of the crossing: it resolves the
	// destination by *name*, copies that world's `DrawList` into a range of its
	// own, and points the surface at it. Nothing here filters, sorts or culls —
	// so if a part is in the far world's draw list it is in the range, and if it
	// is missing from the picture the loss is downstream.
	Universe universe;

	const WorldId here = AddWorld(universe, "presentation.near");
	const WorldId there = AddWorld(universe, "presentation.far");

	// The far world's furniture, named the way the immersive scene names it:
	// a floor everybody sees, a pad that was reported missing, and a marker.
	AddPart(universe, there, "Floor");
	AddPart(universe, there, "SpawnLocation");
	AddPart(universe, there, "Brick");

	// The near world's pane, and the stand-in its camera is aimed at. A
	// cross-world portal has both: `Destination` is a part in *this* world and
	// decides where the camera stands; `DestinationWorld` is a name and decides
	// whose rows are drawn.
	universe.Enter(here, [there, &universe](Store &store) {
		const Entity workspace = engine::scene::InstallServices(store);

		engine::scene::PartDesc pane;
		pane.Size = Vector3{10.0f, 8.0f, 0.4f};
		pane.Frame = engine::core::CFrame(Vector3{0.0f, 4.0f, 0.0f});
		pane.Anchored = true;
		const Entity block = engine::scene::MakePart(store, pane);
		store.SetParent(block, workspace);

		engine::scene::PartDesc stand;
		stand.Size = pane.Size;
		stand.Frame = engine::core::CFrame(Vector3{0.0f, 4.0f, -0.6f});
		stand.Anchored = true;
		const Entity beyond = engine::scene::MakePart(store, stand);
		store.SetParent(beyond, workspace);

		const Entity hole = store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
		engine::scene::SurfaceCamera camera;
		camera.Surface = 0;
		store.Set(hole, camera);

		engine::scene::Portal portal;
		portal.Destination = beyond;
		portal.DestinationWorld = universe.NameOf(there);
		store.Set(hole, portal);
		store.SetParent(hole, block);
	});

	// Both worlds publish, which is what `Editor::PresentPortalDestinations`
	// exists to make true for a world nobody is looking at.
	universe.Present(here, 1.0f / 60.0f, 0.0f);
	universe.Present(there, 1.0f / 60.0f, 0.0f);

	std::vector<engine::render::SurfaceView> views;
	universe.Enter(here, [&views](Store &store) {
		std::vector<engine::render::PortalView> portals;
		(void)client::CollectPortalViews(store, portals);

		// **Empty, and that is the assertion under the assertion.** A
		// cross-world pane is not a `PortalView` — it does not recurse — so it
		// must still be a `SurfaceView`, and a change that swept it onto the
		// recursive path would show up here first.
		CHECK(portals.empty());

		(void)client::CollectSurfaceViews(store, views, portals);
	});

	REQUIRE(views.size() == 1);

	std::vector<engine::scene::DrawInstance> drawn;
	std::vector<engine::scene::DrawInstance> foreign;
	REQUIRE(client::AttachForeignSurfaces(universe, here, drawn, foreign, views) == 1);

	// The far world's whole list, and the range points at all of it.
	const size_t published = Drawn(universe, there);
	REQUIRE(published >= 3);
	CHECK(foreign.size() >= published);
	CHECK(views[0].InstanceCount == static_cast<uint32_t>(published));
	CHECK(views[0].InstanceFirst == 0);
}

TEST_CASE("a hole's picture leaves out the far pane and the stand-in", "[client][presentation]") {
	// **The rule a mirror has always had about itself, which a cross-world pair
	// had nowhere to state — and it blanked the feature outright.**
	//
	// A pair is laid out the same way at both ends: that is what makes a hole
	// read as an opening rather than as a painting, and it is what
	// `ImmersivePortals.luau` does. So the far world's own slab stands exactly
	// where this pane's camera is aimed, at about the distance the frustum is
	// fitted to, and it is the same rectangle that frustum covers. It filled the
	// image edge to edge in one flat colour and hid every room behind it.
	//
	// What that reads as is "the other world does not render its objects" — the
	// floor shows wherever the slab does not quite reach, and nothing else ever
	// does. It survived a correct camera, a correct sampling matrix and a
	// correct foreign range, because all three of those were doing their jobs on
	// a picture of a wall.
	//
	// **Selected by slot rather than by entity**, because a draw instance carries
	// a surface index and no identity — and the slots wanted are exactly the ones
	// `AttachForeignSurfaces` already gathers to bring the far world's straddlers
	// back here.
	Universe universe;

	const WorldId here = AddWorld(universe, "presentation.pair.near");
	const WorldId there = AddWorld(universe, "presentation.pair.far");

	AddPart(universe, there, "Floor");
	AddPart(universe, there, "SpawnLocation");

	// Both worlds get a pane, and each names the other. Without the rule the far
	// one is copied into this one's picture and stands in front of everything.
	const auto pair = [&universe](WorldId world, WorldId other) {
		universe.Enter(world, [&universe, other](Store &store) {
			const Entity workspace = engine::scene::InstallServices(store);

			engine::scene::PartDesc slab;
			slab.Size = Vector3{10.0f, 8.0f, 0.4f};
			slab.Frame = engine::core::CFrame(Vector3{0.0f, 4.0f, 0.0f});
			slab.Anchored = true;
			const Entity block = engine::scene::MakePart(store, slab);
			store.SetInstanceName(block, "PortalBlock");
			store.SetParent(block, workspace);

			engine::scene::PartDesc stand;
			stand.Size = slab.Size;
			stand.Frame = engine::core::CFrame(Vector3{0.0f, 4.0f, -0.6f});
			stand.Anchored = true;
			const Entity beyond = engine::scene::MakePart(store, stand);
			store.SetParent(beyond, workspace);

			// **Invisible, which is how a stand-in is authored and is the whole
			// of the second rule below.** It carries a transform and a size
			// saying where the hole leads and is meant to be seen by nothing.
			if (auto *look = store.GetMutable<engine::scene::Visual>(beyond)) {
				look->Transparency = 1.0f;
			}

			// **An eye, because a surface camera is placed from one.** Without an
			// `ActiveCamera` the aim pass has no viewer to map and assigns no
			// slot — and a pane with no slot is not a pane any filter can see.
			const Entity eye = store.CreateInstance(engine::ecs::Classes::Find(Name("Camera")), "Eye");
			store.Set(eye, engine::scene::Transform{engine::core::CFrame(Vector3{0.0f, 5.0f, 16.0f})});
			store.SetResource(engine::scene::ActiveCamera{eye, 16.0f / 9.0f});

			const Entity hole =
				store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
			store.Set(hole, engine::scene::SurfaceCamera{});

			engine::scene::Portal portal;
			portal.Destination = beyond;
			portal.DestinationWorld = universe.NameOf(other);
			store.Set(hole, portal);
			store.SetParent(hole, block);
		});
	};

	pair(here, there);
	pair(there, here);

	// **Aimed before it is published**, because that is what writes
	// `Visual::Surface` onto a pane — and a draw instance's surface index is the
	// only thing the filter has to recognise a pane by. In a running host
	// `client::InstallControls` registers this; the fixture installs
	// presentation alone, so it is called here.
	for (const WorldId world : {here, there}) {
		universe.Enter(world, [](Store &store) { (void)engine::scene::AimSurfaceCameras(store); });
		universe.Present(world, 1.0f / 60.0f, 0.0f);
	}

	std::vector<engine::render::SurfaceView> views;
	universe.Enter(here, [&views](Store &store) {
		std::vector<engine::render::PortalView> portals;
		(void)client::CollectPortalViews(store, portals);
		(void)client::CollectSurfaceViews(store, views, portals);
	});
	REQUIRE(!views.empty());

	std::vector<engine::scene::DrawInstance> drawn;
	std::vector<engine::scene::DrawInstance> foreign;
	REQUIRE(client::AttachForeignSurfaces(universe, here, drawn, foreign, views) == 1);

	// **Nothing in the picture samples a surface.** The far world's pane is the
	// one row that does, and it is the row that used to fill the hole.
	for (const engine::scene::DrawInstance &instance : foreign) {
		CHECK(instance.Surface < 0);
	}

	// **And nothing invisible is in it, which is the other rule and the other
	// bug.** Every other draw path sends a fully transparent part to the blended
	// run, where an alpha of nothing contributes nothing. This range has no runs
	// — it is one plain draw that bypasses the plan — so an invisible row
	// arrives at the opaque pipeline and draws solid. A cross-world pair has
	// exactly such a row at exactly the worst place: the stand-in sits at the
	// pane, which is where the camera is aimed and the size the frustum is
	// fitted to, so it filled most of the picture with one flat colour.
	for (const engine::scene::DrawInstance &instance : foreign) {
		CHECK(instance.Transparency < 1.0f);
	}

	// And the room is still there — a filter that dropped the far world rather
	// than its pane and its stand-in would pass both lines above and show
	// nothing at all. Two rows fewer, and exactly two.
	CHECK(foreign.size() + 2 == Drawn(universe, there));
	CHECK(views[0].InstanceCount == static_cast<uint32_t>(foreign.size()));
}
