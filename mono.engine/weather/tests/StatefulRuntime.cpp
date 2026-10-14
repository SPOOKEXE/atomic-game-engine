#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/StatefulRuntime.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.weather.statefulruntime")
TEST_DEPENDS("engine.weather.clock")
TEST_DEPENDS("engine.weather.checkpoint")

using namespace engine;

namespace {
	scene::WeatherCompute Controls() {
		scene::WeatherCompute controls;
		controls.Mode = scene::WeatherMode::Stateful;
		controls.CellSizeMetres = 64;
		controls.BaseHeightMetres = 0;
		controls.ThicknessMetres = 256;
		controls.ExtentMetres = {256, 256, 256};
		controls.TurbulenceMetresPerSecond = 0;
		controls.ShearMetresPerSecond = 0;
		controls.LiftMetresPerSecond = 0;
		controls.PressureVariationHectopascals = 0;
		controls.TemperatureVariationCelsius = 0;
		controls.WindMetresPerSecond = {4, 0, 0};
		return controls;
	}
	void Register() {
		scene::RegisterSceneComponents();
		weather::RegisterWeatherRuntimeComponents();
		weather::RegisterStatefulWeatherComponents();
	}
	void Tick(ecs::Store &store, float seconds) {
		store.AdvanceTick(seconds);
		weather::UpdateWeatherClocks(store);
		weather::UpdateStatefulWeather(store);
		weather::UpdateWeatherRuntime(store);
	}
}

TEST_CASE("stateful fixed tick owns real physical cells and pause keeps history", "[weather][stateful]") {
	Register();
	ecs::Store store("stateful");
	const auto provider = store.Create("weather");
	store.Set(provider, Controls());
	Tick(store, 0.2f);
	const auto *state = store.Get<weather::StatefulWeatherState>(provider);
	REQUIRE(state);
	CHECK(state->Simulation.Tick == 4);
	CHECK(state->Simulation.Cells.size() == 64);
	const auto published = state->Simulation;
	weather::UpdateStatefulWeather(store);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation == published);
	auto controls = Controls();
	controls.Paused = true;
	store.Set(provider, controls);
	Tick(store, 1);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation == published);
	const auto sample = weather::QueryStatefulWeather(store, provider, {0, 64, 0});
	REQUIRE(sample);
	CHECK(sample->Air.VapourKilogramsPerKilogram >= 0);
}

TEST_CASE(
	"stateful checkpoints support current latejoin without replica CPU mirrors", "[weather][stateful]"
) {
	Register();
	ecs::Store authority("authority");
	const auto provider = authority.Create("weather");
	authority.Set(provider, Controls());
	Tick(authority, 0.2f);
	REQUIRE(weather::PublishStatefulCheckpoint(authority, provider));
	const auto checkpoint = *authority.Get<weather::StatefulWeatherCheckpoint>(provider);
	ecs::Store replica("replica");
	const auto replicaProvider = replica.Create("weather");
	replica.Set(replicaProvider, Controls());
	replica.Set(replicaProvider, checkpoint);
	replica.SetAdoptOnly(true);
	Tick(replica, 1);
	CHECK_FALSE(replica.Has<weather::StatefulWeatherState>(replicaProvider));
	CHECK(replica.Get<weather::StatefulWeatherCheckpoint>(replicaProvider)->Bytes == checkpoint.Bytes);
	CHECK_FALSE(weather::RestoreStatefulCheckpoint(replica, replicaProvider, checkpoint));
	core::ByteWriter saved;
	REQUIRE(authority.Save(saved));
	ecs::Store loaded("loaded");
	core::ByteReader reader(saved.Bytes());
	REQUIRE(loaded.Load(reader));
	CHECK(
		loaded.Get<weather::StatefulWeatherState>(provider)->Simulation ==
		authority.Get<weather::StatefulWeatherState>(provider)->Simulation
	);
	Tick(authority, 0.1f);
	Tick(loaded, 0.1f);
	CHECK(
		loaded.Get<weather::StatefulWeatherState>(provider)->Simulation ==
		authority.Get<weather::StatefulWeatherState>(provider)->Simulation
	);
}

