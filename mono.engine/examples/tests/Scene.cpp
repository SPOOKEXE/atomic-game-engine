// The example scenes, loaded.
//
// **A scene that fails to load is one nobody notices is broken** until they run
// the client and see a black screen. Neither the shadow pass nor the surface
// pass can be asserted against without a GPU — `AGENTS.md` names that exception
// and refuses a mock renderer to close it on paper — so what this suite asserts
// is what can be: that each scene builds the *inputs* those passes need, in the
// world, through the same bindings a game would use.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.examples.scene")
TEST_DEPENDS("engine.script.scripting")

using Catch::Approx;
using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::examples::ExamplePath;
using engine::examples::LoadScene;
using engine::scene::ActiveCamera;
using engine::scene::SurfaceCamera;
using engine::scene::Visual;
using engine::scene::WorldBounds;

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
	// The staged assets root, not the test binary's own directory.
	//
	// `Paths::Assets` defaults to where the running executable sits, and a test
	// binary sits in `tests/` while the examples stage into `assets/`. A client
	// finds them because a client stages beside them; this has to be told.
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	size_t CountNamed(Store &store, const char *name) {
		size_t found = 0;
		store.Each<const Visual>([&](Entity entity, const Visual &) {
			if (store.InstanceNameOf(entity) == Name(name)) {
				found++;
			}
		});
		return found;
	}

	// The same count over the 2D tree.
	//
	// **A separate helper rather than a wider query, because the two trees have
	// no component in common and that is the design.** A `Part` is named by its
	// `scene::Visual` and a `TextButton` by its `gui::Element`; a counter that
	// took both would have to name a class rather than a component, which is
	// the coupling `gui/AGENTS.md` spends a section refusing.
	size_t CountElements(Store &store, const std::string &name) {
		size_t found = 0;
		store.Each<const engine::gui::Element>([&](Entity entity, const engine::gui::Element &) {
			if (store.InstanceNameOf(entity) == Name(name)) {
				found++;
			}
		});
		return found;
	}

	// The first element of a given name, or null.
	Entity FirstElement(Store &store, const char *name) {
		Entity found = engine::ecs::NULL_ENTITY;
		store.Each<const engine::gui::Element>([&](Entity entity, const engine::gui::Element &) {
			if (found == engine::ecs::NULL_ENTITY && store.InstanceNameOf(entity) == Name(name)) {
				found = entity;
			}
		});
		return found;
	}
}

TEST_CASE("the rings scene builds and moves itself", "[examples][scene]") {
	const StagedAssets assets;

	Store store("rings");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Rings.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	CHECK(CountNamed(store, "Orbiter") == 512);

	// The bounds are measured from what the script built rather than declared
	// by it, so a scene that produced nothing would frame a world one unit
	// across — which is exactly the bug the settle beat in `LoadScene` exists
	// for.
	REQUIRE(store.Resource<WorldBounds>() != nullptr);
	CHECK(store.Resource<WorldBounds>()->HalfExtent > 5.0f);
}

