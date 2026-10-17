#include <engine/audio/Sample.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Runtime.hpp>

#include <catch2/catch_approx.hpp>
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
	audio::SampleBuffer RenderBlock(audio::AudioMixer &mixer) {
		audio::SampleBuffer block(mixer.Format(), 512);
		const auto report = mixer.Render(block);
		REQUIRE(report.Frames == 512);
		return block;
	}
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

TEST_CASE(
	"expired latejoin thunder stays silent and consumes the retained journal",
	"[weather][audio][thunder-history]"
) {
	ecs::Store store("weather.audio.old_journal");
	const auto provider = Storm(store);
	auto journal = *store.Get<weather::WeatherEvents>(provider);
	const auto original = journal.Events.front();
	journal.Events.clear();
	for (uint64_t sequence = 1; sequence <= weather::MAX_WEATHER_EVENTS; ++sequence) {
		auto event = original;
		event.Id.Sequence = sequence;
		event.Tick = sequence;
		event.ElapsedSeconds = sequence;
		journal.Events.push_back(event);
	}
	store.Set(provider, journal);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Tick = 1000;
	clock.ElapsedSeconds = 50;
	store.Set(provider, clock);
	CHECK_FALSE(client::WeatherThunderDueSoon(store, {34, 0, 0}));
	REQUIRE(store.Resource<weather::WeatherListener>());
	CHECK(store.Resource<weather::WeatherListener>()->Thunder.Pending.empty());
	REQUIRE(store.Resource<weather::WeatherListener>()->Thunder.Cursors.size() == 1);
	CHECK(store.Resource<weather::WeatherListener>()->Thunder.Cursors.front().Last.Sequence == 32);
	CHECK_FALSE(client::WeatherThunderDueSoon(store, {34, 0, 0}));
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	CHECK(stage.Voices.empty());
	CHECK(stage.Starts == 0);
	CHECK_FALSE(stage.Thunder);
	CHECK(RenderBlock(mixer).Peak() == 0);
	CHECK(mixer.LastAppliedCommands().empty());
}

TEST_CASE(
	"partly late thunder seeks to the audible tail at huge elapsed time", "[weather][audio][thunder-history]"
) {
	ecs::Store store("weather.audio.partial");
	const auto provider = Storm(store);
	const uint64_t emittedSeconds = UINT64_MAX - 10;
	auto events = *store.Get<weather::WeatherEvents>(provider);
	events.Events.front().ElapsedSeconds = emittedSeconds;
	store.Set(provider, events);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Tick = 30;
	clock.ElapsedSeconds = emittedSeconds + 1;
	clock.ElapsedFractionQ32 = uint32_t{1} << 31;
	store.Set(provider, clock);
	REQUIRE(client::WeatherThunderDueSoon(store, {}));
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	REQUIRE(stage.Thunder);
	CHECK(stage.Thunder->Frames() == 2 * mixer.Format().SampleRate);
	const auto offset = mixer.Format().SampleRate * 3 / 2;
	CHECK(stage.Voices.front().StartSample == 0);
	CHECK(stage.Voices.front().EndSample == mixer.Format().SampleRate / 2);
	const auto block = RenderBlock(mixer);
	CHECK(block.Peak() > 0);
	CHECK(block.Data().front() == Catch::Approx(stage.Thunder->Frame(offset).front() * 0.3f));
	REQUIRE(mixer.Graph().Find(stage.Voices.front().Player));
	CHECK(mixer.Graph().Find(stage.Voices.front().Player)->Cursor == offset + 512);
	CHECK(mixer.Graph().Find(stage.Voices.front().Player)->NotBeforeSample == 0);
	const auto end = stage.Voices.front().EndSample;
	while (mixer.Clock() < end)
		(void)RenderBlock(mixer);
	CHECK_FALSE(mixer.Graph().Find(stage.Voices.front().Player)->Playing);
	stage.Sync(store, mixer, {}, catalogue);
	CHECK(stage.Voices.empty());
	CHECK(stage.Starts == 1);
	CHECK(store.Resource<weather::WeatherListener>()->Thunder.Pending.empty());
	CHECK(RenderBlock(mixer).Peak() == 0);
}

TEST_CASE(
	"future thunder rebuilds its exact sample deadline after clock correction",
	"[weather][audio][thunder-history]"
) {
	ecs::Store store("weather.audio.corrected");
	const auto provider = Storm(store);
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	const auto oldPlayer = stage.Voices.front().Player;
	const auto oldDeadline = stage.Voices.front().StartSample;
	REQUIRE(RenderBlock(mixer).Peak() == 0);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Tick = 1;
	clock.Fraction = 0;
	clock.ElapsedFractionQ32 = 214748364;
	clock.ElapsedRemainder = 16;
	store.Set(provider, clock);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	CHECK(stage.Voices.front().Player != oldPlayer);
	CHECK(stage.Voices.front().StartSample < oldDeadline);
	const auto arrival = store.Resource<weather::WeatherListener>()->Thunder.Pending.front();
	const auto remaining =
		weather::RemainingThunderSeconds(arrival, clock.ElapsedSeconds, clock.ElapsedFractionQ32, 0);
	REQUIRE(remaining);
	const uint64_t expected =
		mixer.Clock() + static_cast<uint64_t>(std::llround(*remaining * mixer.Format().SampleRate));
	CHECK(stage.Voices.front().StartSample == expected);
	CHECK(stage.Starts == 2);
	while (mixer.Clock() + 512 <= expected)
		CHECK(RenderBlock(mixer).Peak() == 0);
	const uint64_t before = mixer.Clock();
	const auto heard = RenderBlock(mixer);
	CHECK(mixer.Graph().Find(oldPlayer) == nullptr);
	REQUIRE(mixer.Graph().Find(stage.Voices.front().Player));
	CHECK(mixer.Graph().Find(stage.Voices.front().Player)->NotBeforeSample == expected);
	for (size_t frame = 0; frame < expected - before; ++frame)
		for (uint16_t channel = 0; channel < heard.Format().Channels; ++channel)
			CHECK(heard.Frame(frame)[channel] == 0);
	CHECK(heard.Peak() > 0);
	CHECK(stage.Clear(mixer));
	(void)RenderBlock(mixer);
}

