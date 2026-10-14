#include <engine/audio/Sample.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Runtime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <client/Sounds.hpp>
#include <client/WeatherSounds.hpp>
#include <cmath>

TEST_SUITE_ID("client.weather-sounds")
TEST_DEPENDS("engine.weather.events")
TEST_DEPENDS("engine.audio.mixer")

using namespace engine;

namespace {
	float RenderFrames(audio::AudioMixer &mixer, uint64_t frames) {
		audio::SampleBuffer block(mixer.Format(), audio::DEFAULT_BLOCK_FRAMES);
		float peak = 0;
		while (frames != 0) {
			const auto count = static_cast<size_t>(std::min<uint64_t>(frames, audio::DEFAULT_BLOCK_FRAMES));
			block.Resize(count);
			block.Silence();
			const auto report = mixer.Render(block);
			REQUIRE(report.Frames == count);
			peak = std::max(peak, block.Peak());
			frames -= count;
		}
		return peak;
	}
	ecs::Entity Storm(ecs::Store &store, uint64_t sequence = 1) {
		scene::RegisterSceneClasses();
		weather::RegisterWeatherRuntimeComponents();
		const auto provider = store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Storm");
		REQUIRE(provider != ecs::NULL_ENTITY);
		store.Set(provider, scene::WeatherClock{});
		weather::StormEvent event;
		event.Layer = weather::WeatherLayerName(store, provider);
		event.Id = {1, sequence, 1};
		event.StartMetres = {0, 100, 0};
		event.EndMetres = {0, 0, 0};
		event.BranchSeed = 9;
		store.Set(provider, weather::WeatherEvents{{event}, 0});
		return provider;
	}
}

TEST_CASE("thunder uses its layer clock and an exact sample deadline once", "[weather][audio]") {
	ecs::Store store("weather.audio");
	const auto provider = Storm(store);
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	const auto &arrival = store.Resource<weather::WeatherListener>()->Thunder.Pending.front();
	const uint64_t expected =
		static_cast<uint64_t>(std::llround(arrival.DelaySeconds * mixer.Format().SampleRate));
	CHECK(stage.Voices.front().StartSample == expected);
	CHECK(expected % (mixer.Format().SampleRate / 20) != 0);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	CHECK(stage.Starts == 1);
	CHECK(RenderFrames(mixer, expected) == 0);
	CHECK(mixer.Clock() == expected);
	CHECK(RenderFrames(mixer, 1024) > 0);
	CHECK(stage.Clear(mixer));
	mixer.ApplyPending();
	CHECK(stage.Voices.empty());
	CHECK(store.Get<weather::WeatherEvents>(provider)->Events.size() == 1);
}

TEST_CASE("full audio queue preserves a thunder arrival for retry", "[weather][audio]") {
	ecs::Store store("weather.audio.retry");
	(void)Storm(store);
	audio::AudioMixer mixer;
	while (mixer.Commands().Free() != 0) {
		audio::Command command;
		REQUIRE(mixer.Commands().Post(command));
	}
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {}, catalogue);
	CHECK(stage.Starts == 0);
	CHECK(stage.Refused == 1);
	REQUIRE(store.Resource<weather::WeatherListener>()->Thunder.Pending.size() == 1);
	mixer.ApplyPending();
	stage.Sync(store, mixer, {}, catalogue);
	CHECK(stage.Starts == 1);
	CHECK(stage.Voices.size() == 1);
	CHECK(stage.Clear(mixer));
	mixer.ApplyPending();
}

TEST_CASE("skipping a completed thunder waveform does not restart its event", "[weather][audio]") {
	ecs::Store store("weather.audio.skip");
	(void)Storm(store);
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	CHECK(RenderFrames(mixer, stage.Voices.front().EndSample + 1024) > 0);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	CHECK(stage.Starts == 1);
	CHECK(stage.Voices.empty());
	CHECK(store.Resource<weather::WeatherListener>()->Thunder.Pending.empty());
	mixer.ApplyPending();
}

TEST_CASE("paused weather cancels a future sample command and resumes the same event", "[weather][audio]") {
	ecs::Store store("weather.audio.pause");
	const auto provider = Storm(store);
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Paused = true;
	store.Set(provider, clock);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	CHECK(stage.Voices.empty());
	CHECK(RenderFrames(mixer, 8000) == 0);
	clock.Paused = false;
	store.Set(provider, clock);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	CHECK(stage.Voices.front().StartSample > mixer.Clock());
	CHECK(stage.Starts == 2);
	CHECK(stage.Clear(mixer));
	mixer.ApplyPending();
}

TEST_CASE("skipped layer rate edits retain physical thunder age at huge elapsed time", "[weather][audio]") {
	ecs::Store store("weather.audio.rate");
	const auto provider = Storm(store);
	const uint64_t seconds = UINT64_MAX - 100;
	auto events = *store.Get<weather::WeatherEvents>(provider);
	events.Events.front().Tick = 1000;
	events.Events.front().ElapsedSeconds = seconds;
	store.Set(provider, events);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Tick = 1000;
	clock.ElapsedSeconds = seconds;
	store.Set(provider, clock);
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {150, 0, 0}, catalogue);
	CHECK(stage.Voices.empty());
	const double delay = store.Resource<weather::WeatherListener>()->Thunder.Pending.front().DelaySeconds;
	clock.TickRate = 100;
	clock.Tick = 1025;
	clock.ElapsedFractionQ32 = uint32_t{1} << 30;
	store.Set(provider, clock);
	// The adapter misses this intermediate copied rate. The canonical acoustic
	// pair alone preserves age when its next snapshot uses another base rate.
	clock.TickRate = 40;
	clock.Tick = 1030;
	clock.ElapsedFractionQ32 = uint32_t{3} << 29;
	clock.Fraction = 0.5;
	store.Set(provider, clock);
	stage.Sync(store, mixer, {150, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	const auto expected =
		static_cast<uint64_t>(std::llround((delay - 0.375 - 0.5 / 40) * mixer.Format().SampleRate));
	CHECK(stage.Voices.front().StartSample == expected);
	CHECK(stage.Clear(mixer));
	mixer.ApplyPending();
}
