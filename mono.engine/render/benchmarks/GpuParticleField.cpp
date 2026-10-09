// Device residency and real world-to-camera staging for generic GPU fields.

#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/VectorField.hpp>
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
	using namespace engine;
	using FieldClock = std::chrono::steady_clock;
	constexpr uint32_t STAGING_BATCHES = 16;
	constexpr std::array<uint32_t, 5> PRESETS{262'144, 1'048'576, 4'194'304, 16'777'216, 50'000'000};

	struct Report {
		uint32_t Requested = 0;
		uint32_t ResidentRows = 0;
		uint32_t RasterInstances = 0;
		bool Admitted = false;
		bool TimestampAvailable = false;
		uint64_t CpuFirstRecordingNanoseconds = 0;
		uint64_t CpuWarmRecordingNanoseconds = 0;
		uint64_t CpuCollectNanoseconds = 0;
		uint64_t CpuBindNanoseconds = 0;
		uint32_t SourceFrames = 0;
		uint32_t WarmFrames = 0;
		uint64_t CollectBytes = 0;
		uint64_t BindBytes = 0;
		uint64_t PrepareBytes = 0;
		uint64_t UploadBytes = 0;
		uint64_t WarmUploadBytes = 0;
		double GpuMicroseconds = 0;
		render::GpuMemoryStatistics Before;
		render::GpuMemoryStatistics Resident;
	};
	struct StagingReport {
		uint32_t Samples = 0;
		uint32_t Cameras = 0;
		uint64_t CollectNanoseconds = 0;
		uint64_t BindNanoseconds = 0;
		uint64_t CollectBytes = 0;
		uint64_t BindBytes = 0;
		uint64_t AllocatedBytes = 0;
		uint64_t AllocatedBlocks = 0;
	};
	std::array<std::optional<Report>, PRESETS.size()> Reports;
	std::array<std::optional<StagingReport>, 4> StagingReports;

	uint64_t Delta(uint64_t after, uint64_t before) {
		return after - std::min(after, before);
	}
	uint64_t NanosecondsSince(FieldClock::time_point started) {
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(FieldClock::now() - started).count()
		);
	}
	uint64_t Counter(std::string_view name) {
		const auto value = core::Metrics::Get(name);
		return value ? static_cast<uint64_t>(value->Value) : 0;
	}
	struct SampleTraffic {
		uint64_t Collect = Counter("render.gpu_particle_field.sample_collect_bytes");
		uint64_t Bind = Counter("render.gpu_particle_field.sample_bind_bytes");
		uint64_t Prepare = Counter("render.gpu_particle_field.sample_prepare_bytes");
		uint64_t Upload = Counter("render.gpu_particle_field.sample_upload_bytes");
	};

	void PrintReports() {
		for (const auto &stored : StagingReports) {
			if (!stored) continue;
			const auto &report = *stored;
			std::cout << "gpu-field-staging-report samples=" << report.Samples
					  << " cameras=" << report.Cameras << " batches=" << STAGING_BATCHES
					  << " collect_ns_per_batch=" << report.CollectNanoseconds / STAGING_BATCHES
					  << " bind_ns_per_batch=" << report.BindNanoseconds / STAGING_BATCHES
					  << " collect_sample_bytes=" << report.CollectBytes
					  << " bind_sample_bytes=" << report.BindBytes
					  << " heap_hooks=" << core::HeapProfile::IsCompiledIn()
					  << " allocated_bytes=" << report.AllocatedBytes
					  << " allocated_blocks=" << report.AllocatedBlocks << '\n';
		}
		for (const auto &stored : Reports) {
			if (!stored) continue;
			const auto &report = *stored;
			std::cout << "gpu-particle-field-report requested=" << report.Requested
					  << " resident_rows=" << report.ResidentRows << " admitted=" << report.Admitted
					  << " raster_instances=" << report.RasterInstances
					  << " samples=" << scene::MAX_GPU_PARTICLE_SPAWN_SAMPLES << " cameras=1"
					  << " cpu_first_record_ns=" << report.CpuFirstRecordingNanoseconds
					  << " cpu_warm_record_ns="
					  << (report.WarmFrames == 0 ? 0 : report.CpuWarmRecordingNanoseconds / report.WarmFrames)
					  << " collect_ns_per_frame="
					  << (report.SourceFrames == 0 ? 0 : report.CpuCollectNanoseconds / report.SourceFrames)
					  << " bind_ns_per_frame="
					  << (report.SourceFrames == 0 ? 0 : report.CpuBindNanoseconds / report.SourceFrames)
					  << " source_frames=" << report.SourceFrames << " warm_frames=" << report.WarmFrames
					  << " collect_sample_bytes=" << report.CollectBytes
					  << " bind_sample_bytes=" << report.BindBytes
					  << " prepare_sample_bytes=" << report.PrepareBytes
					  << " upload_sample_bytes=" << report.UploadBytes
					  << " warm_upload_sample_bytes=" << report.WarmUploadBytes << " gpu_us=";
			if (report.TimestampAvailable)
				std::cout << report.GpuMicroseconds;
			else
				std::cout << "unavailable";
			std::cout << " buffer_live_bytes="
					  << Delta(report.Resident.BufferBytes, report.Before.BufferBytes)
					  << " live_bytes=" << Delta(report.Resident.LiveBytes, report.Before.LiveBytes)
					  << " peak_bytes=" << Delta(report.Resident.PeakBytes, report.Before.PeakBytes)
					  << " allocated_bytes="
					  << Delta(report.Resident.AllocatedBytes, report.Before.AllocatedBytes)
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

	struct FieldFixture {
		ecs::Store Store{"gpu-field-benchmark"};
		core::Name Owner{"gpu-field-benchmark"};
		render::WorldViewFrame Frame;
		render::WorldCameraFrame Camera;
		render::WorldViewBinding Binding;
		std::array<render::View, 8> Views;

		FieldFixture(uint32_t count, uint32_t samples) {
			scene::RegisterSceneClasses();
			const auto workspace = scene::InstallServices(Store);
			const auto flow =
				Store.CreateInstance(ecs::Classes::Find(core::Name("VectorField3D")), "GenericFlow");
			scene::VectorField3D wind;
			wind.Vector = {0, 12, 0};
			wind.Tangential = 30;
			wind.HalfExtent = {512, 512, 512};
			Store.Set(flow, wind);
			if (!Store.SetParent(flow, workspace))
				throw std::runtime_error("GPU field fixture flow parent refused");
			const auto instance =
				Store.CreateInstance(ecs::Classes::Find(core::Name("GpuParticleField")), "GenericField");
			if (!Store.SetParent(instance, flow))
				throw std::runtime_error("GPU field fixture parent refused");
			scene::GpuParticleField field;
			field.RequestedCount = count;
			for (uint32_t sample = 0; sample < samples; ++sample)
				field.SpawnSamples.push_back(
					{{static_cast<float>(sample % 32) * 4 - 62,
					  static_cast<float>((sample / 32) % 32) * 8 + 40,
					  static_cast<float>(sample / 1024) * 4},
					 15,
					 {0, 4, 0},
					 sample % 3}
				);
			Store.Set(instance, std::move(field));
			Binding.World = 104;
			Binding.Name = Owner;
			Binding.Identity = Store.Identity();
			for (auto &view : Views) {
				view.CameraFrame = core::CFrame::LookAt({0, 185, 650}, {0, 185, 0});
				view.Camera.FarPlane = 4000;
			}
		}
		void Collect() {
			Store.AdvanceTick(1.0f / 60.0f);
			render::CollectWorldView(Store, Owner, Frame);
		}
		void Bind(uint32_t cameras) {
			for (uint32_t camera = 0; camera < cameras; ++camera) {
				if (!render::BindWorldView(Frame, Camera, Binding, Views[camera]))
					throw std::runtime_error("GPU field fixture binding refused");
			}
		}
	};

	void MeasureStaging(size_t index, uint32_t samples, uint32_t cameras) {
		ArrangeReport();
		FieldFixture fixture(1'048'576, samples);
		fixture.Collect();
		fixture.Bind(cameras);
		const SampleTraffic before;
		const auto heapBefore = core::HeapProfile::Totals();
		StagingReport report;
		report.Samples = samples;
		report.Cameras = cameras;
		for (uint32_t batch = 0; batch < STAGING_BATCHES; ++batch) {
			const auto collect = FieldClock::now();
			fixture.Collect();
			report.CollectNanoseconds += NanosecondsSince(collect);
			const auto bind = FieldClock::now();
			fixture.Bind(cameras);
			report.BindNanoseconds += NanosecondsSince(bind);
			testing::Consume(fixture.Views[cameras - 1].GpuParticles->Field.SpawnSamples.back());
		}
		const auto heapAfter = core::HeapProfile::Totals();
		const SampleTraffic after;
		report.AllocatedBytes = Delta(heapAfter.TotalBytes, heapBefore.TotalBytes);
		report.AllocatedBlocks = Delta(heapAfter.TotalBlocks, heapBefore.TotalBlocks);
		report.CollectBytes = Delta(after.Collect, before.Collect);
		report.BindBytes = Delta(after.Bind, before.Bind);
		StagingReports[index] = report;
	}

	Report Measure(uint32_t count) {
		ArrangeReport();
		if (!SDL_Init(SDL_INIT_VIDEO))
			throw std::runtime_error(std::string("SDL video init failed: ") + SDL_GetError());
		struct VideoQuit {
			~VideoQuit() {
				SDL_QuitSubSystem(SDL_INIT_VIDEO);
			}
		} videoQuit;
		Report report;
		report.Requested = count;
		render::Renderer renderer;
		if (!renderer.Initialise(nullptr))
			throw std::runtime_error(std::string("headless renderer init failed: ") + SDL_GetError());
		if (!renderer.Capabilities().HasCompute) {
			renderer.Shutdown();
			return report;
		}
		renderer.SetProfiling(render::ProfilingTier::Full);
		report.Before = renderer.MemoryStatistics();
		render::SceneTarget target{960, 540};
		render::OverlayImage overlay;
		FieldFixture fixture(count, static_cast<uint32_t>(scene::MAX_GPU_PARTICLE_SPAWN_SAMPLES));
		fixture.Views[0].Target = &target;
		const SampleTraffic before;
		const auto stage = [&] {
			const auto collect = FieldClock::now();
			fixture.Collect();
			report.CpuCollectNanoseconds += NanosecondsSince(collect);
			const auto bind = FieldClock::now();
			fixture.Bind(1);
			report.CpuBindNanoseconds += NanosecondsSince(bind);
			++report.SourceFrames;
		};
		stage();
		const auto started = FieldClock::now();
		const auto first = renderer.Render(std::span(fixture.Views.data(), 1), overlay, nullptr, false);
		report.CpuFirstRecordingNanoseconds = NanosecondsSince(started);
		report.Resident = renderer.MemoryStatistics();
		report.ResidentRows = first.Particles;
		report.RasterInstances = first.ParticlesDrawn;
		// This fixture has no ordinary emitters: Particles is the actual admitted
		// field population, whereas ParticlesDrawn counts only the raster cohort.
		report.Admitted = first.Submitted && first.Particles == scene::NormalizeGpuParticleCount(count) &&
						  first.ComputeDispatches != 0;
		const uint64_t uploaded = Counter("render.gpu_particle_field.sample_upload_bytes");
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		static const core::Name GPU_PARTICLE_FIELD_NAME("gpu-particle-field");
		if (report.Admitted && device != nullptr) {
			// Wait outside the CPU recording measurement and collect completed
			// nonblocking timestamps in a bounded sample-and-drain window.
			for (uint32_t frame = 0; frame < 8; ++frame) {
				if (!SDL_WaitForGPUIdle(device)) break;
				stage();
				const auto warm = FieldClock::now();
				renderer.Render(std::span(fixture.Views.data(), 1), overlay, nullptr, false);
				report.CpuWarmRecordingNanoseconds += NanosecondsSince(warm);
				++report.WarmFrames;
				const auto found = renderer.PassTimings().find(GPU_PARTICLE_FIELD_NAME.Id());
				if (found != renderer.PassTimings().end() && found->second > 0) {
					report.TimestampAvailable = true;
					report.GpuMicroseconds = found->second;
				}
			}
		}
		const SampleTraffic after;
		report.CollectBytes = Delta(after.Collect, before.Collect);
		report.BindBytes = Delta(after.Bind, before.Bind);
		report.PrepareBytes = Delta(after.Prepare, before.Prepare);
		report.UploadBytes = Delta(after.Upload, before.Upload);
		report.WarmUploadBytes = Delta(after.Upload, uploaded);
		renderer.Shutdown();
		return report;
	}
	void Store(size_t index) {
		Reports[index] = Measure(PRESETS[index]);
		testing::Consume(Reports[index]->Resident.LiveBytes);
	}
}

BENCH("GPU field staging | 1024 samples | 1 camera", STAGING_BATCHES) {
	MeasureStaging(0, 1024, 1);
}
BENCH("GPU field staging | 1024 samples | 8 cameras", STAGING_BATCHES) {
	MeasureStaging(1, 1024, 8);
}
BENCH("GPU field staging | 8192 samples | 1 camera", STAGING_BATCHES) {
	MeasureStaging(2, 8192, 1);
}
BENCH("GPU field staging | 8192 samples | 8 cameras", STAGING_BATCHES) {
	MeasureStaging(3, 8192, 8);
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
