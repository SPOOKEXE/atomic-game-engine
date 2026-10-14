#include "WeatherAudio.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Runtime.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.weather-audio")
TEST_DEPENDS("client.weather-sounds")

using namespace engine;

namespace {
	ecs::Entity Provider(ecs::Store &store) {
		scene::RegisterSceneClasses();
		weather::RegisterWeatherRuntimeComponents();
		const auto provider = store.Create("Weather");
		store.Set(provider, scene::WeatherCompute{});
		store.Set(provider, scene::WeatherClock{});
		return provider;
	}
}

TEST_CASE("quiet Studio weather never opens an audio device", "[weather][audio]") {
	ecs::Store store("weather.studio.quiet");
	(void)Provider(store);
	studio::WeatherAudio weatherAudio;
	weatherAudio.Sync(store, {});
	CHECK_FALSE(weatherAudio.Device);
	CHECK(weatherAudio.Stages.empty());
	CHECK(weatherAudio.Catalogue.Count() == 0);
}

TEST_CASE("Studio listener switches retire previous thunder bindings on a null device", "[weather][audio]") {
	ecs::Store first("weather.studio.first");
	const auto provider = Provider(first);
	weather::StormEvent event;
	event.Layer = weather::WeatherLayerName(first, provider);
	event.Id = {1, 1, 1};
	event.StartMetres = {0, 100, 0};
	event.EndMetres = {};
	first.Set(provider, weather::WeatherEvents{{event}, 0});
	studio::WeatherAudio weatherAudio;
	weatherAudio.Device = std::unique_ptr<audio::Device>(audio::OpenNullDevice({}));
	REQUIRE(weatherAudio.Device);
	weatherAudio.Sync(first, {});
	REQUIRE(weatherAudio.Stages.size() == 1);
	CHECK(weatherAudio.Stages.begin()->second.Starts == 1);
	const auto player = weatherAudio.Stages.begin()->second.Voices.front().Player;
	weatherAudio.Device->Mixer().ApplyPending();
	REQUIRE(weatherAudio.Device->Mixer().Graph().Find(player));
	ecs::Store second("weather.studio.second");
	(void)Provider(second);
	weatherAudio.Sync(second, {});
	weatherAudio.Device->Mixer().ApplyPending();
	CHECK_FALSE(weatherAudio.Device->Mixer().Graph().Find(player));
	CHECK(weatherAudio.Stages.contains(second.Identity()));
	weatherAudio.Silence();
	weatherAudio.Device->Mixer().ApplyPending();
	CHECK(weatherAudio.Stages.empty());
}
