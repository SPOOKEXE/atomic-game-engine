#include "WeatherGpu.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.render.bench.weather-gpu")

BENCH("weather descriptor CPU preparation and bounded full-field grid", 1000) {
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

namespace {
	using namespace engine;
	constexpr uint32_t PHASE_FRAMES = 8;
	constexpr std::array<const char *, 6> WEATHER_PASSES{
		"weather-field",
		"weather-summary",
		"weather-integrate",
		"weather-history",
		"weather-reconstruct",
		"weather-shadow-map"
	};
	constexpr std::array<const char *, 19> WEATHER_WORK{
		"field_dispatches",
		"summary_dispatches",
		"ray_dispatches",
		"shadow_dispatches",
		"temporal_dispatches",
		"reconstruct_dispatches",
		"generated_bytes",
		"page_upload_bytes",
		"history_copy_bytes",
		"history_samples",
		"history_resets",
		"field_cache_hits",
		"summary_cache_hits",
		"shadow_cache_hits",
		"residency_cache_hits",
		"generated_cells",
		"ray_pixels",
		"shadow_texels",
		"page_uploads"
	};

	struct PassTiming {
		double Microseconds = 0;
		uint32_t Samples = 0;
	};
	struct WeatherPhase {
		const char *Name = "cold";
		uint32_t Frames = 0;
		int64_t RecordingNanoseconds = 0;
		uint64_t SubmittedComputeDispatches = 0;
		std::array<PassTiming, WEATHER_PASSES.size()> Timings{};
		std::array<double, WEATHER_WORK.size()> Before{};
		std::array<double, WEATHER_WORK.size()> After{};
		render::GpuMemoryStatistics InitialHeap;
		render::GpuMemoryStatistics FinalHeap;
	};

	std::array<double, WEATHER_WORK.size()> ReadWeatherWork() {
		std::array<double, WEATHER_WORK.size()> values{};
		for (size_t index = 0; index < WEATHER_WORK.size(); ++index) {
			const auto counter = core::Metrics::Get(std::string("render.weather.") + WEATHER_WORK[index]);
			if (counter) values[index] = counter->Value;
		}
		return values;
	}

	void PrintPhase(const WeatherPhase &phase) {
		std::cout << "weather-gpu phase=" << phase.Name << " frames=" << phase.Frames
				  << " cpu_record_ns=" << phase.RecordingNanoseconds / std::max(phase.Frames, 1u)
				  << " submitted_compute_dispatches=" << phase.SubmittedComputeDispatches;
		for (size_t index = 0; index < WEATHER_PASSES.size(); ++index) {
			const auto &timing = phase.Timings[index];
			std::cout << " gpu_" << WEATHER_PASSES[index] << "_us=";
			if (timing.Samples)
				std::cout << timing.Microseconds / timing.Samples;
			else
				std::cout << "unavailable";
			std::cout << " gpu_" << WEATHER_PASSES[index] << "_samples=" << timing.Samples;
		}
		for (size_t index = 0; index < WEATHER_WORK.size(); ++index)
			std::cout << ' ' << WEATHER_WORK[index] << "=" << phase.After[index] - phase.Before[index];
		std::cout << " live_bytes=" << phase.FinalHeap.LiveBytes
				  << " peak_bytes=" << phase.FinalHeap.PeakBytes
				  << " allocated_bytes=" << phase.FinalHeap.AllocatedBytes - phase.InitialHeap.AllocatedBytes
				  << " released_bytes=" << phase.FinalHeap.ReleasedBytes - phase.InitialHeap.ReleasedBytes
				  << " buffer_bytes=" << phase.FinalHeap.BufferBytes
				  << " transfer_bytes=" << phase.FinalHeap.TransferBufferBytes
				  << " texture_bytes=" << phase.FinalHeap.TextureBytes
				  << " live_buffers=" << phase.FinalHeap.Buffers
				  << " live_transfers=" << phase.FinalHeap.TransferBuffers
				  << " live_textures=" << phase.FinalHeap.Textures << " created_buffers="
				  << phase.FinalHeap.BufferAllocations - phase.InitialHeap.BufferAllocations
				  << " created_transfers="
				  << phase.FinalHeap.TransferBufferAllocations - phase.InitialHeap.TransferBufferAllocations
				  << " created_textures="
				  << phase.FinalHeap.TextureAllocations - phase.InitialHeap.TextureAllocations
				  << " memory=logical_payload_not_driver_commitment\n";
	}
}

BENCH("opt-in weather device scenario CPU harness 96x54", 1) {
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
	view.CameraFrame = core::CFrame::LookAt({0, 400, 800}, {0, 0, -400});
	view.Camera.FarPlane = 32768;
	view.OverrideLighting = true;
	view.Lighting.Direction = {-.3f, -1, -.2f};
	view.Lighting.Direct = {1, .95f, .9f};
	view.Lighting.Ambient = {.1f, .13f, .16f};
	view.Lighting.OutdoorAmbient = {.2f, .25f, .3f};
	scene::DrawInstance ground;
	ground.Source = 1;
	ground.Frame.Position = {0, -2, 0};
	ground.HalfExtent = {4096, 2, 4096};
	ground.Tint = {.4f, .5f, .4f};
	ground.CastShadow = false;
	view.Instances = std::span(&ground, 1);
	auto &environment = view.Lighting.EnvironmentState;
	environment.HasClouds = true;
	environment.HasWeatherCompute = true;
	environment.HasWeatherClock = true;
	environment.Weather.Coverage = .8f;
	environment.Weather.VapourMixingRatio = .025f;
	environment.Weather.ShadowsEnabled = true;
	environment.Weather.ShadowResolution = 32;
	environment.Weather.ShadowCascades = 1;
	environment.Weather.ShadowDistanceMetres = 1024;
	environment.CloudLayer.Cover = 1;
	environment.CloudLayer.Density = 1;
	const auto compiled = weather::Compile(environment.Weather);
	if (!compiled) throw std::runtime_error("weather benchmark descriptor refused");
	environment.Clock.WindQ20 = compiled->Definition.WindQ20;
	render::OverlayImage overlay;
	auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
	std::array<WeatherPhase, 3> phases;
	phases[1].Name = "same_tick_reuse";
	phases[2].Name = "advancing_ticks";
	uint64_t lastTiming = renderer.PassTimingSequence();
	WeatherPhase *previousSubmission = nullptr;
	const auto collect = [&] {
		const uint64_t sequence = renderer.PassTimingSequence();
		if (!previousSubmission || sequence == lastTiming) return;
		lastTiming = sequence;
		const auto &completed = renderer.PassTimings();
		for (size_t index = 0; index < WEATHER_PASSES.size(); ++index) {
			const auto found = completed.find(core::Name(WEATHER_PASSES[index]).Id());
			if (found == completed.end()) continue;
			previousSubmission->Timings[index].Microseconds += found->second;
			++previousSubmission->Timings[index].Samples;
		}
	};
	const auto submit = [&](WeatherPhase *phase) {
		const auto start = std::chrono::steady_clock::now();
		const auto result = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		const auto nanoseconds =
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
				.count();
		if (!result.Submitted) throw std::runtime_error("weather benchmark submission refused");
		if (renderer.DroppedProfileMarks())
			throw std::runtime_error("weather benchmark GPU timestamp marks dropped");
		collect();
		if (phase) {
			phase->RecordingNanoseconds += nanoseconds;
			phase->SubmittedComputeDispatches += result.ComputeDispatches;
			++phase->Frames;
		}
		previousSubmission = phase;
	};
	phases[0].InitialHeap = renderer.MemoryStatistics();
	phases[0].Before = ReadWeatherWork();
	submit(&phases[0]);
	if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
	phases[0].FinalHeap = renderer.MemoryStatistics();
	phases[0].After = ReadWeatherWork();
	for (size_t phaseIndex = 1; phaseIndex < phases.size(); ++phaseIndex) {
		auto &phase = phases[phaseIndex];
		phase.InitialHeap = renderer.MemoryStatistics();
		phase.Before = ReadWeatherWork();
		for (uint32_t frame = 0; frame < PHASE_FRAMES; ++frame) {
			if (phaseIndex == 2) ++environment.Clock.Tick;
			submit(&phase);
			if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
		}
		phase.FinalHeap = renderer.MemoryStatistics();
		phase.After = ReadWeatherWork();
	}
	// The final render collects the last completed query. It is excluded from
	// all recording/work/memory phase totals, and its own query is not sampled.
	submit(nullptr);
	const auto properties = SDL_GetGPUDeviceProperties(device);
	std::cout << "weather-gpu backend=" << renderer.BackendName() << " device="
			  << std::quoted(
					 SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unavailable")
				 )
			  << " resolution=96x54 views=1 layer=1 opaque_receivers=1 page_budget="
			  << environment.Weather.MaximumResidentBricks << " lod=" << environment.Weather.MaximumLod
			  << " shadow_resolution=32 shadow_cascades=1"
			  << " completed_device_timestamps=" << renderer.Timed()
			  << " history_timing=publication_marker reconstruct_timing=temporal_upsample_history_copies\n";
	for (const auto &phase : phases)
		PrintPhase(phase);
	if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
	renderer.Shutdown();
}
