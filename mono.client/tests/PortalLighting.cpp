// Portal-light authoring and seam-field eligibility, measured on shipped scenes.
//
// `CollectLights` supplies only authored local lights. The renderer captures
// portal transport into bounded seam fields, avoiding a wide lamp becoming a
// point light at every same-world mouth. The GPU portal-radiance fixture checks
// the transported pixels; this suite keeps the authored input and portal-view
// eligibility tied to those same scenes.

#include "ScenarioChecks.hpp"

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>
#include <vector>

TEST_SUITE_ID("client.scene.portallighting")
TEST_DEPENDS("engine.testing.scenario")
TEST_DEPENDS("engine.scene.surfacecameras")
TEST_DEPENDS("engine.examples.scene")

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

	struct LightingSample {
		std::vector<SceneLight> Lights;
		size_t PortalViews = 0;
	};

	engine::testing::ScenarioReport LightingScenario(const char *sceneName) {
		using namespace engine::testing;
		using client::tests::Near;
		const bool mixed = std::string_view(sceneName) == "PortalLightMix.luau";
		const bool tunnels = std::string_view(sceneName) == "Tunnels.luau";
		std::vector<ScenarioCheck<LightingSample>> checks;
		checks.push_back(
			{"authored",
			 "client.scene.portallighting",
			 {{{"baseline"}}},
			 [mixed, tunnels](const auto &observation, CheckResult &result) {
				 const auto &lights = observation.Payload.Lights;
				 if (tunnels) {
					 // Portal transport uses seam fields rather than cloning lamps
					 // into the bounded point-light set.
					 result.Record("ten authored tunnel lights", lights.size() == 10);
					 result.Record("below device cap", lights.size() < engine::render::MAX_SCENE_LIGHTS);
					 return;
				 }
				 result.Record("authored light count", lights.size() == (mixed ? 2u : 1u));
				 if (lights.size() != (mixed ? 2u : 1u)) return;
				 const auto &first = lights[0];
				 if (mixed) {
					 const auto &green = lights[1];
					 result.Record("red X", Near(first.Position.X, 0.0, 0.05f));
					 result.Record("red Z", Near(first.Position.Z, -4.0, 0.05f));
					 result.Record("red colour", first.Colour.R > first.Colour.G * 2.0f);
					 result.Record("green X", green.Position.X > 100.0f);
					 result.Record("green colour", green.Colour.G > green.Colour.R * 2.0f);
				 } else {
					 result.Record("lamp X", Near(first.Position.X, 0.0, 0.01f));
					 result.Record("lamp Y", Near(first.Position.Y, 5.0, 0.01f));
					 result.Record("lamp Z", Near(first.Position.Z, 0.0, 0.01f));
					 result.Record("lamp range", Near(first.Range, 25.0));
				 }
			 },
			 {}}
		);
		if (mixed) {
			checks.push_back(
				{"disable",
				 "client.scene.portallighting",
				 {{{"baseline"}, {"disabled"}}},
				 [](const auto &observation, CheckResult &result) {
					 result.Record(
						 observation.Hook + " capture views",
						 observation.Payload.PortalViews == (observation.Hook == "baseline" ? 2u : 0u)
					 );
				 },
				 {}}
			);
		}
		ScenarioKey key;
		key.Scene = sceneName;
		// Staged assets are immutable during this test process. Completed reports
		// never survive the process or substitute for the runner's source invalidation.
		key.ContentRevision = "staged-examples";
		key.Configuration = "entities=64;jobs=2;physics=default";
		key.TickSchedule = "1@float(1/60)";
		key.FrameSchedule = "default";
		key.Actions = mixed ? "baseline;disable-all-mouths;disabled" : "baseline";
		key.CaptureConfiguration = "CollectLights(origin);CollectPortalViews;owned facts;no GPU";
		key.ObservationSchema = "client.portal-lighting.facts.v1";
		key.CheckSet = mixed ? "authored,disable.v1" : "authored.v1";
		ScenarioDriver<LightingSample> driver;
		driver.Run = [sceneName, mixed](ScenarioSession<LightingSample> &scenario) {
			LitScene scene(sceneName);
			const auto publish = [&](std::string hook) {
				LightingSample sample;
				sample.Lights = scene.Collect();
				std::vector<engine::render::PortalView> views;
				sample.PortalViews = client::CollectPortalViews(scene.World, views);
				const size_t bytes = sizeof(LightingSample) + sample.Lights.capacity() * sizeof(SceneLight);
				scenario.Publish({std::move(hook), scene.World.Time().Tick, std::move(sample), bytes});
			};
			publish("baseline");
			if (mixed) {
				for (const Entity mouth : scene.Portals()) {
					scene.World.GetMutable<engine::scene::Portal>(mouth)->Enabled = false;
				}
				publish("disabled");
			}
		};
		static ScenarioCache<LightingSample> cache(3);
		return cache.Run(std::move(key), std::move(checks), driver);
	}
}

TEST_CASE("a lamp in a portal scene stays one authored local light", "[client][portal][lighting]") {
	client::tests::CheckScenario(LightingScenario("PortalLightOut.luau"), "authored");
}

TEST_CASE("portal scenes retain their authored local light set", "[client][portal][lighting]") {
	client::tests::CheckScenario(LightingScenario("PortalLightMix.luau"), "authored");
}

TEST_CASE("tunnels local lights stay below the device cap", "[client][portal][lighting]") {
	client::tests::CheckScenario(LightingScenario("Tunnels.luau"), "authored");
}

TEST_CASE("a disabled portal withdraws its light-field capture views", "[client][portal][lighting]") {
	client::tests::CheckScenario(LightingScenario("PortalLightMix.luau"), "disable");
}
