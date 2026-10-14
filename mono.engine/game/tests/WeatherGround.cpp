#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
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
#include <engine/weather/StatefulRuntime.hpp>

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

TEST_CASE("stateful weather copies collider terrain only at accepted physical revisions", "[game][weather]") {
	Register();
	ecs::Store store("weather.ground.stateful");
	physics::PreparePhysicsWorld(store);
	const auto ground = Ground(store, core::CFrame({0, 63, 0}), {512, 1, 512});
	const auto provider = store.Create("Weather");
	scene::WeatherCompute controls;
	controls.Mode = scene::WeatherMode::Stateful;
	controls.BaseHeightMetres = 0;
	controls.CellSizeMetres = 64;
	controls.ThicknessMetres = 256;
	controls.ExtentMetres = {256, 256, 256};
	controls.WindMetresPerSecond = {};
	controls.LiftMetresPerSecond = 0;
	controls.TurbulenceMetresPerSecond = 0;
	controls.ShearMetresPerSecond = 0;
	controls.PressureVariationHectopascals = 0;
	controls.TemperatureVariationCelsius = 0;
	controls.GroundVapourFluxKilogramsPerSquareMetreSecond = 0.0001f;
	store.Set(provider, controls);
	ecs::Scheduler scheduler;
	weather::RegisterWeatherClockSystem(scheduler);
	game::RegisterWeatherGroundSystem(scheduler);
	scheduler.Tick(store, 0);
	const auto *state = store.Get<weather::StatefulWeatherState>(provider);
	REQUIRE(state);
	REQUIRE(store.Resource<weather::WeatherGroundQuery>());
	CHECK(state->Simulation.GroundHeightMetres == std::vector<float>(16, 64));
	CHECK(weather::SimulationCellFlags(state->Simulation, 0) == 1);
	CHECK(weather::SimulationCellFlags(state->Simulation, 4) == 2);
	CHECK_FALSE(weather::QueryStatefulWeather(store, provider, {0, 32, 0}));
	CHECK(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
	const uint64_t epoch = state->Simulation.Definition.WorldEpoch;
	scheduler.Tick(store, 0.05f);
	state = store.Get<weather::StatefulWeatherState>(provider);
	REQUIRE(state);
	CHECK(state->LastStatus == weather::SimulationStatus::Success);
	CHECK(
		state->Simulation.Budget.SourceWaterKilograms ==
		Approx(16.0 * 64 * 64 * 0.0001 / controls.TickRate).epsilon(0.00001)
	);
	store.Set(ground, scene::Transform{core::CFrame({0, 127, 0})});
	scheduler.Tick(store, 0);
	state = store.Get<weather::StatefulWeatherState>(provider);
	REQUIRE(state);
	CHECK(state->Simulation.Definition.WorldEpoch == epoch);
	CHECK(state->Simulation.GroundHeightMetres == std::vector<float>(16, 64));
	++controls.TerrainRevision;
	++controls.DefinitionRevision;
	store.Set(provider, controls);
	scheduler.Tick(store, 0);
	state = store.Get<weather::StatefulWeatherState>(provider);
	REQUIRE(state);
	CHECK(state->Simulation.Definition.WorldEpoch > epoch);
	CHECK(state->Simulation.Tick == 1);
	CHECK(state->Simulation.GroundHeightMetres == std::vector<float>(16, 128));
	CHECK(weather::SimulationCellFlags(state->Simulation, 4) == 1);
	CHECK(weather::SimulationCellFlags(state->Simulation, 8) == 2);
	CHECK(state->Simulation.Budget.SourceWaterKilograms == 0);
	REQUIRE(store.Get<weather::StatefulWeatherCheckpoint>(provider));
	weather::SimulationState checkpoint;
	REQUIRE(
		weather::DecodeCheckpoint(store.Get<weather::StatefulWeatherCheckpoint>(provider)->Bytes, checkpoint)
	);
	CHECK(checkpoint == state->Simulation);
	core::ByteWriter bytes;
	REQUIRE(store.Save(bytes));
	ecs::Store loaded("weather.ground.stateful.loaded");
	core::ByteReader reader(bytes.Bytes());
	REQUIRE(loaded.Load(reader));
	CHECK(loaded.Get<weather::StatefulWeatherState>(provider)->Simulation == checkpoint);
	scheduler.Tick(store, 0.05f);
	scheduler.Tick(loaded, 0.05f);
	CHECK(
		loaded.Get<weather::StatefulWeatherState>(provider)->Simulation ==
		store.Get<weather::StatefulWeatherState>(provider)->Simulation
	);
}

TEST_CASE("detached stateful cloud receives no invented ground moisture source", "[game][weather]") {
	Register();
	ecs::Store store("weather.ground.detached");
	physics::PreparePhysicsWorld(store);
	Ground(store, core::CFrame({0, -1, 0}), {512, 1, 512});
	const auto provider = store.Create("Weather");
	scene::WeatherCompute controls;
	controls.Mode = scene::WeatherMode::Stateful;
	controls.BaseHeightMetres = 256;
	controls.CellSizeMetres = 64;
	controls.ThicknessMetres = 256;
	controls.ExtentMetres = {256, 256, 256};
	controls.GroundVapourFluxKilogramsPerSquareMetreSecond = 0.001f;
	store.Set(provider, controls);
	ecs::Scheduler scheduler;
	weather::RegisterWeatherClockSystem(scheduler);
	game::RegisterWeatherGroundSystem(scheduler);
	scheduler.Tick(store, 0.05f);
	const auto *state = store.Get<weather::StatefulWeatherState>(provider);
	REQUIRE(state);
	CHECK(state->Simulation.GroundHeightMetres == std::vector<float>(16, 0));
	CHECK(state->Simulation.Budget.SourceWaterKilograms == 0);
	for (size_t index = 0; index < state->Simulation.Cells.size(); ++index)
		CHECK(weather::SimulationCellFlags(state->Simulation, index) == 0);
}

TEST_CASE("quiet and refused terrain snapshots perform no repeated host queries", "[game][weather]") {
	Register();
	ecs::Store store("weather.ground.lazy");
	physics::PreparePhysicsWorld(store);
	Ground(store, core::CFrame({0, -1, 0}), {512, 1, 512});
	const auto provider = store.Create("Weather");
	scene::WeatherCompute controls;
	controls.Mode = scene::WeatherMode::Stateful;
	controls.BaseHeightMetres = 64000;
	controls.CellSizeMetres = 64;
	controls.ThicknessMetres = 256;
	controls.ExtentMetres = {256, 256, 256};
	controls.LightningRate = 1;
	controls.HailIntensity = 1;
	store.Set(provider, controls);
	ecs::Scheduler scheduler;
	weather::RegisterWeatherClockSystem(scheduler);
	game::RegisterWeatherGroundSystem(scheduler);
	scheduler.Tick(store, 0);
	REQUIRE(store.Has<weather::StatefulWeatherInitializationFailure>(provider));
	REQUIRE(core::Metrics::Get("weather.terrain.broadphase_syncs"));
	REQUIRE(core::Metrics::Get("weather.ground.height_queries"));
	const double failedSyncs = core::Metrics::Get("weather.terrain.broadphase_syncs")->Value;
	const double failedQueries = core::Metrics::Get("weather.ground.height_queries")->Value;
	for (uint32_t tick = 0; tick < 8; ++tick)
		scheduler.Tick(store, 0.05f);
	CHECK(core::Metrics::Get("weather.terrain.broadphase_syncs")->Value == failedSyncs);
	CHECK(core::Metrics::Get("weather.ground.height_queries")->Value == failedQueries);
	// Accepted semantic edits retry even when a raw author keeps the same revision number.
	controls.BaseHeightMetres = 0;
	controls.LightningRate = 0;
	controls.HailIntensity = 0;
	controls.Paused = true;
	store.Set(provider, controls);
	scheduler.Tick(store, 0);
	REQUIRE(store.Has<weather::StatefulWeatherState>(provider));
	CHECK_FALSE(store.Has<weather::StatefulWeatherInitializationFailure>(provider));
	scheduler.Tick(store, 0);
	const double warmSyncs = core::Metrics::Get("weather.terrain.broadphase_syncs")->Value;
	const double warmQueries = core::Metrics::Get("weather.ground.height_queries")->Value;
	for (uint32_t tick = 0; tick < 8; ++tick)
		scheduler.Tick(store, 0.05f);
	CHECK(core::Metrics::Get("weather.terrain.broadphase_syncs")->Value == warmSyncs);
	CHECK(core::Metrics::Get("weather.ground.height_queries")->Value == warmQueries);
}