TEST_CASE("the mirrors scene builds what the render passes need", "[examples][scene]") {
	const StagedAssets assets;

	Store store("mirrors");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Mirrors-1-world.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// The four walls, and the face each one points at the origin. **Stated here
	// as a table rather than checked one wall at a time**, because the thing
	// worth pinning is that all six `NormalId` axes resolve the same way: the
	// walls are unrotated, so a face is the inward normal directly, and a
	// regression that swapped `Left` and `Right` would leave a scene that still
	// loads and reflects the outside of the room.
	struct Wall {
		const char *Name;
		engine::scene::NormalId Face;
	};
	const Wall walls[] = {
		{"MirrorNorth", engine::scene::NormalId::Back},
		{"MirrorEast", engine::scene::NormalId::Left},
		{"MirrorWest", engine::scene::NormalId::Right},
		{"MirrorSouth", engine::scene::NormalId::Front},
	};

	Entity north = engine::ecs::NULL_ENTITY;
	Entity lowest = engine::ecs::NULL_ENTITY;

	for (const Wall &wall : walls) {
		INFO(wall.Name);

		const Entity pane = InScene(store, wall.Name);
		REQUIRE(pane != engine::ecs::NULL_ENTITY);

		// The surface camera **parented to the pane** rather than standing
		// beside it in the workspace. That is the arrangement `SurfaceCamera` is
		// for: the camera is placed off a face of its parent, so it is a child
		// of the thing it reflects.
		const Entity reflection = store.FindFirstChild(pane, "Reflection");
		REQUIRE(reflection != engine::ecs::NULL_ENTITY);

		const auto *surface = store.Get<SurfaceCamera>(reflection);
		REQUIRE(surface != nullptr);
		CHECK(surface->Face == wall.Face);

		// **Wide rather than square, and all four the same** — because all four
		// walls are the same shape, not because they share a target. Each index
		// owns its own pair since v0.8.
		CHECK(surface->Width == 2048);
		CHECK(surface->Height == 512);

		// **The pane is told what it shows by the aiming pass, not by the
		// script.** Before it runs there is nothing to sample, which is why this
		// asserts either side of the call rather than only after: a scene that
		// arrived with `Surface` already set would pass the later check while
		// proving nothing about the mechanism.
		CHECK(store.Get<Visual>(pane)->Surface == -1);

		if (lowest == engine::ecs::NULL_ENTITY || reflection.Id < lowest.Id) {
			lowest = reflection;
		}
		if (std::string_view(wall.Name) == "MirrorNorth") {
			north = reflection;
		}
	}

	// The wall the viewer faces is still built first, which is worth keeping as
	// an assertion even though nothing depends on it any more: it used to decide
	// which of the four mirrors was the live one, because the pipeline drew a
	// single surface view and picked the lowest entity id to fill it. All four
	// are live now, so this is a statement about the scene's build order and no
	// longer about what renders.
	CHECK(lowest == north);

	// Every pane is aimed, and every one of them can now be rendered.
	REQUIRE(engine::scene::AimSurfaceCameras(store) == 4);

	// **A surface each, and the indices are the script's rather than the
	// engine's.** This is what makes four walls four mirrors: they shared index
	// 0 until v0.8, so all four cameras wrote into one texture and three of them
	// projected the fourth's image across themselves.
	//
	// Asserted against the *camera's* authored index rather than against a
	// literal, because what is being checked is the copy — `AimSurfaceCameras`
	// putting the camera's number onto the pane it is parented to is the step
	// that makes a mirror a camera parented to a part and nothing else.
	bool seen[engine::scene::MAX_SURFACES] = {};
	for (const Wall &wall : walls) {
		INFO(wall.Name);

		const Entity pane = InScene(store, wall.Name);
		const Entity reflection = store.FindFirstChild(pane, "Reflection");
		REQUIRE(reflection != engine::ecs::NULL_ENTITY);

		const int8_t index = store.Get<SurfaceCamera>(reflection)->Surface;
		REQUIRE(index >= 0);
		REQUIRE(static_cast<size_t>(index) < engine::scene::MAX_SURFACES);

		CHECK(store.Get<Visual>(pane)->Surface == index);

		// **Distinct, which is the half an equality check cannot see.** Four
		// panes each carrying the index of the camera above them would pass
		// every check so far while all four indices were 0 — which is exactly
		// the state this scene was in before, and exactly what looked like three
		// broken mirrors.
		CHECK_FALSE(seen[index]);
		seen[index] = true;
	}

	// **The live camera is not a surface one.** That is the distinction
	// `ActiveCamera` exists for, and getting it wrong would render the scene
	// from inside a mirror.
	const auto *active = store.Resource<ActiveCamera>();
	REQUIRE(active != nullptr);
	CHECK(active->Entity == InScene(store, "Viewer"));
	CHECK(active->Entity != north);

	// A baseplate, because a shadow needs something to fall on — a scene of
	// floating cubes has nothing, and the shadow pass would look broken.
	CHECK(InScene(store, "Baseplate") != engine::ecs::NULL_ENTITY);
	CHECK(CountNamed(store, "Caster") == 24);

	// **The cubes are in the middle, and this is the assertion that says so.**
	// The enclosure is only legible while the casters stay clear of the glass:
	// one drifting into a pane reads as a broken reflection rather than as a
	// cube in the wrong place, and the two look identical from the viewer's
	// chair. The bound is the authored spread plus the largest half-edge, so it
	// fails on a placement change rather than on a rendering one.
	constexpr float FURTHEST = 13.0f;
	size_t counted = 0;
	store.Each<const engine::scene::Transform, const Visual>(
		[&](Entity entity, const engine::scene::Transform &placement, const Visual &) {
			if (store.InstanceNameOf(entity) != Name("Caster")) {
				return;
			}
			counted++;
			CHECK(std::abs(placement.Frame.Position.X) <= FURTHEST);
			CHECK(std::abs(placement.Frame.Position.Z) <= FURTHEST);
		}
	);
	CHECK(counted == 24);
}

