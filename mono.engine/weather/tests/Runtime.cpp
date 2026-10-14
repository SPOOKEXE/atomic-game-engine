#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.weather.runtime")

using namespace engine;

namespace {
	std::optional<float> FlatGround(const ecs::Store &, core::Vector3 origin, float maximumDistance) {
		return origin.Y >= 0 && origin.Y <= maximumDistance ? std::optional(0.0f) : std::nullopt;
	}
	ecs::Entity Storm(ecs::Store &store) {
		scene::RegisterSceneClasses();
		weather::RegisterWeatherRuntimeComponents();
		scene::RegisterWeatherClockComponents();
		store.SetResource(weather::WeatherGroundQuery{FlatGround});
		const auto provider = store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Storm");
		scene::WeatherCompute controls;
		controls.BaseTemperatureCelsius = -20;
		controls.TemperatureVariationCelsius = 0;
		controls.IceFraction = 1;
		controls.StormIntensity = 1;
		controls.HailIntensity = 1;
		controls.PrecipitationRate = 20;
		controls.LightningRate = 1;
		controls.WindMetresPerSecond.Y = 8;
		controls.LiftMetresPerSecond = 0;
		controls.TurbulenceMetresPerSecond = 0;
		store.Set(provider, controls);
		scene::WeatherClock clock;
		const auto compiled = weather::Compile(controls);
		REQUIRE(compiled);
		const auto anchor = weather::InitialAnchor(compiled->Definition);
		clock.WindQ20 = anchor.WindQ20;
		clock.DisplacementRemainderQ12 = anchor.DisplacementRemainderQ12;
		store.Set(provider, clock);
		return provider;
	}
	void Step(ecs::Store &store, ecs::Entity provider, uint64_t tick) {
		store.AdvanceTick(0.0f);
		weather::PrepareWeatherRuntime(store, true);
		auto *clock = store.GetMutable<scene::WeatherClock>(provider);
		clock->Tick = tick;
		clock->ElapsedSeconds = tick / clock->TickRate;
		const uint64_t fraction = (tick % clock->TickRate) << 32u;
		clock->ElapsedFractionQ32 = static_cast<uint32_t>(fraction / clock->TickRate);
		clock->ElapsedRemainder = static_cast<uint32_t>(fraction % clock->TickRate);
		weather::UpdateWeatherRuntime(store);
	}
	ecs::Entity BurstStorm(ecs::Store &store) {
		const auto provider = Storm(store);
		auto controls = *store.Get<scene::WeatherCompute>(provider);
		controls.TickRate = 1;
		controls.TimeScale = 16;
		controls.LightningRate = 10;
		store.Set(provider, controls);
		const auto compiled = weather::Compile(controls);
		REQUIRE(compiled);
		const auto anchor = weather::InitialAnchor(compiled->Definition);
		scene::WeatherClock clock;
		clock.TickRate = 1;
		clock.TimeScaleQ16 = anchor.TimeScaleQ16;
		clock.WindQ20 = anchor.WindQ20;
		store.Set(provider, clock);
		return provider;
	}
}

TEST_CASE("headless storm schedules stable copied events through changed-only compilation", "[weather]") {
	ecs::Store first("weather.first");
	ecs::Store second("weather.second");
	const auto firstProvider = Storm(first);
	const auto secondProvider = Storm(second);
	for (uint64_t tick = 1; tick <= 160; ++tick) {
		Step(first, firstProvider, tick);
		Step(second, secondProvider, tick);
	}
	const auto *firstEvents = first.Get<weather::WeatherEvents>(firstProvider);
	const auto *secondEvents = second.Get<weather::WeatherEvents>(secondProvider);
	REQUIRE(firstEvents);
	REQUIRE(secondEvents);
	CHECK_FALSE(firstEvents->Events.empty());
	CHECK(firstEvents->Events == secondEvents->Events);
	CHECK(first.Resource<weather::WeatherRuntimeCache>()->Compilations == 1);
	const auto nextSequence = first.Get<weather::WeatherRuntimeState>(firstProvider)->NextSequence;
	Step(first, firstProvider, 160);
	CHECK(first.Get<weather::WeatherRuntimeState>(firstProvider)->NextSequence == nextSequence);
	CHECK(first.Resource<weather::WeatherRuntimeCache>()->Compilations == 1);
	CHECK(weather::QueryWeather(first, firstProvider, {}).has_value());
	first.ClearChanges();
	auto changed = *first.Get<scene::WeatherCompute>(firstProvider);
	changed.PrecipitationRate = 4;
	++changed.DefinitionRevision;
	first.Set(firstProvider, changed);
	first.GetMutable<scene::WeatherClock>(firstProvider)->DefinitionRevision = changed.DefinitionRevision;
	first.ClearChanges();
	weather::PrepareWeatherRuntime(first, true);
	CHECK(first.Resource<weather::WeatherRuntimeCache>()->Compilations == 2);
	const auto query = weather::QueryWeather(first, firstProvider, {});
	REQUIRE(query);
	CHECK(query->Precipitation.SnowMillimetresPerHour + query->Precipitation.HailMillimetresPerHour == 4);
}

