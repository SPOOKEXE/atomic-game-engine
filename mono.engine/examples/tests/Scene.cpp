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