TEST_CASE("the reflected camera is the eye mirrored through the plane", "[examples][scene]") {
	// **The whole of planar reflection**, and the one number in the scene that
	// is easy to get subtly wrong: the same distance behind the plane as the
	// eye is in front. An offset here reads as a reflection that slides across
	// the pane as the camera moves, which looks like a projection bug.
	const StagedAssets assets;

	Store store("mirrors");
	Scheduler systems;

	std::string error;
	REQUIRE(LoadScene(store, systems, ExamplePath("Mirrors-1-world.luau"), error));

	// **Placed by the engine now, so the pass has to run first.** The script
	// used to compute this itself and the assertion held straight after
	// loading; moving the arithmetic into `scene` is what made the reflection
	// follow a moving viewer, and it means the camera is wherever it was left
	// until something aims it.
	REQUIRE(engine::scene::AimSurfaceCameras(store) == 4);

	const auto *viewer = store.Get<engine::scene::Transform>(InScene(store, "Viewer"));
	REQUIRE(viewer != nullptr);

	// **Both axes, because the enclosure is the first scene to reflect off
	// one that is not Z.** A single pane facing -Z could not tell an axis-generic
	// `NormalOf` from a hard-coded one, so the wall on +X is doing the work here:
	// it is the same assertion with the roles of the components swapped, and a
	// reflection that only ever mirrored Z would pass the first and fail this.
	SECTION("the north wall, mirroring Z") {
		const Entity pane_ = InScene(store, "MirrorNorth");
		const auto *pane = store.Get<engine::scene::Transform>(pane_);
		const auto *bounds = store.Get<engine::scene::Bounds>(pane_);
		const auto *reflected =
			store.Get<engine::scene::Transform>(store.FindFirstChild(pane_, "Reflection"));
		REQUIRE(pane != nullptr);
		REQUIRE(bounds != nullptr);
		REQUIRE(reflected != nullptr);

		// **The plane is the face, not the middle of the slab**, and the face
		// here is `Back` — so the half thickness is *added*, putting the plane on
		// the inward side where the baseplate ends. Getting the sign wrong
		// mirrors through a plane four tenths of a unit outside the room, which
		// reads as a reflection that slides across the glass rather than as an
		// arithmetic error.
		const float plane = pane->Frame.Position.Z + bounds->HalfExtent.Z;

		CHECK(reflected->Frame.Position.Z == Approx(2.0f * plane - viewer->Frame.Position.Z));

		// And the two axes the plane does not mirror are unchanged.
		CHECK(reflected->Frame.Position.X == Approx(viewer->Frame.Position.X));
		CHECK(reflected->Frame.Position.Y == Approx(viewer->Frame.Position.Y));
	}

	SECTION("the east wall, mirroring X") {
		const Entity pane_ = InScene(store, "MirrorEast");
		const auto *pane = store.Get<engine::scene::Transform>(pane_);
		const auto *bounds = store.Get<engine::scene::Bounds>(pane_);
		const auto *reflected =
			store.Get<engine::scene::Transform>(store.FindFirstChild(pane_, "Reflection"));
		REQUIRE(pane != nullptr);
		REQUIRE(bounds != nullptr);
		REQUIRE(reflected != nullptr);

		// `Left` is -X, so the plane is the pane's centre pulled *back* towards
		// the room by its half thickness — the mirror image of the north wall's
		// sign, for the mirror image of the reason.
		const float plane = pane->Frame.Position.X - bounds->HalfExtent.X;

		CHECK(reflected->Frame.Position.X == Approx(2.0f * plane - viewer->Frame.Position.X));
		CHECK(reflected->Frame.Position.Y == Approx(viewer->Frame.Position.Y));
		CHECK(reflected->Frame.Position.Z == Approx(viewer->Frame.Position.Z));
	}
}