TEST_CASE("storm checkpoints preserve event identity and replicas only receive thunder", "[weather]") {
	ecs::Store store("weather.saved");
	const auto provider = Storm(store);
	for (uint64_t tick = 1; tick <= 80; ++tick)
		Step(store, provider, tick);
	core::ByteWriter writer;
	REQUIRE(store.Save(writer));
	ecs::Store restored("weather.restored");
	core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	REQUIRE(restored.Resource<weather::WeatherGroundQuery>());
	CHECK(restored.Resource<weather::WeatherGroundQuery>()->Height == nullptr);
	restored.SetResource(weather::WeatherGroundQuery{FlatGround});
	CHECK(reader.AtEnd());
	for (uint64_t tick = 81; tick <= 160; ++tick) {
		Step(store, provider, tick);
		Step(restored, provider, tick);
	}
	REQUIRE(store.Get<weather::WeatherEvents>(provider));
	REQUIRE(restored.Get<weather::WeatherEvents>(provider));
	CHECK(
		store.Get<weather::WeatherEvents>(provider)->Events ==
		restored.Get<weather::WeatherEvents>(provider)->Events
	);
	CHECK(
		store.Get<weather::WeatherRuntimeState>(provider)->NextSequence ==
		restored.Get<weather::WeatherRuntimeState>(provider)->NextSequence
	);
	restored.SetAdoptOnly(true);
	restored.SetResource(weather::WeatherListener{});
	const auto before = restored.Get<weather::WeatherEvents>(provider)->Events;
	Step(restored, provider, 200);
	CHECK(restored.Get<weather::WeatherEvents>(provider)->Events == before);
	CHECK_FALSE(restored.Resource<weather::WeatherListener>()->Thunder.Pending.empty());
}

TEST_CASE("storm damage is authoritative once and respects root position and radius", "[weather]") {
	ecs::Store store("weather.damage");
	Storm(store);
	const auto close = store.Create();
	store.Set(close, scene::Humanoid{});
	store.Set(close, scene::Transform{});
	const auto distant = store.Create();
	store.Set(distant, scene::Humanoid{});
	store.Set(distant, scene::Transform{core::CFrame({100, 0, 0})});
	weather::StormEvent event;
	event.Layer = "storm";
	event.Id.Sequence = 1;
	event.Damage = 30;
	event.RadiusMetres = 4;
	weather::WeatherEventCursor cursor;
	REQUIRE(weather::ApplyStormDamage(store, cursor, event));
	CHECK(store.Get<scene::Humanoid>(close)->Health == 70);
	CHECK(store.Get<scene::Humanoid>(distant)->Health == 100);
	CHECK_FALSE(weather::ApplyStormDamage(store, cursor, event));
	CHECK(store.Get<scene::Humanoid>(close)->Health == 70);
	store.SetAdoptOnly(true);
	event.Id.Sequence = 2;
	CHECK_FALSE(weather::ApplyStormDamage(store, cursor, event));
	CHECK(cursor.Last.Sequence == 1);
}

