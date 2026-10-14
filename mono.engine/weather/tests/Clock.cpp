#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
TEST_SUITE_ID("engine.weather.clock")

using namespace engine;

namespace {
	void Register() {
		scene::RegisterSceneComponents();
		weather::RegisterWeatherRuntimeComponents();
	}
	void Tick(ecs::Store &store, float seconds) {
		store.AdvanceTick(seconds);
		weather::UpdateWeatherClocks(store);
		weather::UpdateWeatherRuntime(store);
	}
	std::optional<float> EpochGround(const ecs::Store &, core::Vector3, float) {
		return 0.0f;
	}
	ecs::Entity CreateEpochStorm(ecs::Store &store) {
		scene::RegisterSceneClasses();
		const auto provider = store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Storm");
		scene::WeatherCompute controls;
		controls.Coverage = 1;
		controls.BaseTemperatureCelsius = -20;
		controls.TemperatureVariationCelsius = 0;
		controls.StormIntensity = 1;
		controls.LightningRate = 10;
		controls.IceFraction = 1;
		controls.PrecipitationRate = 20;
		store.Set(provider, controls);
		store.SetResource(weather::WeatherGroundQuery{EpochGround});
		return provider;
	}
	std::optional<weather::StormEvent> NextEpochLightning(ecs::Store &store, ecs::Entity provider) {
		for (uint32_t tick = 0; tick < 400; ++tick) {
			Tick(store, 0.05f);
			const auto *events = store.Get<weather::WeatherEvents>(provider);
			if (!events) continue;
			const auto lightning =
				std::find_if(events->Events.begin(), events->Events.end(), [](const auto &event) {
					return event.Kind == weather::StormEventKind::Lightning;
				});
			if (lightning != events->Events.end()) return *lightning;
		}
		return {};
	}
}

TEST_CASE(
	"weather clock runs on fixed simulation time and preserves time scale and pause anchors", "[weather]"
) {
	Register();
	ecs::Store store("weather_clock");
	const auto layer = store.Create("Layer");
	scene::WeatherCompute controls;
	controls.TickRate = 20;
	controls.WindMetresPerSecond = {40, 0, 0};
	controls.CellSizeMetres = 10;
	store.Set(layer, controls);
	Tick(store, 1);
	const auto initial = scene::WeatherClockOf(store, layer);
	CHECK(initial.Tick == 20);
	const auto beforePause = weather::WeatherTimeOf(initial);
	REQUIRE(beforePause);
	CHECK(beforePause->PhaseTick == 20);
	CHECK(beforePause->DisplacementQ8[0] > 0);
	weather::UpdateWeatherClocks(store);
	CHECK(scene::WeatherClockOf(store, layer).Tick == 20);
	controls.Paused = true;
	store.Set(layer, controls);
	Tick(store, 1);
	const auto paused = weather::WeatherTimeOf(scene::WeatherClockOf(store, layer));
	REQUIRE(paused);
	CHECK(paused->PhaseTick == beforePause->PhaseTick);
	CHECK(paused->DisplacementQ8 == beforePause->DisplacementQ8);
	store.Observe<scene::WeatherClock>();
	store.ClearChanges();
	Tick(store, 1);
	CHECK_FALSE(store.Changed<scene::WeatherClock>(layer));
	controls.Paused = false;
	controls.TimeScale = 0.5f;
	controls.WindMetresPerSecond = {-40, 0, 0};
	controls.DefinitionRevision++;
	store.Set(layer, controls);
	Tick(store, 1);
	const auto resumedClock = scene::WeatherClockOf(store, layer);
	const auto resumed = weather::WeatherTimeOf(resumedClock);
	REQUIRE(resumed);
	CHECK(resumed->PhaseTick == 30);
	const uint64_t beforeDisplacement = (static_cast<uint64_t>(beforePause->DisplacementQ8[0]) << 12) |
										beforePause->DisplacementRemainderQ12[0];
	const uint64_t expectedDisplacement =
		(beforeDisplacement + 20u * static_cast<uint64_t>(static_cast<int64_t>(resumedClock.WindQ20[0]))) &
		((uint64_t{1} << 44) - 1);
	CHECK(resumed->DisplacementQ8[0] == static_cast<uint32_t>(expectedDisplacement >> 12));
	CHECK(resumed->DisplacementRemainderQ12[0] == (expectedDisplacement & 4095));
	CHECK(resumedClock.AnchorTick == 20);
	CHECK(store.Time().Elapsed == 4);
}