TEST_CASE("canonical grid cap rejects oversized authored state before allocation", "[weather][stateful]") {
	Register();
	ecs::Store store("gridcap");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	controls.ExtentMetres = {16384, 4096, 16384};
	store.Set(provider, controls);
	Tick(store, 0.1f);
	CHECK_FALSE(store.Has<weather::StatefulWeatherState>(provider));
	CHECK_FALSE(store.Has<weather::StatefulWeatherCheckpoint>(provider));
	CHECK_FALSE(weather::MakeSimulationDefinition(controls));
}

TEST_CASE(
	"stateful event columns retain completed samples across fractional phase rates", "[weather][stateful]"
) {
	Register();
	ecs::Store store("columns");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	controls.TimeScale = 0.5f;
	store.Set(provider, controls);
	Tick(store, 1);
	const auto *state = store.Get<weather::StatefulWeatherState>(provider);
	REQUIRE(state);
	CHECK(state->Simulation.Tick == 10);
	const auto column = weather::StatefulEventColumn(*state, 0);
	CHECK(weather::QueryStatefulWeather(store, provider, column, 0, 0));
	CHECK_FALSE(weather::QueryStatefulWeather(store, provider, {column.X + 1, 0, column.Z}, 0, 0));
	CHECK_FALSE(weather::QueryStatefulWeather(store, provider, column, 0, 11));
}

TEST_CASE("retired stateful providers free canonical admission slots", "[weather][stateful]") {
	Register();
	ecs::Store store("retirement");
	std::vector<ecs::Entity> providers;
	for (uint32_t index = 0; index < 32; ++index) {
		const auto provider = store.Create("layer" + std::to_string(index));
		providers.push_back(provider);
		store.Set(provider, Controls());
	}
	Tick(store, 0);
	for (ecs::Entity provider : providers)
		REQUIRE(store.Has<weather::StatefulWeatherState>(provider));
	for (ecs::Entity provider : providers) {
		auto controls = Controls();
		controls.Mode = scene::WeatherMode::Procedural;
		store.Set(provider, controls);
	}
	const auto replacement = store.Create("replacement");
	store.Set(replacement, Controls());
	Tick(store, 0);
	for (ecs::Entity provider : providers) {
		CHECK_FALSE(store.Has<weather::StatefulWeatherState>(provider));
		CHECK_FALSE(store.Has<weather::StatefulWeatherCheckpoint>(provider));
	}
	CHECK(store.Has<weather::StatefulWeatherState>(replacement));
}

