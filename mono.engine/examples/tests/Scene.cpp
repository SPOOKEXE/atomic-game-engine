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

	// A pane, and the surface camera **parented to it** rather than standing
	// beside it in the workspace. That is the arrangement `SurfaceCamera` is
	// for: the camera is placed off a face of its parent, so it is a child of
	// the thing it reflects.
	const Entity mirror = InScene(store, "Mirror");
	REQUIRE(mirror != engine::ecs::NULL_ENTITY);

	const Entity reflection = store.FindFirstChild(mirror, "Reflection");
	REQUIRE(reflection != engine::ecs::NULL_ENTITY);

	const auto *surface = store.Get<SurfaceCamera>(reflection);
	REQUIRE(surface != nullptr);
	CHECK(surface->Width == 1024);
	CHECK(surface->Height == 1024);
	CHECK(surface->Face == engine::scene::NormalId::Front);

	// **The pane is told what it shows by the aiming pass, not by the script.**
	// Before it runs there is nothing to sample, which is why this asserts
	// either side of the call rather than only after: a scene that arrived with
	// `Surface` already set would pass the second check while proving nothing
	// about the mechanism.
	CHECK(store.Get<Visual>(mirror)->Surface == -1);
	REQUIRE(engine::scene::AimSurfaceCameras(store) == 1);
	CHECK(store.Get<Visual>(mirror)->Surface == 0);

	// **Two cameras, and the live one is not the surface one.** That is the
	// distinction `ActiveCamera` exists for, and getting it wrong would render
	// the scene from inside the mirror.
	const auto *active = store.Resource<ActiveCamera>();
	REQUIRE(active != nullptr);
	CHECK(active->Entity == InScene(store, "Viewer"));
	CHECK(active->Entity != reflection);

	// A floor, because a shadow needs something to fall on — a scene of
	// floating cubes has nothing, and the shadow pass would look broken.
	CHECK(InScene(store, "Floor") != engine::ecs::NULL_ENTITY);
	CHECK(CountNamed(store, "Caster") == 24);
	CHECK(CountNamed(store, "Frame") == 4);
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
	REQUIRE(engine::scene::AimSurfaceCameras(store) == 1);

	const Entity pane_ = InScene(store, "Mirror");
	const auto *viewer = store.Get<engine::scene::Transform>(InScene(store, "Viewer"));
	const auto *reflected = store.Get<engine::scene::Transform>(store.FindFirstChild(pane_, "Reflection"));
	const auto *pane = store.Get<engine::scene::Transform>(pane_);
	REQUIRE(viewer != nullptr);
	REQUIRE(reflected != nullptr);
	REQUIRE(pane != nullptr);

	// **The plane is the face, not the middle of the slab** — and that moved
	// this number by the pane's half thickness when the arithmetic left the
	// script. The hand-written version mirrored through the part's *centre*,
	// which is the centre of a 0.4-thick box rather than the surface anybody
	// sees; the engine reflects through the face the camera names, so the image
	// lines up with the glass instead of with a plane a fifth of a unit inside
	// it. Small, and exactly the sort of small that reads as a projection bug.
	const auto *bounds = store.Get<engine::scene::Bounds>(pane_);
	REQUIRE(bounds != nullptr);
	const float plane = pane->Frame.Position.Z - bounds->HalfExtent.Z;

	CHECK(reflected->Frame.Position.Z == Approx(2.0f * plane - viewer->Frame.Position.Z));

	// And the two axes the plane does not mirror are unchanged.
	CHECK(reflected->Frame.Position.X == Approx(viewer->Frame.Position.X));
	CHECK(reflected->Frame.Position.Y == Approx(viewer->Frame.Position.Y));
}
