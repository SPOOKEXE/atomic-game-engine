#include <engine/core/Bytes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/WeatherGround.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.game.weatherground")
TEST_DEPENDS("engine.weather.runtime")

using namespace engine;
using Catch::Approx;

namespace {
	void Register() {
		scene::RegisterSceneClasses();
		weather::RegisterWeatherRuntimeComponents();
		physics::RegisterPhysicsComponents();
	}
	ecs::Entity
	Ground(ecs::Store &store, const core::CFrame &frame, core::Vector3 extent, bool trigger = false) {
		const auto ground = store.Create();
		store.Set(ground, scene::Transform{frame});
		scene::Collider collider;
		collider.Extent = extent;
		collider.Trigger = trigger;
		store.Set(ground, collider);
		return ground;
	}
	ecs::Entity Storm(ecs::Store &store) {
		const auto provider = store.Create("Storm");
		scene::WeatherCompute controls;
		controls.BaseTemperatureCelsius = -20;
		controls.TemperatureVariationCelsius = 0;
		controls.IceFraction = 1;
		controls.StormIntensity = 1;
		controls.HailIntensity = 1;
		controls.LightningRate = 1;
		controls.WindMetresPerSecond.Y = 8;
		controls.LiftMetresPerSecond = 0;
		controls.TurbulenceMetresPerSecond = 0;
		store.Set(provider, controls);
		return provider;
	}
}

TEST_CASE("weather ground bindings query real elevated and rotated collider surfaces", "[game][weather]") {
	Register();
	ecs::Store store("weather.ground.geometry");
	physics::PreparePhysicsWorld(store, 4);
	Ground(store, core::CFrame({0, 125, 0}), {10, 1, 10});
	Ground(store, core::CFrame({0, 180, 0}), {10, 1, 10}, true);
	physics::SyncBroadphase(store);
	game::InstallWeatherGround(store);
	const auto *query = store.Resource<weather::WeatherGroundQuery>();
	REQUIRE(query);
	REQUIRE(query->Height);
	const auto elevated = query->Height(store, {0, 250, 0}, 300);
	REQUIRE(elevated);
	CHECK(*elevated == Approx(126));
	CHECK_FALSE(query->Height(store, {100, 250, 100}, 300));
	CHECK_FALSE(query->Height(store, {0, 250, 0}, 50));

	ecs::Store sloped("weather.ground.slope");
	physics::PreparePhysicsWorld(sloped, 4);
	const auto frame = core::CFrame({0, 10, 0}) * core::CFrame::Angles(0, 0, 0.5235987756f);
	Ground(sloped, frame, {10, 0.5f, 10});
	physics::SyncBroadphase(sloped);
	game::InstallWeatherGround(sloped);
	const auto *slopeQuery = sloped.Resource<weather::WeatherGroundQuery>();
	REQUIRE(slopeQuery);
	const auto middle = slopeQuery->Height(sloped, {0, 30, 0}, 50);
	const auto uphill = slopeQuery->Height(sloped, {2, 30, 0}, 50);
	REQUIRE(middle);
	REQUIRE(uphill);
	CHECK(*middle == Approx(10.5773503f).margin(0.001f));
	CHECK(*uphill == Approx(11.7320508f).margin(0.001f));
}

TEST_CASE(
	"headless weather impact scheduler binds lazily and reconstructs after save load", "[game][weather]"
) {
	Register();
	ecs::Store store("weather.ground.scheduler");
	physics::PreparePhysicsWorld(store);
	Ground(store, core::CFrame({0, 125, 0}), {10000, 1, 10000});
	const auto provider = Storm(store);
	ecs::Scheduler scheduler;
	physics::RegisterPhysicsSystems(scheduler);
	weather::RegisterWeatherClockSystem(scheduler);
	game::RegisterWeatherGroundSystem(scheduler);
	scheduler.Tick(store, 0.05f);
	CHECK_FALSE(store.HasResource<weather::WeatherGroundQuery>());
	for (uint32_t tick = 1; tick < 40; ++tick)
		scheduler.Tick(store, 0.05f);
	REQUIRE(store.Resource<weather::WeatherGroundQuery>());
	REQUIRE(store.Get<weather::WeatherEvents>(provider));
	REQUIRE_FALSE(store.Get<weather::WeatherEvents>(provider)->Events.empty());
	for (const auto &event : store.Get<weather::WeatherEvents>(provider)->Events)
		CHECK(event.EndMetres.Y == Approx(126));
	const auto sequence = store.Get<weather::WeatherRuntimeState>(provider)->NextSequence;
	core::ByteWriter saved;
	REQUIRE(store.Save(saved));
	ecs::Store restored("weather.ground.restored");
	core::ByteReader reader(saved.Bytes());
	REQUIRE(restored.Load(reader));
	REQUIRE(restored.Resource<weather::WeatherGroundQuery>());
	CHECK(restored.Resource<weather::WeatherGroundQuery>()->Height == nullptr);
	for (uint32_t tick = 0; tick < 40; ++tick)
		scheduler.Tick(restored, 0.05f);
	REQUIRE(restored.Resource<weather::WeatherGroundQuery>()->Height);
	CHECK(restored.Get<weather::WeatherRuntimeState>(provider)->NextSequence > sequence);
	for (const auto &event : restored.Get<weather::WeatherEvents>(provider)->Events)
		CHECK(event.EndMetres.Y == Approx(126));
	restored.SetAdoptOnly(true);
	restored.RemoveResource<weather::WeatherGroundQuery>();
	game::InstallWeatherGround(restored);
	CHECK_FALSE(restored.HasResource<weather::WeatherGroundQuery>());
}
