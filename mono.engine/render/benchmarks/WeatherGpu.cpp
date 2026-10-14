#include "WeatherGpu.hpp"

#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

TEST_SUITE_ID("engine.render.bench.weather-gpu")

BENCH("weather GPU descriptor and bounded full-field grid", 1000) {
	engine::scene::WeatherCompute controls;
	const auto compiled = engine::weather::Compile(controls);
	if (!compiled) return;
	const auto anchor = engine::weather::InitialAnchor(compiled->Definition);
	for (uint32_t iteration = 0; iteration < 1000; ++iteration) {
		const auto grid = engine::render::WeatherGridOf(controls);
		const auto time = engine::weather::ResolveTime(anchor, (uint64_t{1} << 60) + iteration);
		if (grid && time)
			engine::testing::Consume(
				engine::render::WeatherSignature(
					engine::render::WeatherUniformsOf(compiled->Definition, *time, *grid)
				)
			);
	}
}

BENCH("weather GPU sparse field and rays 96x54", 1) {
	using namespace engine;
	// Device checks remain opt-in so ordinary CPU benchmark batches stay headless.
	if (!std::getenv("MONO_WEATHER_GPU_BENCH")) return;
	if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
	struct VideoLifetime {
		~VideoLifetime() {
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	} video;
	render::Renderer renderer;
	if (!renderer.Initialise(nullptr)) throw std::runtime_error(SDL_GetError());
	renderer.SetProfiling(render::ProfilingTier::Full);
	render::SceneTarget target{96, 54};
	render::View view;
	view.World = 928;
	view.WorldName = core::Name("weather.benchmark.world");
	view.Target = &target;
	view.CameraFrame.Position = {0, 1800, 0};
	view.OverrideLighting = true;
	auto &environment = view.Lighting.EnvironmentState;
	environment.HasClouds = true;
	environment.HasWeatherCompute = true;
	environment.HasWeatherClock = true;
	environment.Weather.Coverage = .8f;
	environment.Weather.VapourMixingRatio = .025f;
	const auto compiled = weather::Compile(environment.Weather);
	if (!compiled) throw std::runtime_error("weather benchmark descriptor refused");
	environment.Clock.WindQ20 = compiled->Definition.WindQ20;
	render::OverlayImage overlay;
	const auto coldStart = std::chrono::steady_clock::now();
	const auto first = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	if (!first.Submitted) throw std::runtime_error("weather benchmark first submission refused");
	const auto coldNanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - coldStart)
			.count();
	auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
	uint64_t lastTiming = 0;
	uint32_t timedFrames = 0;
	uint32_t timedShadows = 0;
	double rayMicroseconds = 0;
	double shadowMicroseconds = 0;
	int64_t warmNanoseconds = 0;
	for (uint32_t frame = 0; frame < 16; ++frame) {
		if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
		// First half reuses the same field; second half advances accepted integral ticks.
		if (frame >= 8) environment.Clock.Tick++;
		const auto start = std::chrono::steady_clock::now();
		if (!renderer.Render(std::span(&view, 1), overlay, nullptr, false).Submitted)
			throw std::runtime_error("weather benchmark warm submission refused");
		warmNanoseconds +=
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
				.count();
		const auto sequence = renderer.PassTimingSequence();
		if (sequence == lastTiming) continue;
		lastTiming = sequence;
		const auto found = renderer.PassTimings().find(core::Name("weather-integrate").Id());
		if (found != renderer.PassTimings().end()) {
			rayMicroseconds += found->second;
			++timedFrames;
		}
		const auto shadow = renderer.PassTimings().find(core::Name("weather-shadow-map").Id());
		if (shadow != renderer.PassTimings().end()) {
			shadowMicroseconds += shadow->second;
			++timedShadows;
		}
	}
	const auto memory = renderer.MemoryStatistics();
	const auto properties = SDL_GetGPUDeviceProperties(device);
	std::cout << "weather-gpu backend=" << renderer.BackendName() << " device="
			  << SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unavailable")
			  << " resolution=96x54 views=1 layer=1 page_budget=" << environment.Weather.MaximumResidentBricks
			  << " lod=" << environment.Weather.MaximumLod << " cold_record_ns=" << coldNanoseconds
			  << " warm_record_ns=" << warmNanoseconds / 16 << " gpu_ray_us=";
	if (timedFrames)
		std::cout << rayMicroseconds / timedFrames;
	else
		std::cout << "unavailable";
	std::cout << " gpu_shadow_us=";
	if (timedShadows)
		std::cout << shadowMicroseconds / timedShadows;
	else
		std::cout << "unavailable";
	std::cout << " live_bytes=" << memory.LiveBytes << " peak_bytes=" << memory.PeakBytes
			  << " allocated_bytes=" << memory.AllocatedBytes << " released_bytes=" << memory.ReleasedBytes
			  << '\n';
	renderer.Shutdown();
}