TEST_CASE(
	"large weather gaps retain bounded storm catchup and layer retirement removes derived queries",
	"[weather]"
) {
	ecs::Store store("weather.backlog");
	const auto provider = Storm(store);
	Step(store, provider, 10000);
	REQUIRE(store.Get<weather::WeatherRuntimeState>(provider));
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->LastTick == 32);
	Step(store, provider, 10000);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->LastTick == 64);
	store.Remove<scene::WeatherCompute>(provider);
	weather::PrepareWeatherRuntime(store);
	CHECK(store.Resource<weather::WeatherRuntimeCache>()->Layers.empty());
	CHECK_FALSE(store.Has<weather::WeatherRuntimeState>(provider));
	CHECK_FALSE(store.Has<weather::WeatherEvents>(provider));
	CHECK_FALSE(weather::QueryWeather(store, provider, {}));
}

TEST_CASE("weather runtime follows clock and the host ground binding in the fixed scheduler", "[weather]") {
	ecs::Store store("weather.scheduled");
	const auto provider = Storm(store);
	ecs::Scheduler scheduler;
	weather::RegisterWeatherClockSystem(scheduler);
	scheduler.Add("weather.ground", ecs::Phase::Animation, [](ecs::Store &scheduled) {
		scheduled.SetResource(weather::WeatherGroundQuery{FlatGround});
	});
	store.RemoveResource<weather::WeatherGroundQuery>();
	store.AdvanceTick(0.05f);
	scheduler.RunPhases(store, ecs::Phase::PreSimulation, ecs::Phase::Animation);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider) != nullptr);
	REQUIRE(store.Resource<weather::WeatherGroundQuery>());
	CHECK(store.Resource<weather::WeatherGroundQuery>()->Height != nullptr);
}

TEST_CASE("authoritative impacts resolve elevated and sloped ground before copying identity", "[weather]") {
	ecs::Store elevated("weather.elevated");
	ecs::Store sloped("weather.sloped");
	const auto elevatedProvider = Storm(elevated);
	const auto slopedProvider = Storm(sloped);
	elevated.SetResource(weather::WeatherGroundQuery{[](const ecs::Store &, core::Vector3, float) {
		return std::optional(125.0f);
	}});
	sloped.SetResource(weather::WeatherGroundQuery{[](const ecs::Store &, core::Vector3 origin, float) {
		return std::optional(125.0f + origin.X * 0.01f + origin.Z * 0.02f);
	}});
	for (uint64_t tick = 1; tick <= 40; ++tick) {
		Step(elevated, elevatedProvider, tick);
		Step(sloped, slopedProvider, tick);
	}
	const auto *flatEvents = elevated.Get<weather::WeatherEvents>(elevatedProvider);
	const auto *slopeEvents = sloped.Get<weather::WeatherEvents>(slopedProvider);
	REQUIRE(flatEvents);
	REQUIRE(slopeEvents);
	REQUIRE_FALSE(flatEvents->Events.empty());
	REQUIRE(flatEvents->Events.size() == slopeEvents->Events.size());
	for (size_t index = 0; index < flatEvents->Events.size(); ++index) {
		const auto &flat = flatEvents->Events[index];
		const auto &slope = slopeEvents->Events[index];
		CHECK(flat.EndMetres.Y == 125);
		CHECK(slope.EndMetres.Y == 125.0f + slope.EndMetres.X * 0.01f + slope.EndMetres.Z * 0.02f);
		CHECK(flat.Id == slope.Id);
		CHECK(flat.StartMetres == slope.StartMetres);
		CHECK(flat.BranchSeed == slope.BranchSeed);
	}
	const auto preserved = slopeEvents->Events;
	Step(sloped, slopedProvider, 40);
	CHECK(sloped.Get<weather::WeatherEvents>(slopedProvider)->Events == preserved);
	auto controls = *sloped.Get<scene::WeatherCompute>(slopedProvider);
	controls.Enabled = false;
	++controls.DefinitionRevision;
	sloped.Set(slopedProvider, controls);
	sloped.GetMutable<scene::WeatherClock>(slopedProvider)->DefinitionRevision = controls.DefinitionRevision;
	Step(sloped, slopedProvider, 100);
	CHECK(sloped.Get<weather::WeatherEvents>(slopedProvider)->Events == preserved);
}