TEST_CASE(
	"named weather epochs survive provider retirement and save load for existing listeners",
	"[weather][epoch]"
) {
	Register();
	ecs::Store store("epoch_lifecycle");
	const auto first = CreateEpochStorm(store);
	const auto firstEvent = NextEpochLightning(store, first);
	REQUIRE(firstEvent);
	weather::ThunderQueue listener;
	REQUIRE(weather::QueueThunder(listener, *firstEvent, {}, 15, firstEvent->Tick));
	const auto layer = weather::WeatherLayerName(store, first);
	const auto originalEpoch = scene::WeatherClockOf(store, first).Epoch;
	store.Destroy(first);
	const auto replacement = CreateEpochStorm(store);
	const auto secondEvent = NextEpochLightning(store, replacement);
	REQUIRE(secondEvent);
	CHECK(weather::WeatherLayerName(store, replacement) == layer);
	CHECK(scene::WeatherClockOf(store, replacement).Epoch == originalEpoch + 1);
	CHECK(secondEvent->Id != firstEvent->Id);
	REQUIRE(weather::QueueThunder(listener, *secondEvent, {}, 15, secondEvent->Tick));
	CHECK_FALSE(weather::QueueThunder(listener, *firstEvent, {}, 15, firstEvent->Tick));
	const auto beforeLoadEpoch = scene::WeatherClockOf(store, replacement).Epoch;
	store.Destroy(replacement);
	core::ByteWriter saved;
	REQUIRE(store.Save(saved));
	ecs::Store loaded("epoch_loaded");
	core::ByteReader reader(saved.Bytes());
	REQUIRE(loaded.Load(reader));
	const auto afterLoad = CreateEpochStorm(loaded);
	const auto thirdEvent = NextEpochLightning(loaded, afterLoad);
	REQUIRE(thirdEvent);
	CHECK(scene::WeatherClockOf(loaded, afterLoad).Epoch == beforeLoadEpoch + 1);
	CHECK(thirdEvent->Id != secondEvent->Id);
	REQUIRE(weather::QueueThunder(listener, *thirdEvent, {}, 15, thirdEvent->Tick));
}

TEST_CASE(
	"weather sample sequence rollover starts a fresh anchor without replaying old storm damage",
	"[weather][epoch]"
) {
	Register();
	ecs::Store store("sample_rollover");
	const auto provider = CreateEpochStorm(store);
	auto controls = *store.Get<scene::WeatherCompute>(provider);
	controls.CellSizeMetres = 1;
	controls.ExtentMetres = {1, 4096, 1};
	store.Set(provider, controls);
	const auto victim = store.Create();
	store.Set(victim, scene::Humanoid{});
	store.Set(victim, scene::Transform{});
	const auto original = NextEpochLightning(store, provider);
	REQUIRE(original);
	REQUIRE(store.Get<scene::Humanoid>(victim)->Health < 100);
	store.Set(victim, scene::Humanoid{});
	auto before = scene::WeatherClockOf(store, provider);
	before.Sequence = UINT64_MAX;
	store.Set(provider, before);
	Tick(store, 0.05f);
	const auto rolled = scene::WeatherClockOf(store, provider);
	CHECK(rolled.Epoch == before.Epoch + 1);
	CHECK(rolled.Sequence == 1);
	CHECK(rolled.AnchorTick == rolled.Tick);
	const auto expected = weather::ResolveTime(weather::WeatherAnchorOf(before), rolled.Tick);
	const auto resolved = weather::WeatherTimeOf(rolled);
	REQUIRE(expected);
	REQUIRE(resolved);
	CHECK(resolved->PhaseTick == expected->PhaseTick);
	CHECK(resolved->DisplacementQ8 == expected->DisplacementQ8);
	CHECK(resolved->DisplacementRemainderQ12 == expected->DisplacementRemainderQ12);
	CHECK(rolled.ElapsedSeconds >= before.ElapsedSeconds);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->LastTick == rolled.Tick);
	CHECK(store.Get<weather::WeatherEvents>(provider)->Events.empty());
	CHECK(store.Get<scene::Humanoid>(victim)->Health == 100);
	const auto next = NextEpochLightning(store, provider);
	REQUIRE(next);
	CHECK(next->Id.Epoch == rolled.Epoch);
	CHECK(next->Tick > rolled.AnchorTick);
	CHECK(store.Get<scene::Humanoid>(victim)->Health < 100);
}