TEST_CASE("the interface scene builds and connects its buttons", "[examples][scene][gui]") {
	// **The scene that proves the 2D branch, and now the join under it.**
	// `Interface.luau` builds a `ScreenGui` entirely from a script — every
	// element exists because one specific thing looks correct without it — and
	// since v0.8's last step it also *connects*: `.Activated` on two named
	// buttons and on six swatches made in a loop.
	//
	// **What this case is for is the loop.** A join that only worked for
	// instances a script held a local for would pass every hand-written case and
	// fail on generated elements, which is most of a real interface. The
	// swatches are found by name through the tree, exactly as a script does.
	//
	// The dispatch itself — that a delivered event becomes a call, at the
	// barrier, in the router's order — is `engine.script.scripting`'s, where a
	// runtime can be driven directly. What cannot be checked there is that this
	// file is valid against the real class tree, which is this case's whole job:
	// a `.Activated` on a class that does not declare one, or a handler
	// referring to a local declared later, fails here and nowhere else.
	const StagedAssets assets;

	Store store("interface");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Interface.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// The tree the script describes, spot-checked at the two ends that matter:
	// the named buttons the handlers are attached to by local, and the
	// generated ones they are attached to by lookup.
	CHECK(CountElements(store, "Confirm") == 1);
	CHECK(CountElements(store, "Cancel") == 1);
	for (int index = 1; index <= 6; index++) {
		CHECK(CountElements(store, "Swatch" + std::to_string(index)) == 1);
	}

	// `refresh()` runs at the top level, so the hint carries the click count
	// rather than the authored placeholder. That is the cheapest proof the
	// handlers' shared state was reachable at all — a `refresh` that raised
	// would have failed the load above, and one that never ran would leave the
	// original text.
	const Entity hint = FirstElement(store, "Hint");
	REQUIRE(hint != engine::ecs::NULL_ENTITY);
	const engine::gui::Label *label = store.Get<engine::gui::Label>(hint);
	REQUIRE(label != nullptr);
	CHECK(std::string(label->Text.Text()).find("0 clicks") != std::string::npos);

	// **And it actually lays out**, which is a stronger claim than that the tree
	// exists and is the one containment can break.
	//
	// A `ScreenGui` draws only from `StarterGui` or a player's `PlayerGui`. This
	// file left its own unparented for a version and drew anyway, because the
	// engine was more permissive than the thing it models — so an author could
	// have shipped an interface that appeared in the studio and was missing in
	// the client. Asserting the tree alone would not have noticed either state.
	engine::gui::Screen display;
	display.Width = 1920.0f;
	display.Height = 1080.0f;

	const size_t rendered = engine::gui::Layout(store, display);
	CHECK(rendered > 0);

	const engine::gui::Resolved *placed = store.Get<engine::gui::Resolved>(hint);
	REQUIRE(placed != nullptr);
	CHECK(placed->Rendered);
	CHECK(placed->AbsoluteSize.X > 0.0f);

	// **The close button disables the collector rather than hiding the card**,
	// which is what makes closing cost nothing afterwards: `Layout` skips a
	// disabled `ScreenGui` before it asks how big anything under it is. Hiding
	// the card instead would leave the whole subtree measured and compiled every
	// frame for something nobody can see.
	const Entity close = FirstElement(store, "Close");
	REQUIRE(close != engine::ecs::NULL_ENTITY);

	// **Found by `Layer`, not by `Element`.** A `ScreenGui` is a
	// `LayerCollector` rather than a `GuiObject` — it has a canvas and no
	// rectangle of its own — which is the split `GuiBase2d` exists to hold, and
	// `FirstElement` would never see one.
	Entity screen = engine::ecs::NULL_ENTITY;
	store.Each<const engine::gui::Layer>([&](Entity entity, const engine::gui::Layer &) {
		if (screen == engine::ecs::NULL_ENTITY &&
			store.InstanceNameOf(entity) == engine::core::Name("InterfaceExample")) {
			screen = entity;
		}
	});
	REQUIRE(screen != engine::ecs::NULL_ENTITY);

	engine::gui::Layer *layer = store.GetMutable<engine::gui::Layer>(screen);
	REQUIRE(layer != nullptr);
	CHECK(layer->Enabled);

	layer->Enabled = false;
	CHECK(engine::gui::Layout(store, display) == 0);
	CHECK_FALSE(store.Get<engine::gui::Resolved>(hint)->Rendered);
}

