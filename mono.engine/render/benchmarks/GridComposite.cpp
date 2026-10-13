// Opt-in real-device comparison of blank Studio grid composition. Device waits
// stay outside CPU recording; GPU results are accepted once per resolved sequence.

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.render.bench.grid-composite")

namespace {
	using namespace engine;
	using Clock = std::chrono::steady_clock;
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
		double GpuTotalMicroseconds = 0;
		double CpuRecordingMicroseconds = 0;
		double CpuTransparentMicroseconds = 0;
		Traffic Work;
		render::GpuMemoryStatistics WarmHeap;
		render::GpuMemoryStatistics FinalHeap;
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
		}
	}

	void Wait(SDL_GPUDevice *device) {
		if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
	}

	Report Measure(render::Renderer &renderer, uint32_t width, uint32_t height, bool reference) {
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
			if (renderer.DroppedProfileMarks() != 0)
				throw std::runtime_error("grid benchmark GPU timestamp marks dropped");
			return recording;
		};
		for (uint32_t frame = 0; frame < WARM_FRAMES; ++frame) {
			Wait(device);
			record();
		}
		Report report;
		report.Path = reference ? "copy_plus_grid" : "fused_grid";
		report.Backend = renderer.BackendName();
		report.Width = width;
		report.Height = height;
		report.Frames = FrameCount();
		report.WarmHeap = renderer.MemoryStatistics();
		const Traffic before = ReadTraffic();
		const core::Name transparent("transparent");
		uint64_t seenSequence = renderer.PassTimingSequence();
		for (uint32_t frame = 0; frame <= report.Frames; ++frame) {
			// One command is in flight. This wait makes the prior frame's query
			// available before Render collects it; no repeated snapshot is a sample.
			Wait(device);
			if (frame == report.Frames) {
				const Traffic after = ReadTraffic();
				report.Work.CopyBytes = after.CopyBytes - before.CopyBytes;
				report.Work.CopyCommands = after.CopyCommands - before.CopyCommands;
				report.Work.CompositeCommands = after.CompositeCommands - before.CompositeCommands;
				report.Work.RasterPasses = after.RasterPasses - before.RasterPasses;
				report.FinalHeap = renderer.MemoryStatistics();
			}
			const double recording = record();
			if (frame < report.Frames) {
				report.CpuRecordingMicroseconds += recording;
				const auto &wallTimes = renderer.PassWallTimes();
				const auto found = wallTimes.find(transparent.Id());
				if (found != wallTimes.end()) report.CpuTransparentMicroseconds += found->second;
			}
			const uint64_t sequence = renderer.PassTimingSequence();
			// The first collection still belongs to warm-up. The extra final
			// render collects the last measured query and is excluded from work/CPU.
			if (frame != 0 && sequence != seenSequence) {
				const auto &timings = renderer.PassTimings();
				const auto found = timings.find(transparent.Id());
				if (found != timings.end()) {
					report.GpuTotalMicroseconds += found->second;
					++report.GpuSamples;
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
		testing::Consume(report.GpuTotalMicroseconds);
		return report;
	}

	void Compare() {
		static const bool arrangedReport = [] {
			if (std::getenv("MONO_GRID_COMPOSITE_REPORT") != nullptr) std::atexit(PrintReports);
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
		if (graph::Build(graph::DefaultPbrDocument(), pipeline, offender) !=
				graph::PipelineDocumentStatus::Ok ||
			!renderer.SetPipeline(core::Name("grid-composite-bench"), pipeline))
			throw std::runtime_error("grid benchmark PBR pipeline unavailable");
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