TEST_CASE("absent or malformed ground hits refuse event identity and damage", "[weather]") {
	ecs::Store store("weather.no-ground");
	const auto provider = Storm(store);
	store.RemoveResource<weather::WeatherGroundQuery>();
	for (uint64_t tick = 1; tick <= 40; ++tick)
		Step(store, provider, tick);
	REQUIRE(store.Get<weather::WeatherEvents>(provider));
	CHECK(store.Get<weather::WeatherEvents>(provider)->Events.empty());
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence == 1);
	store.SetResource(weather::WeatherGroundQuery{[](const ecs::Store &, core::Vector3, float) {
		return std::optional<float>{};
	}});
	Step(store, provider, 41);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence == 1);
	store.SetResource(weather::WeatherGroundQuery{[](const ecs::Store &, core::Vector3 origin, float) {
		return std::optional(origin.Y + 1);
	}});
	Step(store, provider, 42);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence == 1);
	store.SetResource(weather::WeatherGroundQuery{[](const ecs::Store &, core::Vector3, float) {
		return std::optional(std::numeric_limits<float>::quiet_NaN());
	}});
	Step(store, provider, 43);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence == 1);
	store.SetResource(
		weather::WeatherGroundQuery{[](const ecs::Store &, core::Vector3 origin, float maximum) {
			return std::optional(origin.Y - maximum - 1);
		}}
	);
	Step(store, provider, 44);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence == 1);
	auto controls = *store.Get<scene::WeatherCompute>(provider);
	controls.LightningRate = 0;
	++controls.DefinitionRevision;
	store.Set(provider, controls);
	store.GetMutable<scene::WeatherClock>(provider)->DefinitionRevision = controls.DefinitionRevision;
	store.SetResource(weather::WeatherGroundQuery{FlatGround});
	Step(store, provider, 45);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence > 1);
	CHECK_FALSE(store.Get<weather::WeatherEvents>(provider)->Events.empty());
	for (const auto &event : store.Get<weather::WeatherEvents>(provider)->Events)
		CHECK(event.Kind == weather::StormEventKind::Hail);
}

TEST_CASE("all ambiguous layer names refuse storms and cloned histories resume without replay", "[weather]") {
	ecs::Store store("weather.ambiguous");
	const auto first = Storm(store);
	for (uint64_t tick = 1; tick <= 80; ++tick)
		Step(store, first, tick);
	const auto controls = *store.Get<scene::WeatherCompute>(first);
	const auto clock = *store.Get<scene::WeatherClock>(first);
	const auto history = *store.Get<weather::WeatherEvents>(first);
	const auto state = *store.Get<weather::WeatherRuntimeState>(first);
	const auto clone = store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Storm");
	store.Set(clone, controls);
	store.Set(clone, clock);
	store.Set(clone, state);
	store.Set(clone, history);
	Step(store, first, 81);
	CHECK_FALSE(weather::QueryWeather(store, first, {}));
	CHECK_FALSE(weather::QueryWeather(store, clone, {}));
	CHECK_FALSE(store.Has<weather::WeatherEvents>(first));
	CHECK_FALSE(store.Has<weather::WeatherEvents>(clone));
	CHECK(store.Get<weather::WeatherRuntimeState>(first)->NextSequence == state.NextSequence);
	CHECK(store.Get<weather::WeatherRuntimeState>(clone)->NextSequence == state.NextSequence);
	store.SetInstanceName(clone, "OtherStorm");
	store.GetMutable<scene::WeatherClock>(clone)->Tick = 120;
	Step(store, first, 120);
	CHECK(weather::QueryWeather(store, first, {}).has_value());
	CHECK(weather::QueryWeather(store, clone, {}).has_value());
	REQUIRE(store.Get<weather::WeatherEvents>(first));
	REQUIRE(store.Get<weather::WeatherEvents>(clone));
	REQUIRE_FALSE(store.Get<weather::WeatherEvents>(first)->Events.empty());
	REQUIRE_FALSE(store.Get<weather::WeatherEvents>(clone)->Events.empty());
	const auto &original = store.Get<weather::WeatherEvents>(first)->Events;
	const auto &renamed = store.Get<weather::WeatherEvents>(clone)->Events;
	CHECK(original.front().Id.Sequence >= state.NextSequence);
	CHECK(renamed.front().Id.Sequence >= state.NextSequence);
	CHECK(original.front().Layer != renamed.front().Layer);
	CHECK(renamed.front().Layer == weather::WeatherLayerName(store, clone));
}