TEST_CASE("the four-world mirrors scene varies by world", "[examples][scene][worlds]") {
	// **What this scene is for is the arrangement, not the room.** `--worlds N`
	// runs one script in N worlds and composites their views side by side; four
	// identical rooms in a row would look correct whatever order they were
	// placed in, so the file distinguishes itself by `game.JobId` and this case
	// is what says it actually does.
	//
	// Two worlds, built from the *same file*, asserted to differ. A script that
	// ignored its identity would give both the same caster count and pass every
	// check that only looked at one of them.
	const StagedAssets assets;

	const auto casterCount = [&](const char *worldName) {
		Store store(worldName);
		Scheduler systems;

		std::string error;
		const bool loaded = LoadScene(store, systems, ExamplePath("Mirrors-4-worlds.luau"), error);
		INFO(error);
		REQUIRE(loaded);

		// The parts every world has, whichever index it is.
		CHECK(CountNamed(store, "Baseplate") == 1);
		CHECK(CountNamed(store, "Mirror") == 1);

		// A surface camera on the pane, which is what makes it a mirror rather
		// than a coloured wall — the same pairing `Mirrors-1-world` asserts.
		size_t surfaces = 0;
		store.Each<const SurfaceCamera>([&](Entity, const SurfaceCamera &) { surfaces++; });
		CHECK(surfaces == 1);

		return CountNamed(store, "Caster");
	};

	// The palette table runs six, nine, twelve, fifteen. World 0 has no suffix
	// at all — `Client::BuildDemoWorlds` keeps the original name — so this also
	// covers the one index whose name a naive parse would get wrong.
	CHECK(casterCount("client.world") == 6);
	CHECK(casterCount("client.world.1") == 9);
	CHECK(casterCount("client.world.2") == 12);
	CHECK(casterCount("client.world.3") == 15);

	// **Wraps rather than clamps**, so a fifth world is a distinguishable room
	// rather than a fourth copy. `--worlds 6` is a thing somebody will type.
	CHECK(casterCount("client.world.4") == 6);
}

TEST_CASE("the four-world scene builds the same way in TypeScript", "[examples][scene][worlds][js]") {
	// **The roadmap's gate: each item lands in Luau *and* JavaScript, or it is
	// not done** — and this case could not exist until the staging step did.
	//
	// `Runtime.hpp` has always said a `.ts` file "is expected to have been
	// type-stripped already — nothing in the C++ build compiles TypeScript, and
	// nothing should: the engine loads what a toolchain emitted". Nothing
	// emitted it, so every `.ts` example was copied verbatim and QuickJS refused
	// it at the first type annotation. `Mirrors-1-world.ts` had been in that
	// state since the day it was written: typechecked, shipped, and unable to
	// run.
	//
	// `mono.engine/examples/CMakeLists.txt` transpiles them now, which is why
	// this loads `.js` — that is the file the toolchain actually produced, and
	// naming it `.ts` would have been a lie about its contents.
	//
	// **The strongest form of the parity check**, because the two files compute
	// the placement hash with different primitives — `bit32` against
	// `Math.imul` — so a mismatch there shows as a different scene rather than
	// as an error.
	const StagedAssets assets;

	const std::filesystem::path transpiled = ExamplePath("Mirrors-4-worlds.js");
	if (!std::filesystem::exists(transpiled)) {
		// A checkout without `bun install` has no `tsc`, so the configure said
		// so and staged no JavaScript twins. Recorded as skipped rather than
		// passed: the Luau case above still proves the scene builds.
		SUCCEED("no transpiled twin; tsc was not available at configure time");
		return;
	}

	Store store("client.world.2");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, transpiled.string(), error);
	INFO(error);
	REQUIRE(loaded);

	CHECK(CountNamed(store, "Baseplate") == 1);
	CHECK(CountNamed(store, "Mirror") == 1);
	CHECK(CountNamed(store, "Caster") == 12);
}