TEST_CASE(
	"backward clock correction cancels future thunder until its emission is reached",
	"[weather][audio][thunder-history]"
) {
	ecs::Store store("weather.audio.corrected_back");
	const auto provider = Storm(store);
	auto events = *store.Get<weather::WeatherEvents>(provider);
	events.Events.front().ElapsedSeconds = 1;
	store.Set(provider, events);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Tick = 20;
	clock.ElapsedSeconds = 1;
	store.Set(provider, clock);
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	const auto oldDeadline = stage.Voices.front().StartSample;
	CHECK(RenderBlock(mixer).Peak() == 0);
	clock.Tick = 15;
	clock.ElapsedSeconds = 0;
	clock.ElapsedFractionQ32 = uint32_t{3} << 30;
	store.Set(provider, clock);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	CHECK(stage.Voices.empty());
	CHECK_FALSE(client::WeatherThunderDueSoon(store, {34, 0, 0}));
	REQUIRE(store.Resource<weather::WeatherListener>()->Thunder.Pending.size() == 1);
	while (mixer.Clock() <= oldDeadline + 512)
		CHECK(RenderBlock(mixer).Peak() == 0);
	clock.Tick = 20;
	clock.ElapsedSeconds = 1;
	clock.ElapsedFractionQ32 = 0;
	store.Set(provider, clock);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	CHECK(stage.Voices.front().StartSample > mixer.Clock());
	CHECK(stage.Starts == 2);
	CHECK(stage.Clear(mixer));
	(void)RenderBlock(mixer);
}

TEST_CASE(
	"clock correction past waveform expiry consumes a scheduled thunder event",
	"[weather][audio][thunder-history]"
) {
	ecs::Store store("weather.audio.corrected_expired");
	const auto provider = Storm(store);
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	const auto oldDeadline = stage.Voices.front().StartSample;
	CHECK(RenderBlock(mixer).Peak() == 0);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Tick = 80;
	clock.ElapsedSeconds = 4;
	store.Set(provider, clock);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	CHECK(stage.Voices.empty());
	CHECK(store.Resource<weather::WeatherListener>()->Thunder.Pending.empty());
	CHECK_FALSE(client::WeatherThunderDueSoon(store, {34, 0, 0}));
	while (mixer.Clock() <= oldDeadline + 512)
		CHECK(RenderBlock(mixer).Peak() == 0);
	clock.Tick = 0;
	clock.ElapsedSeconds = 0;
	store.Set(provider, clock);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	CHECK(stage.Starts == 1);
	CHECK(stage.Voices.empty());
}

TEST_CASE(
	"partial thunder start requires all six mixer commands before accepting playback",
	"[weather][audio][thunder-history]"
) {
	ecs::Store store("weather.audio.six_commands");
	const auto provider = Storm(store);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Tick = 10;
	clock.ElapsedFractionQ32 = uint32_t{1} << 31;
	store.Set(provider, clock);
	audio::AudioMixer mixer;
	while (mixer.Commands().Free() > 5)
		REQUIRE(mixer.Commands().Post(audio::Command{}));
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {}, catalogue);
	CHECK(stage.Starts == 0);
	CHECK(stage.Voices.empty());
	CHECK(stage.Refused == 1);
	REQUIRE(store.Resource<weather::WeatherListener>()->Thunder.Pending.size() == 1);
	CHECK(RenderBlock(mixer).Peak() == 0);
	stage.Sync(store, mixer, {}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	CHECK(RenderBlock(mixer).Peak() > 0);
	CHECK(stage.Clear(mixer));
	(void)RenderBlock(mixer);
}

TEST_CASE(
	"refused future thunder cancellation retries after its old deadline passes",
	"[weather][audio][thunder-history]"
) {
	ecs::Store store("weather.audio.correction_retry");
	const auto provider = Storm(store);
	audio::AudioMixer mixer;
	client::WeatherSoundStage stage;
	client::SoundCatalogue catalogue;
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	const auto oldPlayer = stage.Voices.front().Player;
	const auto oldDeadline = stage.Voices.front().StartSample;
	CHECK(RenderBlock(mixer).Peak() == 0);
	while (mixer.Commands().Free() > 1)
		REQUIRE(mixer.Commands().Post(audio::Command{}));
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.Tick = 1;
	clock.ElapsedFractionQ32 = 214748364;
	clock.ElapsedRemainder = 16;
	store.Set(provider, clock);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	CHECK(stage.Refused == 1);
	REQUIRE(stage.Voices.size() == 1);
	CHECK(stage.Voices.front().Player == oldPlayer);
	REQUIRE(store.Resource<weather::WeatherListener>()->Thunder.Pending.size() == 1);
	while (mixer.Clock() <= oldDeadline)
		(void)RenderBlock(mixer);
	stage.Sync(store, mixer, {34, 0, 0}, catalogue);
	REQUIRE(stage.Voices.size() == 1);
	CHECK(stage.Voices.front().Player != oldPlayer);
	CHECK(stage.Voices.front().StartSample > mixer.Clock());
	CHECK(stage.Starts == 2);
	CHECK(RenderBlock(mixer).Peak() == 0);
	CHECK(mixer.Graph().Find(oldPlayer) == nullptr);
	CHECK(stage.Clear(mixer));
	(void)RenderBlock(mixer);
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
