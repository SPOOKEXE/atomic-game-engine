// Release device measurements for the analytical storm particle field.

#include <engine/core/Metrics.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.render.bench.gpu-particle-field")

namespace {

	struct Report {
		uint32_t Count = 0;
		bool Admitted = false;
		bool TimestampAvailable = false;
		uint64_t CpuRecordingNanoseconds = 0;
		double GpuMicroseconds = 0.0;
		engine::render::GpuMemoryStatistics Before;
		engine::render::GpuMemoryStatistics Resident;
	};

	constexpr std::array<uint32_t, 5> PRESETS{262'144, 1'048'576, 4'194'304, 16'777'216, 50'000'000};
	std::array<std::optional<Report>, PRESETS.size()> Reports;

	uint64_t Delta(uint64_t after, uint64_t before) {
		return after - std::min(after, before);
	}

	void PrintReports() {
		for (const std::optional<Report> &stored : Reports) {
			if (!stored) continue;
			const Report &report = *stored;
			std::cout << "gpu-particle-field-report count=" << report.Count
					  << " admitted=" << report.Admitted << " cpu_record_ns=" << report.CpuRecordingNanoseconds
					  << " gpu_us=";
			if (report.TimestampAvailable) {
				std::cout << report.GpuMicroseconds;
			} else {
				std::cout << "unavailable";
			}
			std::cout << " buffer_live_bytes=" << Delta(report.Resident.BufferBytes, report.Before.BufferBytes)
					  << " live_bytes=" << Delta(report.Resident.LiveBytes, report.Before.LiveBytes)
					  << " peak_bytes=" << Delta(report.Resident.PeakBytes, report.Before.PeakBytes)
					  << " allocated_bytes=" << Delta(report.Resident.AllocatedBytes, report.Before.AllocatedBytes)
					  << " transfer_live_bytes="
					  << Delta(report.Resident.TransferBufferBytes, report.Before.TransferBufferBytes)
					  << " buffer_allocations="
					  << Delta(report.Resident.BufferAllocations, report.Before.BufferAllocations) << '\n';
		}
	}

	void ArrangeReport() {
		static const bool registered = [] {
			if (std::getenv("MONO_GPU_PARTICLE_FIELD_REPORT") != nullptr) std::atexit(PrintReports);
			return true;
		}();
		(void)registered;
	}

	engine::render::View FieldView(engine::render::SceneTarget &target, uint32_t count) {
		using namespace engine;
		render::View view;
		view.Target = &target;
		view.World = 104;
		view.WorldName = core::Name("gpu-particle-field-benchmark");
		view.CameraFrame = core::CFrame::LookAt({0.0f, 185.0f, 650.0f}, {0.0f, 185.0f, 0.0f});
		view.Camera.FarPlane = 4'000.0f;
		view.ParticleDelta = 1.0f / 60.0f;
		view.GpuParticles = render::GpuParticleFieldView{
			.Field = {.RequestedCount = count, .Seed = 0xC105D00D},
			.Storm = scene::EfPreset(scene::EfCategory::EF3),
			.Centre = {},
			.Seconds = 1.0f,
		};
		return view;
	}

	Report Measure(uint32_t count) {
		ArrangeReport();
		if (!SDL_Init(SDL_INIT_VIDEO)) {
			throw std::runtime_error(std::string("SDL video init failed: ") + SDL_GetError());
		}
		struct VideoQuit {
			~VideoQuit() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
		} videoQuit;

		Report report;
		report.Count = count;
		{
			engine::render::Renderer renderer;
			if (!renderer.Initialise(nullptr)) {
				throw std::runtime_error(std::string("headless renderer init failed: ") + SDL_GetError());
			}
			if (!renderer.Capabilities().HasCompute) {
				renderer.Shutdown();
				return report;
			}
			renderer.SetProfiling(engine::render::ProfilingTier::Full);
			report.Before = renderer.MemoryStatistics();
			engine::render::SceneTarget target{960, 540};
			engine::render::OverlayImage overlay;
			auto view = FieldView(target, count);
			const auto started = std::chrono::steady_clock::now();
			const engine::render::FrameResult first = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			report.CpuRecordingNanoseconds = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()
			);
			report.Resident = renderer.MemoryStatistics();
			report.Admitted = first.ParticlesDrawn >= count;

			auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
			if (report.Admitted && device != nullptr && SDL_WaitForGPUIdle(device)) {
				// The second recording collects the completed nonblocking timestamp
				// slot from the measured first frame.
				renderer.Render(std::span(&view, 1), overlay, nullptr, false);
				const auto found = renderer.PassTimings().find(engine::core::Name("gpu-particle-field").Id());
				if (found != renderer.PassTimings().end() && found->second > 0.0) {
					report.TimestampAvailable = true;
					report.GpuMicroseconds = found->second;
				}
			}
			engine::core::Metrics::Count("render.gpu_particle_field_probe.particle_count", count);
			engine::core::Metrics::Count(
				"render.gpu_particle_field_probe.buffer_live_bytes",
				Delta(report.Resident.BufferBytes, report.Before.BufferBytes)
			);
			if (report.TimestampAvailable) {
				engine::core::Metrics::CountTime(
					"render.gpu_particle_field_probe.gpu_work",
					static_cast<uint64_t>(report.GpuMicroseconds * 1'000.0)
				);
			}
			renderer.Shutdown();
		}
		return report;
	}

	void Store(size_t index) {
		Reports[index] = Measure(PRESETS[index]);
		engine::testing::Consume(Reports[index]->Resident.LiveBytes);
	}
}

BENCH("GPU particle field | 262k rows | 960x540", 1) {
	Store(0);
}

BENCH("GPU particle field | 1M rows | 960x540", 1) {
	Store(1);
}

BENCH("GPU particle field | 4M rows | 960x540", 1) {
	Store(2);
}

BENCH("GPU particle field | 16M rows | 960x540", 1) {
	Store(3);
}

BENCH("GPU particle field | 50M rows | 960x540", 1) {
	Store(4);
}