TEST_CASE("the gui containment names match the services scene installs", "[examples][scene][gui]") {
	// **A duplicated constant is only safe while something fails when the two
	// copies disagree, and this is that something.**
	//
	// `gui::Layout` decides whether a `ScreenGui` draws by walking its ancestors
	// and comparing *names* against `Workspace`, `StarterGui` and a player's
	// `PlayerGui`. It compares names rather than class ids because
	// `gui/AGENTS.md` refuses an edge to `scene` — the same refusal that made
	// `gui::Face` re-declare `NormalId`'s six members, pinned the same way by
	// `gui/tests/Enums.cpp`.
	//
	// **The check lives here because this is where both ends are linked.**
	// `scene` may not link `gui` and `gui` may not link `scene`, so neither
	// module's own tests can compare the two; `examples` links both, which makes
	// it the lowest place the comparison can be made at all.
	//
	// Renaming a service without renaming its copy would not break a build. It
	// would produce an engine in which every interface silently stops drawing —
	// a bug found by looking at a black screen rather than by running anything.
	Store store("examples_test.gui_names");
	engine::scene::InstallServices(store);

	CHECK(store.FindFirstRoot(engine::gui::WORKSPACE) != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstRoot(engine::gui::STARTER_GUI) != engine::ecs::NULL_ENTITY);

	// **`PlayerGui` is not a root**, so it is pinned differently: it is a child
	// of a `Player`, created by `scene::AddPlayer`, and what the two ends have
	// to agree on is the spelling.
	CHECK(engine::gui::PLAYER_GUI == engine::scene::PLAYER_GUI_NAME);

	// And a player actually gets one. A client whose player had no `PlayerGui`
	// could never be shown an interface, and the symptom would be a black
	// overlay rather than an error — which is why this is asserted here rather
	// than left to whoever adds a player.
	const Entity player = engine::scene::AddPlayer(store, "Someone", true);
	REQUIRE(player != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(player, engine::gui::PLAYER_GUI) != engine::ecs::NULL_ENTITY);
}

TEST_CASE("the studio's TypeScript property grid builds its tree", "[examples][scene][gui][panel]") {
	// **The point of the whole 2D branch, and deliberately the last step.**
	// `mono.studio` keeps Dear ImGui until the engine's own tree can draw a
	// property grid — because an editor half on each is two widget sets — so
	// this is not a replacement for the imgui panels. It is the proof that the
	// tree can carry one, which is what has to be true before any of them move.
	//
	// **Loaded as `.js`, because that is what the toolchain produced.**
	// `mono.studio/panels/*.ts` is transpiled at staging, the same way the
	// examples are and for the reason `Runtime.hpp` has always given: the engine
	// loads what a toolchain emitted and compiles no TypeScript itself.
	//
	// What it exercises, and none of it had a caller before this version:
	// `UIListLayout` stacking rows, `UIPadding` insetting them, a
	// `ScrollingFrame` whose canvas is longer than its panel, `.Activated` on a
	// generated row — the `gui`-to-`script` join — and `StarterGui` containment,
	// without which the whole thing draws nothing.
	const StagedAssets assets;

	const std::filesystem::path panel =
		engine::core::Paths::Assets() / "panels" / "Properties.js";
	if (!std::filesystem::exists(panel)) {
		SUCCEED("no transpiled panel; tsc was not available at configure time");
		return;
	}

	Store store("studio.panel");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, panel.string(), error);
	INFO(error);
	REQUIRE(loaded);

	// Ten rows, each a `TextButton` with a key and a value label under it. The
	// count is asserted so that a panel which silently built nothing — the
	// failure a containment or layout regression produces — cannot pass.
	CHECK(CountElements(store, "Position") == 1);
	CHECK(CountElements(store, "Transparency") == 1);
	CHECK(CountElements(store, "Key") == 10);
	CHECK(CountElements(store, "Value") == 10);

	// **And it lays out**, which is the claim the tree existing does not make.
	engine::gui::Screen display;
	display.Width = 1920.0f;
	display.Height = 1080.0f;
	CHECK(engine::gui::Layout(store, display) > 0);

	const Entity rows = FirstElement(store, "Rows");
	REQUIRE(rows != engine::ecs::NULL_ENTITY);

	const engine::gui::Resolved *placed = store.Get<engine::gui::Resolved>(rows);
	REQUIRE(placed != nullptr);
	CHECK(placed->Rendered);

	// The panel is anchored to the right edge, so its rows sit in the right half
	// of a 1920-wide screen. A layout that ignored `AnchorPoint` would put them
	// at the far right *edge* rather than inset from it, and one that ignored
	// `Position` would put them at zero — this separates all three.
	CHECK(placed->AbsolutePosition.X > 960.0f);
	CHECK(placed->AbsolutePosition.X < 1920.0f - 300.0f);
}