TEST_CASE(
	"weather epoch budgets and per-name ordering refuse collisions without eviction", "[weather][epoch]"
) {
	Register();
	ecs::Store forward("epoch_forward"), reverse("epoch_reverse");
	for (const auto name : {"A", "B", "C"})
		REQUIRE(weather::ClaimWeatherEpoch(forward, name) == 1);
	for (const auto name : {"C", "B", "A"})
		REQUIRE(weather::ClaimWeatherEpoch(reverse, name) == 1);
	CHECK(
		*forward.Resource<weather::WeatherEpochHistory>() == *reverse.Resource<weather::WeatherEpochHistory>()
	);
	REQUIRE(weather::ClaimWeatherEpoch(forward, "B") == 2);
	REQUIRE(weather::ClaimWeatherEpoch(forward, "A") == 2);
	REQUIRE(weather::ClaimWeatherEpoch(reverse, "A") == 2);
	CHECK(weather::ClaimWeatherEpoch(forward, "A") == weather::ClaimWeatherEpoch(reverse, "A"));
	ecs::Store full("epoch_full");
	for (uint32_t index = 0; index < weather::MAX_WEATHER_EPOCH_STREAMS; ++index)
		REQUIRE(weather::ObserveWeatherEpoch(full, "layer_" + std::to_string(index), 0));
	const auto before = *full.Resource<weather::WeatherEpochHistory>();
	CHECK_FALSE(weather::ObserveWeatherEpoch(full, "over_budget", 0));
	CHECK_FALSE(weather::ClaimWeatherEpoch(full, "over_budget"));
	CHECK(*full.Resource<weather::WeatherEpochHistory>() == before);
	REQUIRE(weather::ClaimWeatherEpoch(full, "layer_0") == 1);
	REQUIRE(weather::ObserveWeatherEpoch(full, "layer_0", 99));
	REQUIRE(weather::ClaimWeatherEpoch(full, "layer_0", 50) == 100);
	REQUIRE(weather::ObserveWeatherEpoch(full, "layer_0", UINT64_MAX));
	const auto exhausted = *full.Resource<weather::WeatherEpochHistory>();
	CHECK_FALSE(weather::ClaimWeatherEpoch(full, "layer_0"));
	CHECK(*full.Resource<weather::WeatherEpochHistory>() == exhausted);
	full.SetAdoptOnly(true);
	CHECK_FALSE(weather::ObserveWeatherEpoch(full, "layer_0", 0));
	CHECK_FALSE(weather::ClaimWeatherEpoch(full, "layer_1"));
	CHECK(*full.Resource<weather::WeatherEpochHistory>() == exhausted);
}

TEST_CASE(
	"weather epoch history codec refuses malformed names counts and ordering transactionally",
	"[weather][epoch]"
) {
	Register();
	weather::WeatherEpochHistory history{{{"A", 0}, {"B", UINT64_MAX}}};
	core::ByteWriter encoded;
	weather::WriteWeatherEpochHistory(encoded, history);
	weather::WeatherEpochHistory decoded;
	core::ByteReader complete(encoded.Bytes());
	REQUIRE(weather::ReadWeatherEpochHistory(complete, decoded));
	CHECK(decoded == history);
	for (size_t size = 0; size < encoded.Size(); ++size) {
		core::ByteReader truncated(encoded.Bytes().first(size));
		CHECK_FALSE(weather::ReadWeatherEpochHistory(truncated, decoded));
		CHECK(decoded == history);
	}
	for (const auto &invalid : std::array<weather::WeatherEpochHistory, 4>{
			 weather::WeatherEpochHistory{{{"", 1}}},
			 weather::WeatherEpochHistory{{{std::string(weather::MAX_WEATHER_LAYER_NAME_BYTES + 1, 'x'), 1}}},
			 weather::WeatherEpochHistory{{{"B", 1}, {"A", 2}}},
			 weather::WeatherEpochHistory{{{"A", 1}, {"A", 2}}}
		 }) {
		core::ByteWriter bytes;
		weather::WriteWeatherEpochHistory(bytes, invalid);
		core::ByteReader malformed(bytes.Bytes());
		CHECK_FALSE(weather::ReadWeatherEpochHistory(malformed, decoded));
		CHECK(decoded == history);
	}
	core::ByteWriter overBudget;
	overBudget.WriteString("weather.epoch-history.v1");
	overBudget.WriteUInt32(weather::MAX_WEATHER_EPOCH_STREAMS + 1);
	core::ByteReader excessive(overBudget.Bytes());
	CHECK_FALSE(weather::ReadWeatherEpochHistory(excessive, decoded));
	CHECK(decoded == history);
}