TEST_CASE("pending weather edits preserve storm backlog and accepted controls across load", "[weather]") {
	ecs::Store delayed("weather.edited-backlog");
	ecs::Store reference("weather.unedited-backlog");
	const auto provider = Storm(delayed);
	const auto referenceProvider = Storm(reference);
	for (const auto &[world, layer] :
		 {std::pair{&delayed, provider}, std::pair{&reference, referenceProvider}}) {
		scene::InstallServices(*world);
		const auto lighting = scene::ServiceOf(*world, ecs::Classes::Find(core::Name("Lighting")));
		REQUIRE(world->SetParent(layer, lighting));
	}
	delayed.AdvanceTick(2.0f);
	reference.AdvanceTick(2.0f);
	weather::UpdateWeatherClocks(delayed);
	weather::UpdateWeatherClocks(reference);
	weather::UpdateWeatherRuntime(delayed);
	weather::UpdateWeatherRuntime(reference);
	weather::UpdateWeatherRuntime(reference);
	REQUIRE(delayed.Get<scene::WeatherClock>(provider)->Tick == 40);
	REQUIRE(delayed.Get<weather::WeatherRuntimeState>(provider)->LastTick == 32);
	const auto original = *scene::WeatherControlsOf(delayed, provider);
	auto desired = original;
	desired.WindMetresPerSecond.X = 12;
	desired.PrecipitationRate = 4;
	desired.LightningRate = 2;
	++desired.DefinitionRevision;
	delayed.Set(provider, desired);
	delayed.AdvanceTick(0.05f);
	weather::UpdateWeatherClocks(delayed);
	CHECK(delayed.Get<scene::WeatherClock>(provider)->AnchorTick == 0);
	CHECK(*scene::WeatherControlsOf(delayed, provider) == original);
	CHECK(scene::EnvironmentOf(delayed).Weather == original);
	const auto oldSample = weather::QueryWeather(delayed, provider, {});
	REQUIRE(oldSample);
	CHECK(
		oldSample->Precipitation.SnowMillimetresPerHour + oldSample->Precipitation.HailMillimetresPerHour ==
		20
	);
	core::ByteWriter checkpoint;
	REQUIRE(delayed.Save(checkpoint));
	ecs::Store restored("weather.pending-restored");
	core::ByteReader reader(checkpoint.Bytes());
	REQUIRE(restored.Load(reader));
	restored.SetResource(weather::WeatherGroundQuery{FlatGround});
	CHECK(*scene::WeatherControlsOf(restored, provider) == original);
	CHECK(*restored.Get<scene::WeatherCompute>(provider) == desired);
	reference.AdvanceTick(0.05f);
	weather::UpdateWeatherClocks(reference);
	weather::UpdateWeatherRuntime(reference);
	weather::UpdateWeatherRuntime(delayed);
	weather::UpdateWeatherRuntime(restored);
	CHECK(delayed.Get<weather::WeatherRuntimeState>(provider)->LastTick == 41);
	REQUIRE(delayed.Get<weather::WeatherEvents>(provider));
	REQUIRE(reference.Get<weather::WeatherEvents>(referenceProvider));
	CHECK(
		delayed.Get<weather::WeatherEvents>(provider)->Events ==
		reference.Get<weather::WeatherEvents>(referenceProvider)->Events
	);
	CHECK(
		restored.Get<weather::WeatherEvents>(provider)->Events ==
		delayed.Get<weather::WeatherEvents>(provider)->Events
	);
	// Queries cannot accept an edit before the clock's next acceptance boundary.
	REQUIRE(weather::QueryWeather(delayed, provider, {}));
	weather::UpdateWeatherClocks(delayed);
	CHECK(*scene::WeatherControlsOf(delayed, provider) == original);
	for (auto *world : {&delayed, &restored}) {
		world->AdvanceTick(0.05f);
		weather::UpdateWeatherClocks(*world);
		weather::UpdateWeatherRuntime(*world);
		CHECK(*scene::WeatherControlsOf(*world, provider) == desired);
		CHECK(world->Get<scene::WeatherClock>(provider)->AnchorTick == 41);
		CHECK(scene::EnvironmentOf(*world).Weather == desired);
		const auto newSample = weather::QueryWeather(*world, provider, {});
		REQUIRE(newSample);
		CHECK(
			newSample->Precipitation.SnowMillimetresPerHour +
				newSample->Precipitation.HailMillimetresPerHour ==
			4
		);
	}
	CHECK(
		restored.Get<weather::WeatherRuntimeState>(provider)->NextSequence ==
		delayed.Get<weather::WeatherRuntimeState>(provider)->NextSequence
	);
}

