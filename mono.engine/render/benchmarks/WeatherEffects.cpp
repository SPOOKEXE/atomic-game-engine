#include <engine/ecs/Store.hpp>
#include <engine/render/WeatherEffects.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/weather/Runtime.hpp>

#include <vector>

TEST_SUITE_ID("engine.render.bench.weather-effects")

namespace {
	struct Fixture {
		engine::ecs::Store Store{"weather.effects.bench"};
		std::vector<engine::scene::DrawInstance> Geometry;
		engine::render::View View;
		engine::render::WeatherEffectsFrame Frame;
		engine::effects::RibbonBuffer Ribbons;
		std::vector<engine::render::SceneLight> Lights;

		Fixture() : Geometry(10000) {
			using namespace engine;
			scene::RegisterSceneClasses();
			weather::RegisterWeatherRuntimeComponents();
			const auto provider = Store.Create("Weather");
			scene::WeatherCompute controls;
			controls.PrecipitationRate = 8;
			Store.Set(provider, controls);
			Store.Set(provider, scene::WeatherClock{});
			View.World = Store.Identity();
			View.CameraFrame.Position = {0, 2, 0};
			auto &environment = View.Lighting.EnvironmentState;
			environment.HasWeatherCompute = true;
			environment.HasWeatherClock = true;
			environment.CloudProvider = provider;
			environment.Weather = controls;
			for (size_t index = 0; index < Geometry.size(); ++index) {
				Geometry[index].Frame.Position = {float(index % 100) * 32, -2, float(index / 100) * 32};
				Geometry[index].HalfExtent = {8, 1, 8};
			}
			View.Instances = Geometry;
			render::CollectWeatherEffects(Store, View, Frame, Ribbons, Lights);
		}
	};

	Fixture &WeatherFixture() {
		static Fixture fixture;
		return fixture;
	}
}

BENCH("weather consumers | 10,000 bounds | unchanged emission", 64) {
	auto &fixture = WeatherFixture();
	for (uint32_t iteration = 0; iteration < 64; ++iteration) {
		engine::render::CollectWeatherEffects(
			fixture.Store, fixture.View, fixture.Frame, fixture.Ribbons, fixture.Lights
		);
		engine::testing::Consume(fixture.Frame.Signature);
	}
}

BENCH("weather consumers | 10,000 bounds | cold emission and bounded clipping", 4) {
	auto &fixture = WeatherFixture();
	for (uint32_t iteration = 0; iteration < 4; ++iteration) {
		fixture.Frame.HasLighting = false;
		engine::render::CollectWeatherEffects(
			fixture.Store, fixture.View, fixture.Frame, fixture.Ribbons, fixture.Lights
		);
		engine::testing::Consume(fixture.Frame.Signature);
	}
}