TEST_CASE("cold weather join reconstructs the same field without replay or local elapsed time", "[weather]") {
	Register();
	ecs::Store authority("weather_authority");
	const auto layer = authority.Create("Layer");
	scene::WeatherCompute controls;
	authority.Set(layer, controls);
	for (int tick = 0; tick < 50; ++tick)
		Tick(authority, 0.125f);
	const auto clock = scene::WeatherClockOf(authority, layer);
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	const auto continuous = weather::WeatherTimeOf(clock);
	REQUIRE(continuous);
	ecs::Store replica("weather_cold_join");
	replica.AdvanceTick(0.01f);
	const auto localLayer = replica.Create("Layer");
	replica.Set(localLayer, clock);
	replica.SetAdoptOnly(true);
	scene::SynchronizeWeatherClocks(replica, 1000, 0);
	weather::UpdateWeatherClocks(replica);
	const auto joined = weather::WeatherTimeOf(scene::WeatherClockOf(replica, localLayer));
	REQUIRE(joined);
	CHECK(joined->PhaseTick == continuous->PhaseTick);
	CHECK(joined->DisplacementQ8 == continuous->DisplacementQ8);
	CHECK(
		weather::Evaluate(compiled->Definition, *joined, {17, 20, -9}) ==
		weather::Evaluate(compiled->Definition, *continuous, {17, 20, -9})
	);
	CHECK(replica.Time().Elapsed == Catch::Approx(0.01f));
}

TEST_CASE("weather clock is independent of render frame counts and survives save load", "[weather]") {
	Register();
	ecs::Store coarse("weather_coarse"), fine("weather_fine");
	const auto first = coarse.Create("Layer"), second = fine.Create("Layer");
	coarse.Set(first, scene::WeatherCompute{});
	fine.Set(second, scene::WeatherCompute{});
	for (int count = 0; count < 8; ++count)
		Tick(coarse, 0.125f);
	for (int count = 0; count < 32; ++count)
		Tick(fine, 0.03125f);
	CHECK(scene::WeatherClockOf(coarse, first).Tick == scene::WeatherClockOf(fine, second).Tick);
	CHECK(scene::WeatherClockOf(coarse, first).Fraction == scene::WeatherClockOf(fine, second).Fraction);
	CHECK(
		scene::WeatherClockOf(coarse, first).ElapsedSeconds ==
		scene::WeatherClockOf(fine, second).ElapsedSeconds
	);
	CHECK(
		scene::WeatherClockOf(coarse, first).ElapsedFractionQ32 ==
		scene::WeatherClockOf(fine, second).ElapsedFractionQ32
	);
	CHECK(
		scene::WeatherClockOf(coarse, first).ElapsedRemainder ==
		scene::WeatherClockOf(fine, second).ElapsedRemainder
	);
	engine::core::ByteWriter snapshot;
	REQUIRE(coarse.Save(snapshot));
	ecs::Store loaded("weather_loaded");
	engine::core::ByteReader reader(snapshot.Bytes());
	REQUIRE(loaded.Load(reader));
	Tick(coarse, 0.125f);
	Tick(loaded, 0.125f);
	CHECK(scene::WeatherClockOf(coarse, first).Tick == scene::WeatherClockOf(loaded, first).Tick);
	CHECK(
		weather::WeatherTimeOf(scene::WeatherClockOf(coarse, first))->DisplacementQ8 ==
		weather::WeatherTimeOf(scene::WeatherClockOf(loaded, first))->DisplacementQ8
	);
}