TEST_CASE("renaming and reparenting preserve accepted event names until backlog completes", "[weather]") {
	ecs::Store store("weather.named-backlog");
	ecs::Store reference("weather.named-reference");
	const auto provider = Storm(store);
	const auto other = Storm(reference);
	for (auto *world : {&store, &reference}) {
		world->AdvanceTick(2.0f);
		weather::UpdateWeatherClocks(*world);
		weather::UpdateWeatherRuntime(*world);
	}
	weather::UpdateWeatherRuntime(reference);
	const auto originalName = weather::WeatherLayerName(store, provider);
	const auto parent = store.CreateInstance(ecs::Classes::Find(core::Name("Folder")), "NewParent");
	REQUIRE(store.SetParent(provider, parent));
	store.SetInstanceName(provider, "RenamedStorm");
	store.AdvanceTick(0.05f);
	weather::UpdateWeatherClocks(store);
	CHECK(weather::WeatherLayerName(store, provider) == originalName);
	CHECK(store.Resource<weather::WeatherRuntimeCache>()->Layers.front().PendingDefinition);
	core::ByteWriter checkpoint;
	REQUIRE(store.Save(checkpoint));
	ecs::Store restored("weather.named-restored");
	core::ByteReader reader(checkpoint.Bytes());
	REQUIRE(restored.Load(reader));
	restored.SetResource(weather::WeatherGroundQuery{FlatGround});
	CHECK(weather::WeatherLayerName(restored, provider) == originalName);
	reference.AdvanceTick(0.05f);
	weather::UpdateWeatherClocks(reference);
	weather::UpdateWeatherRuntime(reference);
	weather::UpdateWeatherRuntime(store);
	weather::UpdateWeatherRuntime(restored);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->LastTick == 41);
	REQUIRE(store.Get<weather::WeatherEvents>(provider));
	CHECK(
		store.Get<weather::WeatherEvents>(provider)->Events ==
		reference.Get<weather::WeatherEvents>(other)->Events
	);
	CHECK(
		restored.Get<weather::WeatherEvents>(provider)->Events ==
		store.Get<weather::WeatherEvents>(provider)->Events
	);
	const uint64_t nextSequence = store.Get<weather::WeatherRuntimeState>(provider)->NextSequence;
	weather::PrepareWeatherRuntime(store);
	CHECK(weather::WeatherLayerName(store, provider) == originalName);
	for (auto *world : {&store, &restored}) {
		for (uint64_t tick = 42; tick <= 80; ++tick) {
			world->AdvanceTick(0.05f);
			weather::UpdateWeatherClocks(*world);
			weather::UpdateWeatherRuntime(*world);
		}
		const auto acceptedName = weather::WeatherLayerName(*world, provider);
		CHECK(acceptedName != originalName);
		CHECK(acceptedName.find("NewParent") != std::string::npos);
		CHECK(acceptedName.find("RenamedStorm") != std::string::npos);
		REQUIRE(world->Get<weather::WeatherEvents>(provider));
		REQUIRE_FALSE(world->Get<weather::WeatherEvents>(provider)->Events.empty());
		for (const auto &event : world->Get<weather::WeatherEvents>(provider)->Events) {
			CHECK(event.Layer == acceptedName);
			CHECK(event.Tick >= 42);
			CHECK(event.Id.Sequence >= nextSequence);
		}
	}
	CHECK(
		restored.Get<weather::WeatherEvents>(provider)->Events ==
		store.Get<weather::WeatherEvents>(provider)->Events
	);
}

