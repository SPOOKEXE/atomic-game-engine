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
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

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

	// A surface camera, with the size the scene asked for. Without one the
	// surface pass does not run and the mirror draws as its own tint.
	const Entity reflection = store.FindFirstRoot("Reflection");
	REQUIRE(reflection != engine::ecs::NULL_ENTITY);

	const auto *surface = store.Get<SurfaceCamera>(reflection);
	REQUIRE(surface != nullptr);
	CHECK(surface->Width == 1024);
	CHECK(surface->Height == 1024);

	// A pane that shows it.
	const Entity mirror = store.FindFirstRoot("Mirror");
	REQUIRE(mirror != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Visual>(mirror)->Surface == 0);

	// **Two cameras, and the live one is not the surface one.** That is the
	// distinction `ActiveCamera` exists for, and getting it wrong would render
	// the scene from inside the mirror.
	const auto *active = store.Resource<ActiveCamera>();
	REQUIRE(active != nullptr);
	CHECK(active->Entity == store.FindFirstRoot("Viewer"));
	CHECK(active->Entity != reflection);

	// A floor, because a shadow needs something to fall on — a scene of
	// floating cubes has nothing, and the shadow pass would look broken.
	CHECK(store.FindFirstRoot("Floor") != engine::ecs::NULL_ENTITY);
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

	const auto *viewer = store.Get<engine::scene::Transform>(store.FindFirstRoot("Viewer"));
	const auto *reflected = store.Get<engine::scene::Transform>(store.FindFirstRoot("Reflection"));
	const auto *pane = store.Get<engine::scene::Transform>(store.FindFirstRoot("Mirror"));
	REQUIRE(viewer != nullptr);
	REQUIRE(reflected != nullptr);
	REQUIRE(pane != nullptr);

	const float plane = pane->Frame.Position.Z;
	CHECK(reflected->Frame.Position.Z == Approx(2.0f * plane - viewer->Frame.Position.Z));

	// And the two axes the plane does not mirror are unchanged.
	CHECK(reflected->Frame.Position.X == Approx(viewer->Frame.Position.X));
	CHECK(reflected->Frame.Position.Y == Approx(viewer->Frame.Position.Y));
}
