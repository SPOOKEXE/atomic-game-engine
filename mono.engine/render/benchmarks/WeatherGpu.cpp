#include "WeatherGpu.hpp"

#include "RayConsumerProfile.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

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
	constexpr uint32_t DEFAULT_PHASE_FRAMES = 256;
	constexpr std::array<const char *, 7> WEATHER_PASSES{
		"weather-clock",
		"weather-field",
		"weather-summary",
		"weather-integrate",
		"weather-history",
		"weather-reconstruct",
		"weather-shadow-map"
	};
	constexpr std::array<const char *, 25> WEATHER_WORK{
		"field_dispatches",	   "summary_dispatches",	 "ray_dispatches",		 "shadow_dispatches",
		"temporal_dispatches", "reconstruct_dispatches", "generated_bytes",		 "page_upload_bytes",
		"history_copy_bytes",  "history_samples",		 "history_resets",		 "field_cache_hits",
		"summary_cache_hits",  "shadow_cache_hits",		 "residency_cache_hits", "generated_cells",
		"ray_pixels",		   "shadow_texels",			 "page_uploads",		 "resident_bricks",
		"clock_dispatches",	   "clock_anchors",			 "clock_advances",		 "clock_cache_hits",
		"clock_uniform_bytes"
	};

	uint32_t BenchmarkExtent(const char *name, uint32_t fallback, uint32_t maximum) {
		const char *configured = std::getenv(name);
		if (!configured) return fallback;
		const std::string_view text(configured);
		uint32_t extent = 0;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), extent, 10);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || extent == 0 ||
			extent > maximum)
			throw std::runtime_error(
				std::string(name) + " must be a decimal integer in 1.." + std::to_string(maximum)
			);
		return extent;
	}

	uint64_t BenchmarkTick(const char *name, uint64_t fallback, uint32_t phaseFrames) {
		const char *configured = std::getenv(name);
		if (!configured) return fallback;
		const std::string_view text(configured);
		uint64_t tick = 0;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), tick, 10);
		const uint64_t maximum = std::numeric_limits<uint64_t>::max() - phaseFrames;
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || tick > maximum)
			throw std::runtime_error(
				std::string(name) + " must be a decimal uint64 no greater than " + std::to_string(maximum) +
				" so the advancing phase cannot overflow"
			);
		return tick;
	}

	struct PassTiming {
		double Microseconds = 0;
		uint32_t Samples = 0;
		std::vector<uint64_t> Nanoseconds;
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
		core::HeapTotals CpuBefore, CpuAfter;
		std::vector<uint64_t> RecordingSamples, CompletionSamples;
		std::chrono::steady_clock::time_point LastStarted;
	};

	std::array<double, WEATHER_WORK.size()> ReadWeatherWork() {
		std::array<double, WEATHER_WORK.size()> values{};
		for (size_t index = 0; index < WEATHER_WORK.size(); ++index) {
			const auto counter = core::Metrics::Get(std::string("render.weather.") + WEATHER_WORK[index]);
			if (counter) values[index] = counter->Value;
		}
		return values;
	}

	double PhaseWork(const WeatherPhase &phase, std::string_view name) {
		for (size_t index = 0; index < WEATHER_WORK.size(); ++index)
			if (name == WEATHER_WORK[index]) return phase.After[index] - phase.Before[index];
		return 0;
	}

	bool ValidateWeatherPhases(const std::array<WeatherPhase, 3> &phases, bool inactive) {
		bool valid = true;
		const auto expect = [&](const WeatherPhase &phase, std::string_view counter, bool positive) {
			const double observed = PhaseWork(phase, counter);
			if (positive ? observed > 0 : observed == 0) return;
			std::cerr << "weather-gpu workload refused: phase=" << phase.Name << " counter=" << counter
					  << " expected=" << (positive ? "positive" : "zero") << " observed=" << observed << '\n';
			valid = false;
		};
		if (inactive) {
			for (const auto &phase : phases)
				for (const char *counter :
					 {"clock_dispatches",
					  "field_dispatches",
					  "summary_dispatches",
					  "ray_dispatches",
					  "shadow_dispatches"})
					expect(phase, counter, false);
			return valid;
		}
		// Work counters remain required even when completed device timestamps are unavailable.
		for (const auto *counter :
			 {"clock_dispatches",
			  "clock_anchors",
			  "field_dispatches",
			  "summary_dispatches",
			  "ray_dispatches",
			  "shadow_dispatches"})
			expect(phases[0], counter, true);
		expect(phases[1], "field_dispatches", false);
		expect(phases[1], "clock_dispatches", false);
		expect(phases[1], "clock_cache_hits", true);
		expect(phases[1], "field_cache_hits", true);
		expect(phases[1], "ray_dispatches", true);
		for (const auto *counter :
			 {"clock_dispatches",
			  "clock_advances",
			  "field_dispatches",
			  "summary_dispatches",
			  "ray_dispatches"})
			expect(phases[2], counter, true);
		return valid;
	}

	void PrintPhase(const WeatherPhase &phase, std::ostream &out) {
		out << "weather-gpu phase=" << phase.Name << " frames=" << phase.Frames
			<< " cpu_record_ns=" << phase.RecordingNanoseconds / std::max(phase.Frames, 1u)
			<< " submitted_compute_dispatches=" << phase.SubmittedComputeDispatches;
		for (size_t index = 0; index < WEATHER_PASSES.size(); ++index) {
			const auto &timing = phase.Timings[index];
			out << " gpu_" << WEATHER_PASSES[index] << "_us=";
			if (timing.Samples)
				out << timing.Microseconds / timing.Samples;
			else
				out << "unavailable";
			out << " gpu_" << WEATHER_PASSES[index] << "_samples=" << timing.Samples;
			ray_consumer_bench::PrintTails(WEATHER_PASSES[index], timing.Nanoseconds, out);
		}
		for (size_t index = 0; index < WEATHER_WORK.size(); ++index)
			out << ' ' << WEATHER_WORK[index] << "=" << phase.After[index] - phase.Before[index];
		out << " live_bytes=" << phase.FinalHeap.LiveBytes << " peak_bytes=" << phase.FinalHeap.PeakBytes
			<< " allocated_bytes=" << phase.FinalHeap.AllocatedBytes - phase.InitialHeap.AllocatedBytes
			<< " released_bytes=" << phase.FinalHeap.ReleasedBytes - phase.InitialHeap.ReleasedBytes
			<< " buffer_bytes=" << phase.FinalHeap.BufferBytes
			<< " transfer_bytes=" << phase.FinalHeap.TransferBufferBytes
			<< " texture_bytes=" << phase.FinalHeap.TextureBytes
			<< " live_buffers=" << phase.FinalHeap.Buffers
			<< " live_transfers=" << phase.FinalHeap.TransferBuffers
			<< " live_textures=" << phase.FinalHeap.Textures
			<< " created_buffers=" << phase.FinalHeap.BufferAllocations - phase.InitialHeap.BufferAllocations
			<< " created_transfers="
			<< phase.FinalHeap.TransferBufferAllocations - phase.InitialHeap.TransferBufferAllocations
			<< " created_textures="
			<< phase.FinalHeap.TextureAllocations - phase.InitialHeap.TextureAllocations
			<< " memory=logical_payload_not_driver_commitment gpu_sampling=renderer_schedule_actual_counts";
		ray_consumer_bench::PrintTails("recording", phase.RecordingSamples, out);
		ray_consumer_bench::PrintTails("completion", phase.CompletionSamples, out);
		out << " cpu_heap=" << (core::HeapProfile::IsCompiledIn() ? "available" : "unavailable");
		if (core::HeapProfile::IsCompiledIn())
			out << " cpu_allocated_bytes=" << phase.CpuAfter.TotalBytes - phase.CpuBefore.TotalBytes
				<< " cpu_allocated_blocks=" << phase.CpuAfter.TotalBlocks - phase.CpuBefore.TotalBlocks;
		out << '\n';
	}
}

