#include "WeatherTemporal.hpp"

#include <engine/testing/Bench.hpp>

TEST_SUITE_ID("engine.render.bench.weather-temporal")

BENCH("weather temporal keys for 32 views with wrapped cloud motion", 1000) {
	engine::render::WeatherTemporalFrame previous;
	previous.Key.World = 4;
	previous.Key.Provider = 93;
	previous.Key.Width = 960;
	previous.Key.Height = 540;
	previous.Key.DisplacementQ8[0] = UINT32_MAX - 5;
	for (uint32_t batch = 0; batch < 1000; ++batch) {
		for (uint32_t view = 0; view < 32; ++view) {
			auto current = previous;
			current.Key.DisplacementQ8[0] += batch & 7;
			current.Eye.x = float(view);
			engine::testing::Consume(engine::render::WeatherTemporalCompatible(previous, current));
		}
	}
}
