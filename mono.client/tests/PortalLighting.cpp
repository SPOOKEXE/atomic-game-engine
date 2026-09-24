// Portal-light authoring and seam-field eligibility, measured on shipped scenes.
//
// `CollectLights` supplies only authored local lights. The renderer captures
// portal transport into bounded seam fields, avoiding a wide lamp becoming a
// point light at every same-world mouth. The GPU portal-radiance fixture checks
// the transported pixels; this suite keeps the authored input and portal-view
// eligibility tied to those same scenes.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>
#include <vector>

TEST_SUITE_ID("client.scene.portallighting")
TEST_DEPENDS("engine.scene.surfacecameras")
TEST_DEPENDS("engine.examples.scene")

using Catch::Approx;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::render::SceneLight;

namespace {
	// A world built from one of the portal lighting scenes, ready to collect.
	struct LitScene {
		engine::ecs::Store World{"portal-lighting"};
		engine::ecs::Scheduler Systems;
		std::filesystem::path PreviousAssets = engine::core::Paths::Assets();

		explicit LitScene(const char *scene) {
			engine::parallel::Jobs::Start(2);

			// The staged assets root, not the test binary's own directory -
			// `SceneTick.cpp`'s reason.
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");

			const bool built =
				client::BuildScriptedWorld(World, Systems, engine::examples::ExamplePath(scene), 64);
			REQUIRE(built);

			// One tick so every load-time write has settled the way a client's
			// first frame would have it.
			Systems.Tick(World, 1.0f / 60.0f);
		}

		~LitScene() {
			engine::core::Paths::SetAssetsOverride(PreviousAssets);
			engine::parallel::Jobs::Stop();
		}

		std::vector<SceneLight> Collect() {
			std::vector<SceneLight> lights;
			engine::render::CollectLights(World, Vector3{}, lights);
			return lights;
		}

		// Every portal mouth in the scene, for the Enabled cases.
		std::vector<Entity> Portals() {
			std::vector<Entity> mouths;
			World.Each<const engine::scene::Portal>([&](Entity entity, const engine::scene::Portal &) {
				mouths.push_back(entity);
			});
			return mouths;
		}
	};

}

TEST_CASE("a lamp in a portal scene stays one authored local light", "[client][portal][lighting]") {
	LitScene scene("PortalLightOut.luau");

	const auto lights = scene.Collect();
	REQUIRE(lights.size() == 1);
	const SceneLight &authored = lights.front();
	CHECK(authored.Position.X == Approx(0.0f).margin(0.01f));
	CHECK(authored.Position.Y == Approx(5.0f).margin(0.01f));
	CHECK(authored.Position.Z == Approx(0.0f).margin(0.01f));
	CHECK(authored.Range == Approx(25.0f));
}

TEST_CASE("portal scenes retain their authored local light set", "[client][portal][lighting]") {
	LitScene scene("PortalLightMix.luau");

	const auto lights = scene.Collect();
	REQUIRE(lights.size() == 2);
	const SceneLight &red = lights[0];
	const SceneLight &green = lights[1];
	CHECK(red.Position.X == Approx(0.0f).margin(0.05f));
	CHECK(red.Position.Z == Approx(-4.0f).margin(0.05f));
	CHECK(red.Colour.R > red.Colour.G * 2.0f);
	CHECK(green.Position.X > 100.0f);
	CHECK(green.Colour.G > green.Colour.R * 2.0f);
}

TEST_CASE("tunnels local lights stay below the device cap", "[client][portal][lighting]") {
	LitScene scene("Tunnels.luau");
	const auto lights = scene.Collect();

	// Two plain lamps, six fixed tunnel lamps and two lanterns make ten. Portal
	// transport is a seam field, so it cannot clone those wide plain lamps into
	// the 16-slot point-light set as the camera moves around the spawn.
	CHECK(lights.size() == 10);
	CHECK(lights.size() < engine::render::MAX_SCENE_LIGHTS);
}

TEST_CASE("a disabled portal withdraws its light-field capture views", "[client][portal][lighting]") {
	LitScene scene("PortalLightMix.luau");

	// The renderer's seam light-field capture renders one probe per
	// `PortalView` handed to it, so the view list is the capture path's whole
	// input - two mouths while the pair is enabled.
	std::vector<engine::render::PortalView> views;
	REQUIRE(client::CollectPortalViews(scene.World, views) == 2);

	// Turning the mouths off withdraws the seams, so the renderer is handed no
	// views, captures no light fields, and projects nothing - `Portal.Enabled`
	// is the switch the roadmap's capture-path item names.
	for (const Entity mouth : scene.Portals()) {
		scene.World.GetMutable<engine::scene::Portal>(mouth)->Enabled = false;
	}
	CHECK(client::CollectPortalViews(scene.World, views) == 0);
}