TEST_CASE(
	"reused stream names claim a fresh era while unused names preserve the accepted timeline", "[weather]"
) {
	ecs::Store store("weather.reused-name");
	const auto provider = Storm(store);
	for (uint64_t tick = 1; tick <= 40; ++tick)
		Step(store, provider, tick);
	const auto oldClock = *store.Get<scene::WeatherClock>(provider);
	const auto oldTime = weather::WeatherTimeOf(oldClock);
	REQUIRE(oldTime);
	const auto retired =
		store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "RetiredStorm");
	const auto retiredName = weather::WeatherLayerName(store, retired);
	REQUIRE(weather::ObserveWeatherEpoch(store, retiredName, 9));
	store.Destroy(retired);
	store.SetInstanceName(provider, "RetiredStorm");
	store.AdvanceTick(0.05f);
	weather::UpdateWeatherClocks(store);
	weather::UpdateWeatherRuntime(store);
	const auto renamed = *store.Get<scene::WeatherClock>(provider);
	CHECK(renamed.Epoch == 10);
	CHECK(renamed.AnchorTick == oldClock.Tick);
	const auto retained = weather::ResolveTime(weather::WeatherAnchorOf(renamed), oldClock.Tick);
	REQUIRE(retained);
	CHECK(retained->PhaseTick == oldTime->PhaseTick);
	CHECK(retained->DisplacementQ8 == oldTime->DisplacementQ8);
	CHECK(retained->DisplacementRemainderQ12 == oldTime->DisplacementRemainderQ12);
	const auto elapsed = weather::ElapsedAtTick(renamed, oldClock.Tick);
	REQUIRE(elapsed);
	CHECK(elapsed->Seconds == oldClock.ElapsedSeconds);
	CHECK(elapsed->FractionQ32 == oldClock.ElapsedFractionQ32);
	CHECK(weather::WeatherLayerName(store, provider) == retiredName);
	for (uint64_t tick = 42; tick <= 60; ++tick)
		Step(store, provider, tick);
	REQUIRE(store.Get<weather::WeatherEvents>(provider));
	REQUIRE_FALSE(store.Get<weather::WeatherEvents>(provider)->Events.empty());
	for (const auto &event : store.Get<weather::WeatherEvents>(provider)->Events) {
		CHECK(event.Layer == retiredName);
		CHECK(event.Id.Epoch == 10);
		CHECK(event.Tick > oldClock.Tick);
	}
	store.SetInstanceName(provider, "FreshStorm");
	Step(store, provider, 61);
	CHECK(store.Get<scene::WeatherClock>(provider)->Epoch == 10);
	CHECK(store.Get<scene::WeatherClock>(provider)->AnchorTick == oldClock.Tick);
	CHECK(weather::WeatherLayerName(store, provider).find("FreshStorm") != std::string::npos);
}

TEST_CASE("exhausted stream-name history retains accepted metadata and refuses newborn clocks", "[weather]") {
	ecs::Store store("weather.name-budget");
	const auto provider = Storm(store);
	Step(store, provider, 1);
	const auto acceptedName = weather::WeatherLayerName(store, provider);
	for (uint32_t index = 1; index < weather::MAX_WEATHER_EPOCH_STREAMS; ++index)
		REQUIRE(weather::ObserveWeatherEpoch(store, "retired." + std::to_string(index), 1));
	const auto history = *store.Resource<weather::WeatherEpochHistory>();
	store.SetInstanceName(provider, "CannotAdmitName");
	Step(store, provider, 2);
	CHECK(weather::WeatherLayerName(store, provider) == acceptedName);
	CHECK(*store.Resource<weather::WeatherEpochHistory>() == history);
	CHECK(store.Resource<weather::WeatherRuntimeCache>()->Layers.front().PendingDefinition);
	CHECK(weather::QueryWeather(store, provider, {}).has_value());
	const auto newborn = store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Newborn");
	store.AdvanceTick(0.05f);
	weather::UpdateWeatherClocks(store);
	CHECK_FALSE(store.Has<scene::WeatherAcceptedDefinition>(newborn));
	CHECK_FALSE(store.Has<scene::WeatherClock>(newborn));
	CHECK_FALSE(weather::QueryWeather(store, newborn, {}));
	CHECK(*store.Resource<weather::WeatherEpochHistory>() == history);
	store.SetInstanceName(provider, "Storm");
	Step(store, provider, store.Get<scene::WeatherClock>(provider)->Tick + 1);
	CHECK(weather::WeatherLayerName(store, provider) == acceptedName);
	CHECK_FALSE(store.Resource<weather::WeatherRuntimeCache>()->Layers.front().PendingDefinition);
}