TEST_CASE("stateful queries and restore refuse incompatible authored domains", "[weather][stateful]") {
	Register();
	ecs::Store store("compatibility");
	const auto provider = store.Create("weather");
	store.Set(provider, Controls());
	Tick(store, 0.1f);
	const auto checkpoint = *store.Get<weather::StatefulWeatherCheckpoint>(provider);
	auto controls = Controls();
	controls.ExtentMetres.X = 512;
	store.Set(provider, controls);
	Tick(store, 0);
	CHECK(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
	CHECK_FALSE(weather::RestoreStatefulCheckpoint(store, provider, checkpoint));
	controls = Controls();
	controls.Seed++;
	store.Set(provider, controls);
	Tick(store, 0);
	CHECK(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
	CHECK_FALSE(weather::RestoreStatefulCheckpoint(store, provider, checkpoint));
	store.Set(provider, Controls());
	Tick(store, 0);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Epoch++;
	store.Set(provider, clock);
	CHECK_FALSE(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
	CHECK_FALSE(weather::RestoreStatefulCheckpoint(store, provider, checkpoint));
}

TEST_CASE("visual edits do not exhaust physical forcing history", "[weather][stateful]") {
	Register();
	ecs::Store store("edits");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	store.Set(provider, controls);
	for (uint32_t edit = 0; edit < 80; ++edit) {
		++controls.DefinitionRevision;
		controls.ShadowStrength = edit % 2 ? 0.5f : 0.6f;
		store.Set(provider, controls);
		Tick(store, 0.05f);
		const auto *state = store.Get<weather::StatefulWeatherState>(provider);
		REQUIRE(state);
		CHECK(state->LastStatus == weather::SimulationStatus::Success);
		CHECK(state->Simulation.Tick == edit + 1);
	}
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation.ForcingHistory.size() == 1);
}

TEST_CASE("replica consumer queries read checkpoint cells without a CPU grid mirror", "[weather][stateful]") {
	Register();
	ecs::Store authority("authority");
	const auto provider = authority.Create("weather");
	authority.Set(provider, Controls());
	Tick(authority, 0.1f);
	REQUIRE(weather::PublishStatefulCheckpoint(authority, provider));
	const auto checkpoint = *authority.Get<weather::StatefulWeatherCheckpoint>(provider);
	ecs::Store replica("replica");
	const auto local = replica.Create("weather");
	replica.Set(local, Controls());
	replica.Set(local, checkpoint);
	replica.SetAdoptOnly(true);
	const auto reference = weather::QueryStatefulWeather(authority, provider, {0, 64, 0});
	const auto sampled = weather::QueryStatefulWeather(replica, local, {0, 64, 0});
	REQUIRE(reference);
	REQUIRE(sampled);
	CHECK(sampled->Air.VapourKilogramsPerKilogram == reference->Air.VapourKilogramsPerKilogram);
	CHECK(sampled->Air.TemperatureCelsius == reference->Air.TemperatureCelsius);
	CHECK_FALSE(replica.Has<weather::StatefulWeatherState>(local));
	const uint64_t writes = replica.ComponentChangeVersion<weather::StatefulWeatherCheckpoint>();
	CHECK(weather::QueryStatefulWeather(replica, local, {16, 64, 0}));
	CHECK(replica.ComponentChangeVersion<weather::StatefulWeatherCheckpoint>() == writes);
	auto corrupted = *replica.Get<weather::StatefulWeatherCheckpoint>(local);
	REQUIRE(corrupted.Queries);
	corrupted.Bytes[corrupted.Queries->CellOffset] ^= std::byte{1};
	replica.Set(local, corrupted);
	CHECK_FALSE(weather::QueryStatefulWeather(replica, local, {0, 64, 0}));
	replica.Set(local, checkpoint);
	CHECK(weather::QueryStatefulWeather(replica, local, {0, 64, 0}));
}

TEST_CASE("replica terrain and precipitation queries use saved falling water", "[weather][stateful]") {
	Register();
	ecs::Store authority("terrain.authority");
	const auto provider = authority.Create("weather");
	auto controls = Controls();
	controls.WindMetresPerSecond = {};
	controls.FogStrength = 1;
	authority.Set(provider, controls);
	authority.SetResource(
		weather::WeatherGroundQuery{[](const ecs::Store &, core::Vector3, float) -> std::optional<float> {
			return 64;
		}}
	);
	Tick(authority, 0);
	auto *state = authority.GetMutable<weather::StatefulWeatherState>(provider);
	REQUIRE(state);
	for (size_t index = 0; index < state->Simulation.Cells.size(); ++index) {
		if (weather::SimulationCellFlags(state->Simulation, index) & 1) continue;
		state->Simulation.Cells[index].TemperatureCelsius = -20;
		state->Simulation.Cells[index].VapourKilogramsPerKilogram = 0.03f;
		state->Simulation.Cells[index].VelocityMetresPerSecond = {};
		state->Simulation.FallingCondensateKilogramsPerCubicMetre[index] = 0.0002f;
	}
	state->Simulation.Budget.InitialWaterKilograms = weather::SimulationWaterKilograms(state->Simulation);
	state->Simulation.Budget.InitialEnthalpyJoules = weather::SimulationEnthalpyJoules(state->Simulation);
	REQUIRE(weather::PublishStatefulCheckpoint(authority, provider));
	ecs::Store replica("terrain.replica");
	const auto local = replica.Create("weather");
	replica.Set(local, controls);
	replica.Set(local, *authority.Get<weather::StatefulWeatherCheckpoint>(provider));
	replica.SetAdoptOnly(true);
	CHECK_FALSE(weather::QueryStatefulWeather(authority, provider, {0, 32, 0}));
	CHECK_FALSE(weather::QueryStatefulWeather(replica, local, {0, 32, 0}));
	const auto reference = weather::QueryStatefulWeather(authority, provider, {0, 64, 0}, 0);
	const auto sampled = weather::QueryStatefulWeather(replica, local, {0, 64, 0}, 0);
	REQUIRE(reference);
	REQUIRE(sampled);
	CHECK(reference->Precipitation.SnowMillimetresPerHour == Catch::Approx(7.2f).epsilon(0.00001));
	CHECK(sampled->Precipitation.SnowMillimetresPerHour == reference->Precipitation.SnowMillimetresPerHour);
	CHECK(sampled->Precipitation.LiquidMillimetresPerHour == 0);
	CHECK(sampled->FogExtinctionPerMetre > 0);
	CHECK(sampled->FogExtinctionPerMetre == reference->FogExtinctionPerMetre);
	CHECK_FALSE(replica.Has<weather::StatefulWeatherState>(local));
	REQUIRE(replica.Get<weather::StatefulWeatherCheckpoint>(local)->Queries);
	CHECK(
		replica.Get<weather::StatefulWeatherCheckpoint>(local)->Queries->GroundHeightMetres ==
		std::vector<float>(16, 64)
	);
}

TEST_CASE(
	"canonical storm columns keep positive hash endpoints inside terrain bounds", "[weather][stateful]"
) {
	auto definition = weather::MakeSimulationDefinition(Controls());
	REQUIRE(definition);
	auto simulation =
		weather::CreateSimulation(*definition, weather::SolverCell{}, {}, std::vector<float>(16, 512));
	REQUIRE(simulation);
	weather::StatefulWeatherState state;
	state.Simulation = *simulation;
	state.Seed = 1;
	state.LayerKey = 3477690857u;
	state.Simulation.Definition.WorldEpoch = 1;
	const auto column = weather::StatefulEventColumn(state, 58783);
	CHECK(column.X >= -128);
	CHECK(column.X < 128);
	CHECK(column.Z >= -128);
	CHECK(column.Z < 128);
	CHECK(weather::StatefulEventColumnBlocked(state, column));
	CHECK_FALSE(weather::StatefulEventColumnBlocked(state, {128, column.Y, column.Z}));
}

TEST_CASE("wind changes begin at their accepted phase anchor across solver backlog", "[weather][stateful]") {
	Register();
	ecs::Store store("backlog");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	store.Set(provider, controls);
	Tick(store, 2);
	const auto *before = store.Get<weather::StatefulWeatherState>(provider);
	REQUIRE(before);
	REQUIRE(before->Simulation.Tick == 32);
	REQUIRE(weather::PublishStatefulCheckpoint(store, provider));
	weather::SimulationState continuous;
	REQUIRE(
		weather::DecodeCheckpoint(store.Get<weather::StatefulWeatherCheckpoint>(provider)->Bytes, continuous)
	);
	for (uint32_t tick = 32; tick < 40; ++tick)
		REQUIRE(weather::StepSimulation(continuous) == weather::SimulationStatus::Success);
	const auto beforeWind = continuous.Cells;
	auto forcing = continuous.ForcingHistory.back();
	forcing.Tick = 40;
	forcing.WindTargetMetresPerSecond = {12, 0, 0};
	REQUIRE(weather::AddSimulationForcing(continuous, forcing));
	++continuous.Definition.Revision;
	REQUIRE(weather::StepSimulation(continuous) == weather::SimulationStatus::Success);
	controls.WindMetresPerSecond = {12, 0, 0};
	++controls.DefinitionRevision;
	store.Set(provider, controls);
	Tick(store, 0);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation.Tick == 40);
	// With storm consumers disabled the base backlog is already consumed, so the edit is accepted now.
	// Its phase-40 forcing first affects the transition from 40 to 41, leaving catchup cells unchanged.
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation.Cells == beforeWind);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation.ForcingHistory.front().Tick == 0);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation.ForcingHistory.back().Tick == 40);
	Tick(store, 0.05f);
	const auto *accepted = store.Get<weather::StatefulWeatherState>(provider);
	REQUIRE(accepted);
	CHECK(accepted->Simulation.ForcingHistory.back().Tick == 40);
	CHECK(accepted->Simulation == continuous);
}

