#include "WeatherShadows.hpp"

#include "WeatherGpu.hpp"

#include <engine/testing/Bench.hpp>

TEST_SUITE_ID("engine.render.bench.weather-shadows")

BENCH("weather shadow texel-snapped four-cascade plan 32 views", 1000) {
	using namespace engine;
	scene::WeatherCompute controls;
	controls.ShadowCascades = 4;
	const auto compiled = weather::Compile(controls);
	const auto grid = render::WeatherGridOf(controls);
	if (!compiled || !grid) return;
	const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 100);
	if (!time) return;
	const render::WeatherGpuInputs inputs{
		render::WeatherUniformsOf(compiled->Definition, *time, *grid), *compiled, *time, false
	};
	std::array<render::WeatherResidencyView, 32> views{};
	for (uint32_t view = 0; view < views.size(); ++view)
		views[view].View.CameraPositionMetres = {float(view * 128), 100, float(view * 64)};
	for (uint32_t iteration = 0; iteration < 1000; ++iteration)
		testing::Consume(
			render::BuildWeatherShadowPlan(controls, inputs, {-.3f, -.8f, -.2f}, 1, 1, views).Signature
		);
}