TEST_CASE("coarse fast weather emits every bounded rate crossing with ordered IDs and damage", "[weather]") {
	ecs::Store store("weather.maximum-rate");
	const auto provider = BurstStorm(store);
	const auto controls = *store.Get<scene::WeatherCompute>(provider);
	const auto column = weather::StormColumn(
		controls.Seed, weather::WeatherLayerKey(controls.LayerName.Text()), 1, 1, controls.ExtentMetres
	);
	const auto target = store.Create("Target");
	scene::Humanoid health;
	health.Health = 20000;
	health.MaxHealth = 20000;
	store.Set(target, health);
	store.Set(target, scene::Transform{core::CFrame(column)});
	Step(store, provider, 1);
	const auto *state = store.Get<weather::WeatherRuntimeState>(provider);
	REQUIRE(state);
	CHECK(state->NextSequence == 177);
	CHECK(state->ChargeQ16 == 0);
	CHECK(state->HailGrowthQ16 == 0);
	CHECK(store.Get<scene::Humanoid>(target)->Health == 11920);
	const auto *journal = store.Get<weather::WeatherEvents>(provider);
	REQUIRE(journal);
	CHECK(journal->Dropped == 144);
	REQUIRE(journal->Events.size() == weather::MAX_WEATHER_EVENTS);
	for (size_t index = 0; index < journal->Events.size(); ++index) {
		const auto &event = journal->Events[index];
		CHECK(event.Id.Sequence == 145 + index);
		CHECK(
			event.Kind == (index < 16 ? weather::StormEventKind::Lightning : weather::StormEventKind::Hail)
		);
		CHECK(event.Tick == 1);
		CHECK(event.TickRate == 1);
		CHECK(event.ElapsedSeconds == 1);
		CHECK(event.ElapsedFractionQ32 == 0);
		CHECK(event.EndMetres == column);
	}
	Step(store, provider, 1);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence == 177);
	CHECK(store.Get<scene::Humanoid>(target)->Health == 11920);
	Step(store, provider, 2);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence == 353);
	CHECK(store.Get<weather::WeatherEvents>(provider)->Dropped == 320);
	CHECK(store.Get<weather::WeatherEvents>(provider)->Events.front().Id.Sequence == 321);
}

TEST_CASE(
	"refused coarse impacts retain one pending crossing and never bank an unbounded burst", "[weather]"
) {
	ecs::Store store("weather.refused-maximum-rate");
	const auto provider = BurstStorm(store);
	store.RemoveResource<weather::WeatherGroundQuery>();
	for (uint64_t tick = 1; tick <= 10; ++tick) {
		Step(store, provider, tick);
		CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence == 1);
		CHECK(store.Get<weather::WeatherRuntimeState>(provider)->ChargeQ16 == 65536);
		CHECK(store.Get<weather::WeatherRuntimeState>(provider)->HailGrowthQ16 == 65536);
	}
	store.SetResource(weather::WeatherGroundQuery{FlatGround});
	Step(store, provider, 11);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->NextSequence == 177);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->ChargeQ16 == 65536);
	CHECK(store.Get<weather::WeatherRuntimeState>(provider)->HailGrowthQ16 == 65536);
	CHECK(store.Get<weather::WeatherEvents>(provider)->Dropped == 144);
	for (const auto &event : store.Get<weather::WeatherEvents>(provider)->Events)
		CHECK(event.Tick == 11);
}