TEST_CASE("accepted provider renames preserve physical history and save identity", "[weather][stateful]") {
	Register();
	scene::RegisterSceneClasses();
	ecs::Store store("rename");
	const auto provider = store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Storm");
	store.Set(provider, Controls());
	Tick(store, 0.1f);
	const auto before = store.Get<weather::StatefulWeatherState>(provider)->Simulation;
	REQUIRE(store.SetInstanceName(provider, "Renamed"));
	Tick(store, 0);
	const auto &renamed = store.Get<weather::StatefulWeatherState>(provider)->Simulation;
	CHECK(renamed.Cells == before.Cells);
	CHECK(renamed.Tick == before.Tick);
	CHECK(renamed.Budget == before.Budget);
	CHECK(renamed.ForcingHistory == before.ForcingHistory);
	CHECK(renamed.Definition.LayerName != before.Definition.LayerName);
	CHECK(renamed.Definition.LayerName == weather::WeatherLayerName(store, provider));
	CHECK(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
	const auto checkpoint = *store.Get<weather::StatefulWeatherCheckpoint>(provider);
	weather::SimulationState decoded;
	REQUIRE(weather::DecodeCheckpoint(checkpoint.Bytes, decoded));
	CHECK(decoded == renamed);
	core::ByteWriter saved;
	REQUIRE(store.Save(saved));
	ecs::Store loaded("loaded");
	core::ByteReader reader(saved.Bytes());
	REQUIRE(loaded.Load(reader));
	CHECK(loaded.Get<weather::StatefulWeatherState>(provider)->Simulation == renamed);
	CHECK(weather::QueryStatefulWeather(loaded, provider, {0, 64, 0}));
}

TEST_CASE("structural stateful edits initialize a complete new current-tick epoch", "[weather][stateful]") {
	Register();
	ecs::Store store("structural");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	store.Set(provider, controls);
	Tick(store, 0.2f);
	for (uint32_t edit = 0; edit < 4; ++edit) {
		const auto oldClock = *store.Get<scene::WeatherClock>(provider);
		const auto oldTime = *weather::WeatherTimeOf(oldClock);
		if (edit == 0) controls.BaseTemperatureCelsius = 25;
		if (edit == 1) ++controls.Seed;
		if (edit == 2) controls.ExtentMetres.X = 512;
		if (edit == 3) controls.LayerName = core::Name("new-purpose");
		++controls.DefinitionRevision;
		store.Set(provider, controls);
		Tick(store, 0);
		const auto &state = *store.Get<weather::StatefulWeatherState>(provider);
		const auto &clock = *store.Get<scene::WeatherClock>(provider);
		CHECK(clock.Epoch == oldClock.Epoch + 1);
		CHECK(clock.Sequence > oldClock.Sequence);
		CHECK(clock.Tick == oldClock.Tick);
		CHECK(clock.ElapsedSeconds == oldClock.ElapsedSeconds);
		CHECK(clock.ElapsedFractionQ32 == oldClock.ElapsedFractionQ32);
		CHECK(weather::WeatherTimeOf(clock)->PhaseTick == oldTime.PhaseTick);
		CHECK(weather::WeatherTimeOf(clock)->DisplacementQ8 == oldTime.DisplacementQ8);
		CHECK(state.Simulation.Tick == oldTime.PhaseTick);
		CHECK(state.Simulation.Definition.WorldEpoch == clock.Epoch);
		CHECK(state.Simulation.Budget.SourceWaterKilograms == 0);
		CHECK(state.Simulation.Budget.PrecipitationWaterKilograms == 0);
		CHECK(state.Simulation.Budget.ExternalEnthalpyJoules == 0);
		CHECK(
			state.Simulation.Budget.InitialWaterKilograms ==
			weather::SimulationWaterKilograms(state.Simulation)
		);
		CHECK(state.LastStatus == weather::SimulationStatus::Success);
		CHECK(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
		weather::SimulationState decoded;
		REQUIRE(
			weather::DecodeCheckpoint(store.Get<weather::StatefulWeatherCheckpoint>(provider)->Bytes, decoded)
		);
		CHECK(decoded == state.Simulation);
		CHECK(store.Get<weather::WeatherRuntimeState>(provider)->Epoch == clock.Epoch);
		CHECK(store.Get<weather::WeatherRuntimeState>(provider)->ChargeQ16 == 0);
	}
	core::ByteWriter saved;
	REQUIRE(store.Save(saved));
	ecs::Store loaded("loaded");
	core::ByteReader reader(saved.Bytes());
	REQUIRE(loaded.Load(reader));
	Tick(store, 0.05f);
	Tick(loaded, 0.05f);
	CHECK(
		loaded.Get<weather::StatefulWeatherState>(provider)->Simulation ==
		store.Get<weather::StatefulWeatherState>(provider)->Simulation
	);
}

TEST_CASE("wind and presentation edits preserve the physical epoch and budgets", "[weather][stateful]") {
	Register();
	ecs::Store store("nonstructural");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	store.Set(provider, controls);
	Tick(store, 0.1f);
	const auto physical = store.Get<weather::StatefulWeatherState>(provider)->Simulation;
	const uint64_t epoch = store.Get<scene::WeatherClock>(provider)->Epoch;
	controls.WindMetresPerSecond.X = 12;
	store.Set(provider, controls);
	Tick(store, 0);
	CHECK(store.Get<scene::WeatherClock>(provider)->Epoch == epoch);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation.Cells == physical.Cells);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation.Budget == physical.Budget);
	controls.ShadowStrength = 0.2f;
	++controls.DefinitionRevision;
	store.Set(provider, controls);
	Tick(store, 0);
	CHECK(store.Get<scene::WeatherClock>(provider)->Epoch == epoch);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation.Cells == physical.Cells);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation.Budget == physical.Budget);
}

