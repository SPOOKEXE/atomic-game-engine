// Opt-in real-device comparison of blank Studio grid composition. Device waits
// stay outside CPU recording; GPU results are accepted once per resolved sequence.

#include "GpuHeap.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.render.bench.grid-composite")

namespace {
	using namespace engine;
	using Clock = std::chrono::steady_clock;
	bool LatencyMode() {
		const char *mode = std::getenv("MONO_B0_LATENCY");
		if (!mode || std::string_view(mode) == "0") return false;
		if (std::string_view(mode) != "1") throw std::runtime_error("MONO_B0_LATENCY must be 0 or 1");
		const char *memory = std::getenv("MONO_BENCH_MEMORY");
		if (memory && std::string_view(memory) != "0")
			throw std::runtime_error("B0 latency requires MONO_BENCH_MEMORY=0");
		if (engine::core::HeapProfile::IsCompiledIn())
			throw std::runtime_error("B0 latency requires heap profiling compiled out");
		return true;
	}

	size_t LatencyOption(const char *name, size_t fallback, size_t minimum, size_t maximum) {
		const char *value = std::getenv(name);
		if (!value) return fallback;
		const std::string_view text(value);
		size_t result = 0;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result < minimum ||
			result > maximum)
			throw std::runtime_error(std::string(name) + " is outside its bounded integer range");
		return result;
	}

	template <class Value> void PrintTails(const char *name, std::vector<Value> samples) {
		if (samples.empty()) throw std::runtime_error("B0 latency cohort has no observations");
		std::sort(samples.begin(), samples.end());
		const auto rank = [&](size_t percent) { return samples[(samples.size() * percent + 99) / 100 - 1]; };
		std::cout << ' ' << name << "_samples=" << samples.size() << ' ' << name << "_p50=" << rank(50) << ' '
				  << name << "_p95=" << rank(95) << ' ' << name << "_p99=" << rank(99) << ' ' << name
				  << "_max=" << samples.back();
	}

	bool MemoryMode() {
		const char *setting = std::getenv("MONO_BENCH_MEMORY");
		if (!setting || std::string_view(setting) == "0") return false;
		if (std::string_view(setting) != "1") throw std::runtime_error("MONO_BENCH_MEMORY must be 0 or 1");
		if (!core::HeapProfile::IsCompiledIn())
			throw std::runtime_error("MONO_BENCH_MEMORY=1 requires compiled heap profiling");
		return true;
	}

	void PrintHeap(const core::HeapTotals &before, const core::HeapTotals &after) {
		std::cout << " tracked_cpp_requested_payload=1 malloc_gpu_excluded=1 timing_valid=0"
				  << " process_allocated_bytes=" << after.TotalBytes - before.TotalBytes
				  << " process_allocated_blocks=" << after.TotalBlocks - before.TotalBlocks
				  << " process_live_bytes_entry=" << before.LiveBytes
				  << " process_live_bytes_end=" << after.LiveBytes
				  << " process_live_bytes_delta=" << after.LiveBytes - before.LiveBytes
				  << " process_live_blocks_entry=" << before.LiveBlocks
				  << " process_live_blocks_end=" << after.LiveBlocks
				  << " process_live_blocks_delta=" << after.LiveBlocks - before.LiveBlocks
				  << " process_peak_lifetime_bytes_entry=" << before.PeakBytes
				  << " process_peak_lifetime_bytes_end=" << after.PeakBytes
				  << " peak_basis=process_lifetime_not_interval"
				  << " profiler_overhead_bytes_entry=" << before.OverheadBytes
				  << " profiler_overhead_bytes_end=" << after.OverheadBytes
				  << " dropped_scopes_delta=" << after.DroppedScopes - before.DroppedScopes
				  << " foreign_frees_delta=" << after.ForeignFrees - before.ForeignFrees << '\n';
	}

	constexpr uint32_t WARM_FRAMES = 16;

	class EmptySpatialHook final : public render::FrameOverlayHook {
	  public:
		bool Prepare(void *) override {
			return true;
		}
		bool AffectsScene() const override {
			return true;
		}
		void Record(void *, void *) override {}
	};

	uint32_t FrameCount() {
		const char *value = std::getenv("MONO_GRID_COMPOSITE_FRAMES");
		if (value == nullptr) return 120;
		const std::string_view text(value);
		uint32_t frames = 0;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), frames);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || frames < 4 ||
			frames > 10'000)
			throw std::runtime_error("MONO_GRID_COMPOSITE_FRAMES must be an integer in 4..10000");
		return frames;
	}

	uint64_t Counter(std::string_view name) {
		const auto value = core::Metrics::Get(name);
		return value ? static_cast<uint64_t>(value->Value) : 0;
	}

	struct Traffic {
		uint64_t CopyBytes = 0;
		uint64_t CopyCommands = 0;
		uint64_t CompositeCommands = 0;
		uint64_t RasterPasses = 0;
	};

	Traffic ReadTraffic() {
		return {
			Counter("render.identity_copy.bytes"),
			Counter("render.identity_copy.commands"),
			Counter("render.grid_composite.commands"),
			Counter("render.transparent.raster_passes"),
		};
	}

	struct Report {
		const char *Path = "";
		std::string Backend;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t Frames = 0;
		uint32_t GpuSamples = 0;
		uint32_t GpuObservationTarget = 0;
		double GpuTotalMicroseconds = 0;
		double CpuRecordingMicroseconds = 0;
		double CpuTransparentMicroseconds = 0;
		Traffic Work;
		render::GpuMemoryStatistics WarmHeap;
		render::GpuMemoryStatistics FinalHeap;
		bool HeapCaptured = false;
		core::HeapTotals HeapBefore, HeapAfter;
		std::vector<double> Recording, TransparentCpu, Completed, TransparentGpu;
	};

	std::array<Report, 4> LatestReports;
	uint32_t HarnessRuns = 0;

	void PrintReports() {
		const char *preset = std::getenv("MONO_GRID_COMPOSITE_PRESET");
		for (const auto &report : LatestReports) {
			if (report.Frames == 0) continue;
			std::cout << "grid-composite-report preset=" << (preset != nullptr ? preset : "unspecified")
					  << " backend=" << report.Backend << " scene=blank_pbr instances=0 grid=default_enabled"
					  << " camera=12,9,16_to_0,0,0 path=" << report.Path << " extent=" << report.Width << 'x'
					  << report.Height << " warm_frames=" << WARM_FRAMES << " frames=" << report.Frames
					  << " harness_runs=" << HarnessRuns << " harness_report=last_completed_run"
					  << " queue_depth_max=1 profiling=full waits_outside_cpu=1"
					  << " gpu_unique_samples=" << report.GpuSamples << " gpu_transparent_mean_us=";
			if (report.GpuSamples != 0)
				std::cout << report.GpuTotalMicroseconds / report.GpuSamples;
			else
				std::cout << "unavailable";
			std::cout << " cpu_record_mean_us=" << report.CpuRecordingMicroseconds / report.Frames
					  << " cpu_transparent_mean_us=" << report.CpuTransparentMicroseconds / report.Frames
					  << " identity_copy_commands=" << report.Work.CopyCommands
					  << " identity_copy_bytes=" << report.Work.CopyBytes
					  << " grid_composite_commands=" << report.Work.CompositeCommands
					  << " transparent_raster_passes=" << report.Work.RasterPasses
					  << " gpu_tracked_warm_live_bytes=" << report.WarmHeap.LiveBytes
					  << " gpu_tracked_final_live_bytes=" << report.FinalHeap.LiveBytes
					  << " gpu_tracked_final_texture_bytes=" << report.FinalHeap.TextureBytes
					  << " gpu_allocated_bytes="
					  << report.FinalHeap.AllocatedBytes - report.WarmHeap.AllocatedBytes
					  << " gpu_released_bytes="
					  << report.FinalHeap.ReleasedBytes - report.WarmHeap.ReleasedBytes
					  << " memory=logical_payload_not_driver_commitment" << '\n';
			if (report.HeapCaptured) {
				std::cout << "b0-grid-heap path=" << report.Path << " extent=" << report.Width << 'x'
						  << report.Height << " frames=" << report.Frames
						  << " boundary=post_warm_measured_loop_through_final_measured_completion "
							 "warmup_excluded=1 drain_render_excluded=1 readback_oracles_excluded=1 "
							 "measurement_bookkeeping_included=1 cohort=last_completed_body_invocation";
				PrintHeap(report.HeapBefore, report.HeapAfter);
			}
		}
	}

	void Wait(SDL_GPUDevice *device) {
		if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
	}

	Report Measure(
		render::Renderer &renderer,
		uint32_t width,
		uint32_t height,
		bool reference,
		std::span<const scene::DrawInstance> instances = {},
		bool latency = false
	) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		const render::SceneTarget target{width, height};
		render::View view;
		view.World = 329;
		view.WorldName = core::Name("grid-composite-bench-world");
		view.Pipeline = core::Name("grid-composite-bench");
		view.Target = &target;
		view.CameraFrame = core::CFrame::LookAt({12, 9, 16}, {0, 0, 0});
		view.Camera.NearPlane = .1f;
		view.Camera.FarPlane = 1000;
		view.Grid.Enabled = true;
		view.Instances = instances;
		render::OverlayImage overlay;
		EmptySpatialHook referenceHook;
		const auto record = [&] {
			core::FrameGraph::BeginFrame();
			const auto started = Clock::now();
			const auto result =
				renderer.Render(std::span(&view, 1), overlay, reference ? &referenceHook : nullptr, false);
			const double recording =
				std::chrono::duration<double, std::micro>(Clock::now() - started).count();
			core::FrameGraph::EndFrame();
			if (!result.Submitted) throw std::runtime_error("grid benchmark submission failed");
			if (!instances.empty() && result.DrawCalls == 0)
				throw std::runtime_error("populated grid benchmark submitted no opaque draws");
			if (renderer.DroppedProfileMarks() != 0)
				throw std::runtime_error("grid benchmark GPU timestamp marks dropped");
			return recording;
		};
		const auto warmups = latency ? LatencyOption("MONO_B0_WARMUPS", 16, 0, 128) : WARM_FRAMES;
		for (uint32_t frame = 0; frame < warmups; ++frame) {
			Wait(device);
			record();
		}
		Report report;
		report.Path = reference ? "copy_plus_grid" : "fused_grid";
		report.Backend = renderer.BackendName();
		report.Width = width;
		report.Height = height;
		if (latency) {
			report.GpuObservationTarget = static_cast<uint32_t>(LatencyOption("MONO_B0_TRIALS", 101, 1, 512));
			// RendererState samples ordinary GPU passes every fourth frame. The
			// exact observation check makes a future policy change fail visibly.
			report.Frames = report.GpuObservationTarget * 4;
		} else {
			report.Frames = FrameCount();
		}
		if (latency) {
			report.Recording.reserve(report.Frames);
			report.TransparentCpu.reserve(report.Frames);
			report.Completed.reserve(report.Frames);
			report.TransparentGpu.reserve(report.Frames);
		}
		report.WarmHeap = renderer.MemoryStatistics();
		const Traffic before = ReadTraffic();
		const core::Name transparent("transparent");
		uint64_t seenSequence = renderer.PassTimingSequence();
		report.HeapCaptured = !latency && MemoryMode();
		if (report.HeapCaptured) report.HeapBefore = core::HeapProfile::Totals();
		for (uint32_t frame = 0; frame <= report.Frames; ++frame) {
			// One command is in flight. This wait makes the prior frame's query
			// available before Render collects it; no repeated snapshot is a sample.
			Wait(device);
			if (frame == report.Frames) {
				if (report.HeapCaptured) report.HeapAfter = core::HeapProfile::Totals();
				const Traffic after = ReadTraffic();
				report.Work.CopyBytes = after.CopyBytes - before.CopyBytes;
				report.Work.CopyCommands = after.CopyCommands - before.CopyCommands;
				report.Work.CompositeCommands = after.CompositeCommands - before.CompositeCommands;
				report.Work.RasterPasses = after.RasterPasses - before.RasterPasses;
				report.FinalHeap = renderer.MemoryStatistics();
			}
			const auto began = latency ? Clock::now() : Clock::time_point{};
			const double recording = record();
			if (latency) Wait(device);
			const double completed =
				latency ? std::chrono::duration<double, std::micro>(Clock::now() - began).count() : 0;
			if (frame < report.Frames) {
				report.CpuRecordingMicroseconds += recording;
				const auto &wallTimes = renderer.PassWallTimes();
				const auto found = wallTimes.find(transparent.Id());
				if (found != wallTimes.end()) report.CpuTransparentMicroseconds += found->second;
				if (latency) {
					if (found == wallTimes.end() || !std::isfinite(found->second) || found->second <= 0 ||
						!std::isfinite(recording) || recording <= 0 || !std::isfinite(completed) ||
						completed <= 0)
						throw std::runtime_error("grid benchmark missing individual CPU span");
					report.Recording.push_back(recording);
					report.TransparentCpu.push_back(found->second);
					report.Completed.push_back(completed);
				}
			}
			const uint64_t sequence = renderer.PassTimingSequence();
			// The first collection still belongs to warm-up. The extra final
			// render collects the last measured query and is excluded from work/CPU.
			if (latency && frame != 0 && sequence != seenSequence && sequence != seenSequence + 1)
				throw std::runtime_error("grid benchmark skipped or regressed a device observation");
			if (frame != 0 && sequence != seenSequence) {
				const auto &timings = renderer.PassTimings();
				const auto found = timings.find(transparent.Id());
				if (found != timings.end()) {
					report.GpuTotalMicroseconds += found->second;
					++report.GpuSamples;
					if (latency) {
						if (!std::isfinite(found->second) || found->second <= 0)
							throw std::runtime_error("grid benchmark invalid device span");
						report.TransparentGpu.push_back(found->second);
					}
				}
			}
			seenSequence = sequence;
		}
		if (report.GpuSamples == 0)
			throw std::runtime_error("grid benchmark has no completed GPU transparent timestamps");
		if (!reference && report.Work.CompositeCommands != report.Frames)
			throw std::runtime_error(
				"grid benchmark did not exercise the fused path on every measured frame"
			);

		if (latency &&
			(report.GpuSamples != report.GpuObservationTarget || report.Work.RasterPasses != report.Frames ||
			 (reference && report.Work.CompositeCommands != 0)))
			throw std::runtime_error("grid benchmark incomplete device or traffic observations");
		testing::Consume(report.GpuTotalMicroseconds);
		return report;
	}

	std::vector<std::byte>
	Download(render::Renderer &renderer, core::Name resource, uint32_t width, uint32_t height) {
		const auto extent = renderer.ResourceTextureExtent(resource);
		if (extent.DrawnWidth != width || extent.DrawnHeight != height || width != 1920 || height != 1080)
			throw std::runtime_error("grid oracle source extent mismatch");
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(renderer.ResourceTexture(resource));
		if (!texture) throw std::runtime_error("grid oracle texture unavailable");
		const uint32_t pitch = (width * 4 + 255) / 256 * 256;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = pitch * height;
		const auto release = [device](SDL_GPUTransferBuffer *buffer) {
			render::gpu::ReleaseTransferBuffer(device, buffer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(release)> buffer(
			render::gpu::CreateTransferBuffer(device, &info), release
		);
		if (!buffer) throw std::runtime_error(SDL_GetError());
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		if (!command) throw std::runtime_error(SDL_GetError());
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) {
			SDL_CancelGPUCommandBuffer(command);
			throw std::runtime_error(SDL_GetError());
		}
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = width;
		source.h = height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = buffer.get();
		destination.pixels_per_row = pitch / 4;
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		if (!SDL_SubmitGPUCommandBuffer(command)) throw std::runtime_error(SDL_GetError());
		Wait(device);
		std::vector<std::byte> result(size_t(width) * height * 4);
		const auto *mapped =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, buffer.get(), false));
		if (!mapped) throw std::runtime_error(SDL_GetError());
		for (size_t row = 0; row < height; ++row)
			std::memcpy(result.data() + row * width * 4, mapped + row * pitch, width * 4);
		SDL_UnmapGPUTransferBuffer(device, buffer.get());
		return result;
	}

	void VerifyParity(render::Renderer &renderer, std::span<const scene::DrawInstance> instances) {
		const render::SceneTarget target{1920, 1080};
		render::View view;
		view.World = 329;
		view.WorldName = core::Name("grid-composite-bench-world");
		view.Pipeline = core::Name("grid-composite-oracle");
		view.Target = &target;
		view.CameraFrame = core::CFrame::LookAt({12, 9, 16}, {0, 0, 0});
		view.Camera.NearPlane = .1f;
		view.Camera.FarPlane = 1000;
		view.Grid.Enabled = true;
		view.Instances = instances;
		render::OverlayImage overlay;
		EmptySpatialHook hook;
		const auto draw = [&](bool reference) {
			const auto result =
				renderer.Render(std::span(&view, 1), overlay, reference ? &hook : nullptr, false);
			if (!result.Submitted || (!instances.empty() && result.DrawCalls == 0))
				throw std::runtime_error("grid parity draw oracle failed");
			Wait(static_cast<SDL_GPUDevice *>(renderer.Backend().Device));
		};
		draw(true);
		const auto referenceColour =
			Download(renderer, core::Name("tonemapped"), target.Width, target.Height);
		const auto referenceDepth = Download(renderer, core::Name("depth"), target.Width, target.Height);
		draw(false);
		const auto fusedColour = Download(renderer, core::Name("tonemapped"), target.Width, target.Height);
		const auto fusedDepth = Download(renderer, core::Name("depth"), target.Width, target.Height);
		if (fusedDepth != referenceDepth || fusedColour.size() != referenceColour.size())
			throw std::runtime_error("grid full depth or colour extent parity failed");
		for (size_t index = 0; index < fusedColour.size(); ++index)
			if (std::abs(
					std::to_integer<int>(fusedColour[index]) - std::to_integer<int>(referenceColour[index])
				) > 1)
				throw std::runtime_error("grid full colour parity exceeded one byte");
		size_t opaquePixels = 0;
		for (size_t index = 0; index < fusedDepth.size(); index += 4) {
			float depth = 0;
			std::memcpy(&depth, fusedDepth.data() + index, sizeof(depth));
			if (!std::isfinite(depth) || depth < 0 || depth > 1)
				throw std::runtime_error("grid depth oracle invalid value");
			opaquePixels += depth < 1;
		}
		if (!instances.empty() && opaquePixels == 0)
			throw std::runtime_error("populated grid oracle has no opaque depth");
		view.Grid.Strength = 0;
		draw(false);
		const auto absent = Download(renderer, core::Name("tonemapped"), target.Width, target.Height);
		if (absent == fusedColour) throw std::runtime_error("grid enabled oracle matched zero strength");
	}

	void CompareLatency(render::Renderer &renderer) {
		for (const size_t count : {size_t{0}, size_t{1}, size_t{128}, size_t{1024}}) {
			std::vector<scene::DrawInstance> instances(count);
			for (size_t index = 0; index < count; ++index) {
				auto &instance = instances[index];
				instance.Source = static_cast<uint32_t>(index + 1);
				instance.Frame.Position =
					count == 1 ? core::Vector3{0, .7f, 0}
							   : core::Vector3{
									 (float(index % 32) - 15.5f) * .6f, .5f, (float(index / 32) - 15.5f) * .6f
								 };
				instance.HalfExtent = {.22f, .5f, .22f};
				instance.Tint = {.2f, .7f, .3f};
				instance.CastShadow = false;
			}
			std::array<Report, 2> reports;
			for (size_t path = 0; path < reports.size(); ++path)
				reports[path] = Measure(renderer, 1920, 1080, path == 0, instances, true);
			VerifyParity(renderer, instances);
			const uint64_t expectedSavings = uint64_t{1920} * 1080 * 8 * reports[0].Frames;
			if (reports[0].Work.CopyBytes < reports[1].Work.CopyBytes ||
				reports[0].Work.CopyBytes - reports[1].Work.CopyBytes < expectedSavings)
				throw std::runtime_error("grid positive copy savings oracle failed");
			for (const auto &report : reports) {
				std::cout
					<< "b0-grid-latency instances=" << count << " path=" << report.Path
					<< " extent=1920x1080 backend=" << report.Backend
					<< " warmups=" << LatencyOption("MONO_B0_WARMUPS", 16, 0, 128)
					<< " harness_summary=not_individual_latency quantile=nearest_rank heap=compiled_out "
					   "quiet_host=unverified tail_valid="
					<< (report.GpuSamples >= 100 && report.Recording.size() >= 100)
					<< " requested_gpu_observations=" << report.GpuObservationTarget
					<< " actual_render_frames=" << report.Frames
					<< " actual_gpu_observations=" << report.GpuSamples
					<< " drain_render_frames=1 ordinary_gpu_sample_period_frames=4"
					<< " one_in_flight=1 timed_pipeline=default_pbr oracle_pipeline=separate_retention_graph "
					   "full_colour=1 colour_tolerance_bytes=1 "
					   "exact_depth=1 enabled_grid_positive=1 oracle_outside_clock=1"
					<< " identity_copy_bytes=" << report.Work.CopyBytes
					<< " identity_copy_commands=" << report.Work.CopyCommands
					<< " grid_composite_commands=" << report.Work.CompositeCommands
					<< " transparent_raster_passes=" << report.Work.RasterPasses << " gpu_allocated_bytes="
					<< report.FinalHeap.AllocatedBytes - report.WarmHeap.AllocatedBytes
					<< " gpu_released_bytes="
					<< report.FinalHeap.ReleasedBytes - report.WarmHeap.ReleasedBytes
					<< " gpu_live_bytes=" << report.FinalHeap.LiveBytes
					<< " gpu_memory=logical_payload_not_driver_commitment";
				PrintTails("render_cpu_us", report.Recording);
				PrintTails("transparent_cpu_us", report.TransparentCpu);
				PrintTails("render_completed_us", report.Completed);
				PrintTails("transparent_gpu_us", report.TransparentGpu);
				std::cout << '\n';
			}
		}
	}

	void Compare() {
		const bool latency = LatencyMode();
		const bool memory = MemoryMode();
		static const bool arrangedReport = [memory] {
			if (memory || std::getenv("MONO_GRID_COMPOSITE_REPORT") != nullptr) std::atexit(PrintReports);
			return true;
		}();
		(void)arrangedReport;
		if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
		struct VideoQuit {
			~VideoQuit() {
				SDL_QuitSubSystem(SDL_INIT_VIDEO);
			}
		} videoQuit;
		struct ProfileRestore {
			bool Enabled = core::FrameGraph::IsEnabled();
			~ProfileRestore() {
				core::FrameGraph::SetEnabled(Enabled);
			}
		} profileRestore;
		core::FrameGraph::SetEnabled(true);
		render::Renderer renderer;
		if (!renderer.Initialise(nullptr)) throw std::runtime_error("grid benchmark renderer unavailable");
		renderer.SetProfiling(render::ProfilingTier::Full);
		if (!renderer.Timed()) throw std::runtime_error("grid benchmark GPU timestamps unavailable");
		graph::RenderGraph pipeline;
		core::Name offender;
		auto document = graph::DefaultPbrDocument();
		if (latency) {
			const core::Name kind("b0-grid-capture-boundary");
			graph::NodeKindSpec boundary;
			boundary.Kind = kind;
			boundary.Scope = graph::NodeScope::Frame;
			boundary.Queue = graph::ExecutionQueue::Cpu;
			boundary.Category = graph::NodeCategory::Output;
			for (const char *name : {"tonemapped", "depth"})
				boundary.Inputs.push_back({.Name = core::Name(name), .Kind = graph::ResourceKind::Texture});
			if (!graph::RegisterNodeKind(std::move(boundary)) ||
				!renderer.InstallNodeHandler(kind, [](const graph::RunContext &) { return true; }))
				throw std::runtime_error("grid capture boundary unavailable");
			document.Record(
				{.Kind = graph::EditKind::AddNode,
				 .Name = kind,
				 .NodeKind = kind,
				 .Scope = graph::NodeScope::Frame}
			);
			for (const char *name : {"tonemapped", "depth"})
				document.Record(
					{.Kind = graph::EditKind::Reads, .Target = core::Name(name), .Key = core::Name(name)}
				);
			graph::RenderGraph oraclePipeline;
			if (graph::Build(document, oraclePipeline, offender) != graph::PipelineDocumentStatus::Ok ||
				!renderer.SetPipeline(core::Name("grid-composite-oracle"), oraclePipeline))
				throw std::runtime_error("grid oracle retention pipeline unavailable");
			document = graph::DefaultPbrDocument();
		}
		if (graph::Build(document, pipeline, offender) != graph::PipelineDocumentStatus::Ok ||
			!renderer.SetPipeline(core::Name("grid-composite-bench"), pipeline))
			throw std::runtime_error("grid benchmark PBR pipeline unavailable");
		if (latency) {
			CompareLatency(renderer);
			renderer.Shutdown();
			return;
		}
		const std::array extents{render::SceneTarget{640, 480}, render::SceneTarget{1600, 900}};
		++HarnessRuns;
		for (size_t index = 0; index < extents.size(); ++index) {
			// Alternate order across harness runs to expose neither path only cold.
			for (bool reference : {HarnessRuns % 2 == 0, HarnessRuns % 2 != 0})
				LatestReports[index * 2 + (reference ? 0 : 1)] =
					Measure(renderer, extents[index].Width, extents[index].Height, reference);
		}
		renderer.Shutdown();
	}
}

BENCH("Blank Studio grid | copy plus grid versus fused | two extents", 1) {
	Compare();
}