BENCH("opt-in weather device scenario CPU harness", 1) {
	using namespace engine;
	// Device checks remain opt-in so ordinary CPU benchmark batches stay headless.
	const char *enabled = std::getenv("MONO_WEATHER_GPU_BENCH");
	if (!enabled || std::string_view(enabled) != "1") return;
	uint32_t width = 1920;
	uint32_t height = 1080;
	uint32_t shadowResolution = 256;
	uint64_t initialTick = 0;
	uint32_t phaseFrames = DEFAULT_PHASE_FRAMES;
	const auto scenario =
		ray_consumer_bench::Setting("MONO_WEATHER_GPU_BENCH_SCENE", std::string_view("simple"));
	if (scenario != "inactive" && scenario != "empty" && scenario != "simple" && scenario != "complex")
		throw std::runtime_error("unknown weather GPU benchmark scene");
	try {
		width = BenchmarkExtent("MONO_WEATHER_GPU_BENCH_WIDTH", width, 3840);
		height = BenchmarkExtent("MONO_WEATHER_GPU_BENCH_HEIGHT", height, 2160);
		shadowResolution =
			BenchmarkExtent("MONO_WEATHER_GPU_BENCH_SHADOW_RESOLUTION", shadowResolution, 2048);
		phaseFrames = BenchmarkExtent("MONO_WEATHER_GPU_BENCH_FRAMES", phaseFrames, 4096);
		initialTick = BenchmarkTick("MONO_WEATHER_GPU_BENCH_TICK", initialTick, phaseFrames);
		if (shadowResolution < 32 || !std::has_single_bit(shadowResolution))
			throw std::runtime_error(
				"MONO_WEATHER_GPU_BENCH_SHADOW_RESOLUTION must be a power of two in 32..2048"
			);
	} catch (const std::runtime_error &refusal) {
		std::cerr << "weather-gpu configuration refused: " << refusal.what() << '\n';
		std::exit(EXIT_FAILURE);
	}
	if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
	struct VideoLifetime {
		~VideoLifetime() {
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	} video;
	render::Renderer renderer;
	if (!renderer.Initialise(nullptr)) throw std::runtime_error(SDL_GetError());
	renderer.SetProfiling(render::ProfilingTier::Full);
	render::SceneTarget target{width, height};
	render::View view;
	view.World = 928;
	view.WorldName = core::Name("weather.benchmark.world");
	view.Target = &target;
	view.CameraFrame = core::CFrame::LookAt({0, 400, 800}, {0, 0, -400});
	view.Camera.FarPlane = 32768;
	view.Camera.MaxImageWidth = width;
	view.Camera.MaxImageHeight = height;
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
	std::vector<scene::DrawInstance> receivers{ground};
	if (scenario == "complex") {
		for (uint32_t index = 0; index < 64; ++index) {
			auto receiver = ground;
			receiver.Source = index + 2;
			receiver.HalfExtent = {24, 20 + float(index % 5) * 10, 24};
			receiver.Frame.Position = {
				float(index % 8) * 80 - 280, receiver.HalfExtent.Y, float(index / 8) * 80 - 280
			};
			receivers.push_back(receiver);
		}
	}
	if (scenario == "empty") {
		// A near occluder keeps an active resident field and shadow map while
		// every view ray ends below the cloud layer before it can integrate it.
		auto occluder = ground;
		occluder.Source = 1000;
		occluder.Frame = view.CameraFrame;
		occluder.Frame.Position = view.CameraFrame.PointToWorldSpace({0, 0, -.25f});
		occluder.HalfExtent = {100, 100, .01f};
		receivers.push_back(occluder);
	}
	view.Instances = receivers;
	auto &environment = view.Lighting.EnvironmentState;
	environment.HasClouds = true;
	environment.HasWeatherCompute = true;
	environment.HasWeatherClock = true;
	environment.Weather.Coverage = .8f;
	environment.Weather.VapourMixingRatio = .025f;
	environment.Weather.ShadowsEnabled = true;
	environment.Weather.ShadowResolution = shadowResolution;
	environment.Weather.ShadowCascades = 1;
	if (scenario == "complex") {
		environment.Weather.ShadowCascades = 4;
		environment.CloudVolume.Steps = 64;
	}
	environment.Weather.Enabled = scenario != "inactive";
	environment.Weather.ShadowDistanceMetres = 1024;
	environment.Clock.Tick = initialTick;
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
	for (auto &phase : phases) {
		phase.RecordingSamples.reserve(phaseFrames);
		phase.CompletionSamples.reserve(phaseFrames);
		for (auto &timing : phase.Timings)
			timing.Nanoseconds.reserve(phaseFrames);
	}
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
			previousSubmission->Timings[index].Nanoseconds.push_back(uint64_t(found->second * 1000));
		}
	};
	const auto submit = [&](WeatherPhase *phase) {
		const auto start = std::chrono::steady_clock::now();
		if (phase) phase->LastStarted = start;
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
			phase->RecordingSamples.push_back(uint64_t(nanoseconds));
			phase->SubmittedComputeDispatches += result.ComputeDispatches;
			++phase->Frames;
		}
		previousSubmission = phase;
	};
	phases[0].InitialHeap = renderer.MemoryStatistics();
	phases[0].Before = ReadWeatherWork();
	phases[0].CpuBefore = core::HeapProfile::Totals();
	submit(&phases[0]);
	if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
	phases[0].CompletionSamples.push_back(ray_consumer_bench::Elapsed(phases[0].LastStarted));
	phases[0].CpuAfter = core::HeapProfile::Totals();
	phases[0].FinalHeap = renderer.MemoryStatistics();
	phases[0].After = ReadWeatherWork();
	for (size_t phaseIndex = 1; phaseIndex < phases.size(); ++phaseIndex) {
		auto &phase = phases[phaseIndex];
		phase.InitialHeap = renderer.MemoryStatistics();
		phase.Before = ReadWeatherWork();
		phase.CpuBefore = core::HeapProfile::Totals();
		for (uint32_t frame = 0; frame < phaseFrames; ++frame) {
			if (phaseIndex == 2) ++environment.Clock.Tick;
			submit(&phase);
			if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
			phase.CompletionSamples.push_back(ray_consumer_bench::Elapsed(phase.LastStarted));
		}
		phase.FinalHeap = renderer.MemoryStatistics();
		phase.CpuAfter = core::HeapProfile::Totals();
		phase.After = ReadWeatherWork();
	}
	// The final render collects the last completed query. It is excluded from
	// all recording/work/memory phase totals, and its own query is not sampled.
	submit(nullptr);
	std::ostringstream cohort;
	const auto properties = SDL_GetGPUDeviceProperties(device);
	cohort << "weather-gpu backend=" << renderer.BackendName() << " device="
		   << std::quoted(SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unavailable"))
		   << " resolution=" << width << 'x' << height << " scenario=" << scenario
		   << " phase_frames=" << phaseFrames << " views=1 layers=1 opaque_receivers=" << receivers.size()
		   << " page_budget=" << environment.Weather.MaximumResidentBricks
		   << " lod=" << environment.Weather.MaximumLod
		   << " cell_size_metres=" << environment.Weather.CellSizeMetres
		   << " cloud_steps=" << environment.CloudVolume.Steps
		   << " tick_rate=" << environment.Weather.TickRate
		   << " shadow_resolution=" << environment.Weather.ShadowResolution
		   << " shadow_cascades=" << environment.Weather.ShadowCascades << " initial_tick=" << initialTick
		   << " field_dispatch_cells_per_generation=";
	const double fieldGenerations = PhaseWork(phases[0], "field_dispatches");
	if (fieldGenerations > 0)
		cohort << PhaseWork(phases[0], "generated_cells") / fieldGenerations;
	else
		cohort << "unavailable";
	cohort << " resident_pages_per_summary=";
	const double summaries = PhaseWork(phases[0], "summary_dispatches");
	if (summaries > 0)
		cohort << PhaseWork(phases[0], "resident_bricks") / summaries;
	else
		cohort << "unavailable";
	cohort << " completed_device_timestamps=" << renderer.Timed()
		   << " history_timing=publication_marker reconstruct_timing=temporal_upsample_history_copies\n";
	for (const auto &phase : phases)
		PrintPhase(phase, cohort);
	if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
	renderer.Shutdown();
	if (!ValidateWeatherPhases(phases, scenario == "inactive")) std::exit(EXIT_FAILURE);
	ray_consumer_bench::RetainCompletedReport("weather-gpu/" + std::string(scenario), cohort.str());
}
