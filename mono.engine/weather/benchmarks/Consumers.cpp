#include <engine/testing/Bench.hpp>
#include <engine/weather/Consumers.hpp>

#include <stdexcept>

TEST_SUITE_ID("engine.weather.bench.consumers")

BENCH_PER_ITEM("weather consumer column · 16 shared reference cells", 4096) {
	engine::scene::WeatherCompute controls;
	controls.FogStrength = 1;
	controls.PrecipitationRate = 10;
	controls.IceFraction = 0.5f;
	controls.StormIntensity = 1;
	const auto compiled = engine::weather::Compile(controls, 14);
	if (!compiled) throw std::runtime_error("weather consumer benchmark definition refused");
	const auto anchor = engine::weather::InitialAnchor(compiled->Definition);
	for (uint64_t tick = 0; tick < 4096; ++tick) {
		const auto time = engine::weather::ResolveTime(anchor, tick);
		if (!time) throw std::runtime_error("weather consumer benchmark time refused");
		engine::testing::Consume(
			engine::weather::SampleConsumers(
				*compiled,
				*time,
				controls,
				{static_cast<float>(tick % 128) * 64, 2, static_cast<float>(tick % 97) * 64}
			)
		);
	}
}