TEST_CASE("accepted vapour scale edits start a complete current-phase physics epoch", "[weather][stateful]") {
	Register();
	ecs::Store store("vapour-scale");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	store.Set(provider, controls);
	Tick(store, 0.2f);
	const auto previous = store.Get<weather::StatefulWeatherState>(provider)->Simulation;
	const auto clock = *store.Get<scene::WeatherClock>(provider);
	const auto phase = weather::WeatherTimeOf(clock)->PhaseTick;
	controls.VapourScale = 0.06f;
	++controls.DefinitionRevision;
	store.Set(provider, controls);
	Tick(store, 0);
	const auto &current = store.Get<weather::StatefulWeatherState>(provider)->Simulation;
	CHECK(current.Tick == phase);
	CHECK(current.Definition.WorldEpoch == clock.Epoch + 1);
	CHECK(current.Definition.InitializationRecipe != previous.Definition.InitializationRecipe);
	CHECK(current.Cells != previous.Cells);
	CHECK(current.Budget.InitialWaterKilograms == weather::SimulationWaterKilograms(current));
	CHECK(current.Budget.SourceWaterKilograms == 0);
	CHECK(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
	weather::SimulationState decoded;
	REQUIRE(
		weather::DecodeCheckpoint(store.Get<weather::StatefulWeatherCheckpoint>(provider)->Bytes, decoded)
	);
	CHECK(decoded == current);
}

TEST_CASE("invalid stateful domains retain old physics and valid edits recover", "[weather][stateful]") {
	Register();
	ecs::Store store("recover");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	controls.LightningRate = 1;
	controls.HailIntensity = 1;
	store.Set(provider, controls);
	Tick(store, 0.1f);
	const auto old = store.Get<weather::StatefulWeatherState>(provider)->Simulation;
	controls.ExtentMetres = {16384, 4096, 16384};
	++controls.DefinitionRevision;
	store.Set(provider, controls);
	Tick(store, 0.05f);
	CHECK(store.Get<weather::StatefulWeatherState>(provider)->Simulation == old);
	CHECK(
		store.Get<weather::StatefulWeatherState>(provider)->LastStatus == weather::SimulationStatus::Invalid
	);
	CHECK_FALSE(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
	controls = Controls();
	controls.BaseTemperatureCelsius = 25;
	controls.DefinitionRevision = 3;
	store.Set(provider, controls);
	Tick(store, 0);
	const auto &current = store.Get<weather::StatefulWeatherState>(provider)->Simulation;
	CHECK(current.Definition.WorldEpoch == old.Definition.WorldEpoch + 1);
	CHECK(current.Tick == weather::WeatherTimeOf(*store.Get<scene::WeatherClock>(provider))->PhaseTick);
	CHECK(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
}

TEST_CASE("switching back to stateful begins at the current accepted phase", "[weather][stateful]") {
	Register();
	ecs::Store store("mode");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	store.Set(provider, controls);
	Tick(store, 0.1f);
	controls.Mode = scene::WeatherMode::Procedural;
	store.Set(provider, controls);
	Tick(store, 0.1f);
	CHECK_FALSE(store.Has<weather::StatefulWeatherState>(provider));
	const auto previous = *store.Get<scene::WeatherClock>(provider);
	controls.Mode = scene::WeatherMode::Stateful;
	store.Set(provider, controls);
	Tick(store, 0);
	const auto &state = store.Get<weather::StatefulWeatherState>(provider)->Simulation;
	CHECK(state.Tick == weather::WeatherTimeOf(previous)->PhaseTick);
	CHECK(state.Definition.WorldEpoch == previous.Epoch + 1);
	CHECK(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
}

TEST_CASE("refused initial physics survives save and recovers after a valid edit", "[weather][stateful]") {
	Register();
	ecs::Store store("initial-refusal");
	const auto provider = store.Create("weather");
	auto controls = Controls();
	controls.BaseHeightMetres = 64000;
	controls.LightningRate = 1;
	controls.HailIntensity = 1;
	REQUIRE(weather::MakeSimulationDefinition(controls));
	store.Set(provider, controls);
	Tick(store, 0.1f);
	REQUIRE(store.Has<weather::StatefulWeatherInitializationFailure>(provider));
	CHECK_FALSE(store.Has<weather::StatefulWeatherState>(provider));
	CHECK_FALSE(store.Has<weather::StatefulWeatherCheckpoint>(provider));
	REQUIRE(store.Get<weather::WeatherRuntimeState>(provider));
	CHECK(
		store.Get<weather::WeatherRuntimeState>(provider)->LastTick <
		store.Get<scene::WeatherClock>(provider)->Tick
	);
	core::ByteWriter saved;
	REQUIRE(store.Save(saved));
	ecs::Store loaded("loaded");
	core::ByteReader reader(saved.Bytes());
	REQUIRE(loaded.Load(reader));
	CHECK(loaded.Has<weather::StatefulWeatherInitializationFailure>(provider));
	CHECK_FALSE(loaded.Has<weather::StatefulWeatherState>(provider));
	controls.BaseHeightMetres = 0;
	++controls.DefinitionRevision;
	store.Set(provider, controls);
	loaded.Set(provider, controls);
	Tick(store, 0);
	Tick(loaded, 0);
	REQUIRE(store.Has<weather::StatefulWeatherState>(provider));
	REQUIRE(loaded.Has<weather::StatefulWeatherState>(provider));
	CHECK_FALSE(store.Has<weather::StatefulWeatherInitializationFailure>(provider));
	CHECK_FALSE(loaded.Has<weather::StatefulWeatherInitializationFailure>(provider));
	CHECK(
		store.Get<weather::StatefulWeatherState>(provider)->Simulation ==
		loaded.Get<weather::StatefulWeatherState>(provider)->Simulation
	);
	CHECK(
		store.Get<weather::StatefulWeatherState>(provider)->Simulation.Tick ==
		weather::WeatherTimeOf(*store.Get<scene::WeatherClock>(provider))->PhaseTick
	);
	CHECK(weather::QueryStatefulWeather(store, provider, {0, 64, 0}));
	Tick(store, 0.05f);
	Tick(loaded, 0.05f);
	CHECK(
		store.Get<weather::StatefulWeatherState>(provider)->Simulation ==
		loaded.Get<weather::StatefulWeatherState>(provider)->Simulation
	);

	ecs::Store retired("retired-failure");
	const auto other = retired.Create("weather");
	controls.BaseHeightMetres = 64000;
	retired.Set(other, controls);
	Tick(retired, 0.1f);
	REQUIRE(retired.Has<weather::StatefulWeatherInitializationFailure>(other));
	const auto oldClock = *retired.Get<scene::WeatherClock>(other);
	controls.Mode = scene::WeatherMode::Procedural;
	retired.Set(other, controls);
	Tick(retired, 0);
	CHECK_FALSE(retired.Has<weather::StatefulWeatherInitializationFailure>(other));
	CHECK_FALSE(retired.Has<weather::StatefulWeatherState>(other));
	CHECK(retired.Get<scene::WeatherClock>(other)->Epoch == oldClock.Epoch + 1);
	CHECK(retired.Get<scene::WeatherClock>(other)->AnchorTick == oldClock.Tick);
}