TEST_CASE("world suspension freezes copied clocks while preserving authored pauses", "[weather]") {
	Register();
	ecs::Store store("suspended_weather");
	const auto running = store.Create("Running");
	const auto authoredPaused = store.Create("AuthoredPaused");
	scene::WeatherCompute controls;
	store.Set(running, controls);
	controls.Paused = true;
	store.Set(authoredPaused, controls);
	Tick(store, 1);
	const auto before = scene::WeatherClockOf(store, running);
	REQUIRE(weather::SetWeatherWorldPaused(store, true));
	const auto suspended = scene::WeatherClockOf(store, running);
	CHECK(suspended.WorldPaused);
	CHECK_FALSE(suspended.Paused);
	CHECK(suspended.Tick == before.Tick);
	CHECK(suspended.AnchorTick == before.AnchorTick);
	CHECK(suspended.DisplacementQ8 == before.DisplacementQ8);
	CHECK(suspended.ElapsedSeconds == before.ElapsedSeconds);
	CHECK(suspended.ElapsedFractionQ32 == before.ElapsedFractionQ32);
	CHECK(suspended.Sequence > before.Sequence);
	REQUIRE(store.Resource<scene::WeatherClockPublication>());
	CHECK(store.Resource<scene::WeatherClockPublication>()->Pending);
	scene::WeatherClockEstimate client;
	REQUIRE(scene::SampleWeatherClock(client, suspended, 1000, 0.2));
	scene::AdvanceWeatherClock(client, 10000);
	CHECK(client.Tick == suspended.Tick);
	CHECK_FALSE(weather::SetWeatherWorldPaused(store, true));
	REQUIRE(weather::SetWeatherWorldPaused(store, false));
	CHECK_FALSE(scene::WeatherClockOf(store, running).Paused);
	CHECK(scene::WeatherClockOf(store, authoredPaused).Paused);
	Tick(store, 1);
	CHECK(scene::WeatherClockOf(store, running).Tick == before.Tick + controls.TickRate);
	CHECK(scene::WeatherClockOf(store, authoredPaused).Tick == 0);
}

TEST_CASE("disabled weather freezes its accepted timeline and resumes the same displacement", "[weather]") {
	Register();
	ecs::Store store("disabled_weather_clock");
	const auto layer = store.Create("Layer");
	scene::WeatherCompute controls;
	store.Set(layer, controls);
	Tick(store, 1);
	const auto before = weather::WeatherTimeOf(scene::WeatherClockOf(store, layer));
	REQUIRE(before);
	controls.Enabled = false;
	store.Set(layer, controls);
	Tick(store, 100);
	const auto disabled = scene::WeatherClockOf(store, layer);
	CHECK(disabled.Paused);
	CHECK_FALSE(disabled.WorldPaused);
	CHECK_FALSE(store.Get<scene::WeatherCompute>(layer)->Paused);
	CHECK(weather::WeatherTimeOf(disabled)->PhaseTick == before->PhaseTick);
	CHECK(weather::WeatherTimeOf(disabled)->DisplacementQ8 == before->DisplacementQ8);
	controls.Enabled = true;
	store.Set(layer, controls);
	Tick(store, 1);
	const auto resumed = scene::WeatherClockOf(store, layer);
	CHECK_FALSE(resumed.Paused);
	CHECK(weather::WeatherTimeOf(resumed)->PhaseTick == before->PhaseTick + controls.TickRate);
}

TEST_CASE("accepted weather elapsed survives rate edits without scaling physical thunder time", "[weather]") {
	Register();
	ecs::Store store("physical_weather_clock");
	const auto layer = store.Create("Layer");
	scene::WeatherCompute controls;
	store.Set(layer, controls);
	Tick(store, 1);
	CHECK(scene::WeatherClockOf(store, layer).ElapsedSeconds == 1);
	controls.TickRate = 100;
	controls.TimeScale = 0.25f;
	store.Set(layer, controls);
	Tick(store, 1);
	const auto changed = scene::WeatherClockOf(store, layer);
	CHECK(changed.Tick == 120);
	CHECK(changed.ElapsedSeconds == 2);
	CHECK(changed.ElapsedFractionQ32 == 0);
	const auto boundary = weather::ElapsedAtTick(changed, 20);
	REQUIRE(boundary);
	CHECK(boundary->Seconds == 1);
	CHECK(boundary->FractionQ32 == 0);
	CHECK_FALSE(weather::ElapsedAtTick(changed, 19));
	CHECK_FALSE(weather::ElapsedAtTick(changed, 121));
	for (int drain = 0; drain < 4; ++drain)
		weather::UpdateWeatherRuntime(store);
	controls.TickRate = 40;
	store.Set(layer, controls);
	Tick(store, 1);
	const auto final = scene::WeatherClockOf(store, layer);
	CHECK(final.TickRate == 40);
	CHECK(final.Tick == 160);
	CHECK(final.ElapsedSeconds == 3);
	CHECK(final.ElapsedFractionQ32 == 0);
	const auto secondBoundary = weather::ElapsedAtTick(final, 120);
	REQUIRE(secondBoundary);
	CHECK(secondBoundary->Seconds == 2);
	CHECK(secondBoundary->FractionQ32 == 0);
}
