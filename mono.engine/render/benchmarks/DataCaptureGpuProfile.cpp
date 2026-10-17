// A repeatable, headless Vulkan capture profile for the v0.25 Stage 4 gate.

#include <engine/assets/Mesh.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.render.bench.data-capture-gpu-profile")

namespace {
	using Clock = std::chrono::steady_clock;
	using Nanoseconds = std::chrono::nanoseconds;
	using namespace engine;

	constexpr uint32_t WIDTH = 64;
	constexpr uint32_t HEIGHT = 64;
	constexpr size_t WARMUP_CAPTURES = 5;
	constexpr size_t DEFAULT_CAPTURES = 30;

	struct Sample {
		uint64_t CpuNanoseconds = 0;
		uint64_t ReadbackNanoseconds = 0;
		uint64_t RecordBytes = 0;
		uint64_t GpuNanoseconds = 0;
		uint64_t HostReservedBytes = 0;
		uint64_t DeviceReservedBytes = 0;
		uint64_t CpuAllocationBlocks = 0;
		uint32_t Polls = 0;
		bool HasGpuTiming = false;
		bool HasCpuAllocationCounts = false;
		std::string GpuTimingReason;
	};

	struct Profile {
		std::vector<Sample> Samples;
		size_t Drops = 0;
		render::GpuMemoryStatistics Before;
		render::GpuMemoryStatistics After;
	};

	uint64_t Elapsed(Clock::time_point start) {
		return static_cast<uint64_t>(std::chrono::duration_cast<Nanoseconds>(Clock::now() - start).count());
	}

	uint64_t Percentile(std::vector<uint64_t> values, size_t percentile) {
		if (values.empty()) return 0;
		std::ranges::sort(values);
		const size_t index = (percentile * (values.size() - 1) + 99) / 100;
		return values[index];
	}

	size_t CaptureCount() {
		const char *value = std::getenv("MONO_DATA_CAPTURE_PROFILE_CAPTURES");
		if (value == nullptr) return DEFAULT_CAPTURES;
		char *end = nullptr;
		const unsigned long parsed = std::strtoul(value, &end, 10);
		if (end == value || *end != '\0' || parsed == 0 || parsed > 10000)
			throw std::runtime_error("MONO_DATA_CAPTURE_PROFILE_CAPTURES must be in [1, 10000]");
		return static_cast<size_t>(parsed);
	}

	void CaptureOne(
		render::Renderer &renderer,
		const core::Name pipeline,
		render::SceneTarget &firstTarget,
		render::SceneTarget &captureTarget,
		render::OverlayImage &overlay,
		const scene::DrawInstance &instance,
		uint64_t captureIndex,
		Profile *profile
	) {
		const std::string snapshot = "data-capture-profile-" + std::to_string(captureIndex);
		render::View first;
		first.Pipeline = pipeline;
		first.Target = &firstTarget;
		first.Slot = 1;
		render::View capture;
		capture.Pipeline = pipeline;
		capture.Target = &captureTarget;
		capture.Slot = 2;
		capture.SnapshotId = snapshot;
		capture.Instances = std::span(&instance, 1);
		const std::array views{first, capture};
		render::DataCaptureTicket ticket;
		const core::HeapTotals cpuBefore =
			profile != nullptr ? core::HeapProfile::Totals() : core::HeapTotals{};
		const auto started = Clock::now();
		if (!renderer.QueueDataCapture(
				{.SnapshotId = snapshot,
				 .Pipeline = pipeline,
				 .CaptureNode = core::Name("data-capture"),
				 .ViewSlot = capture.Slot,
				 .Channels = {render::DataCaptureChannel::RgbLinearHdr},
				 .ObjectLabels = {},
				 .SemanticLabels = {},
				 .PartLabels = {},
				 .LocalLightIds = {}},
				ticket
			)) {
			if (profile != nullptr) ++profile->Drops;
			return;
		}
		const render::FrameResult frame = renderer.Render(views, overlay, nullptr, false);
		if (!frame.Submitted) {
			renderer.CancelDataCapture(ticket);
			if (profile != nullptr) ++profile->Drops;
			return;
		}
		const auto readbackStarted = Clock::now();
		render::DataCapturePoll poll;
		uint32_t polls = 0;
		const auto deadline = Clock::now() + std::chrono::seconds(5);
		do {
			poll = renderer.PollDataCapture(ticket);
			++polls;
			if (poll.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
		} while (poll.Status == render::DataCaptureStatus::Pending && Clock::now() < deadline);
		const uint64_t readbackNanoseconds = Elapsed(readbackStarted);
		const core::HeapTotals cpuAfter =
			profile != nullptr ? core::HeapProfile::Totals() : core::HeapTotals{};
		if (poll.Status != render::DataCaptureStatus::Ready &&
			poll.Status != render::DataCaptureStatus::Partial) {
			renderer.CancelDataCapture(ticket);
			if (profile != nullptr) ++profile->Drops;
			return;
		}
		if (profile == nullptr) return;
		Sample sample;
		sample.CpuNanoseconds = Elapsed(started);
		sample.ReadbackNanoseconds = readbackNanoseconds;
		sample.Polls = polls;
		sample.HasCpuAllocationCounts = cpuBefore.Nodes > 0 && cpuAfter.Nodes > 0;
		sample.CpuAllocationBlocks =
			cpuAfter.TotalBlocks - std::min(cpuAfter.TotalBlocks, cpuBefore.TotalBlocks);
		sample.HasGpuTiming = poll.GpuNanoseconds.has_value();
		sample.GpuNanoseconds = poll.GpuNanoseconds.value_or(0);
		sample.GpuTimingReason = poll.GpuTimingReason;
		sample.HostReservedBytes = poll.HostReadbackReservedCapacityBytes;
		sample.DeviceReservedBytes = poll.DeviceReadbackStagingReservedCapacityBytes;
		for (const render::DataCapturePlane &plane : poll.Planes) {
			sample.RecordBytes += plane.Bytes.size();
			if (plane.Status != render::DataCaptureStatus::Ready) ++profile->Drops;
		}
		profile->Samples.push_back(sample);
	}

	struct Fixture {
		render::Renderer Renderer;
		core::Name Pipeline{"data-capture-gpu-profile"};
		render::SceneTarget FirstTarget{WIDTH, HEIGHT};
		render::SceneTarget CaptureTarget{WIDTH, HEIGHT};
		render::OverlayImage Overlay;
		scene::DrawInstance Instance;
		Profile Report;
		uint64_t CaptureIndex = 0;

		Fixture() {
			if (!SDL_Init(SDL_INIT_VIDEO))
				throw std::runtime_error(std::string("SDL video init failed: ") + SDL_GetError());
			if (!Renderer.Initialise(nullptr))
				throw std::runtime_error(
					std::string("headless Vulkan renderer init failed: ") + SDL_GetError()
				);
			if (Renderer.BackendName() != "vulkan")
				throw std::runtime_error("profile backend was not Vulkan");
			graph::RenderGraph graph;
			core::Name offender;
			if (graph::Build(graph::DefaultPbrDataCaptureDocument(), graph, offender) !=
					graph::PipelineDocumentStatus::Ok ||
				!Renderer.SetPipeline(Pipeline, graph))
				throw std::runtime_error("data capture profile pipeline setup failed");
			assets::MeshData plane;
			plane.Vertices = {
				{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
				{{1, -1, 0}, {0, 0, 1}, {1, 1}},
				{{1, 1, 0}, {0, 0, 1}, {1, 0}},
				{{-1, 1, 0}, {0, 0, 1}, {0, 0}},
			};
			plane.Indices = {0, 1, 2, 0, 2, 3};
			plane.ComputeBounds();
			const core::Name mesh("data-capture-profile-plane");
			if (!Renderer.AddMesh(mesh, plane)) throw std::runtime_error("profile mesh upload failed");
			Instance.Source = 1;
			Instance.Mesh = mesh;
			Instance.Frame.Position = {0, 0, -3};
			Instance.HalfExtent = {1, 1, 1};
			Instance.EmissiveTint = {0.25f, 0.5f, 1.0f};
			Instance.EmissiveStrength = 4.0f;
			Instance.CastShadow = false;
			Renderer.SetProfiling(render::ProfilingTier::Off);
			for (size_t index = 0; index < WARMUP_CAPTURES; ++index)
				CaptureOne(
					Renderer, Pipeline, FirstTarget, CaptureTarget, Overlay, Instance, CaptureIndex++, nullptr
				);
			Report.Before = Renderer.MemoryStatistics();
		}

		~Fixture() {
			Renderer.Shutdown();
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}

		void Run(size_t count) {
			Report.Samples.reserve(Report.Samples.size() + count);
			for (size_t index = 0; index < count; ++index)
				CaptureOne(
					Renderer, Pipeline, FirstTarget, CaptureTarget, Overlay, Instance, CaptureIndex++, &Report
				);
			Report.After = Renderer.MemoryStatistics();
		}
	};

	Fixture *ActiveFixture = nullptr;

	void PrintProfile(const Profile &profile) {
		std::vector<uint64_t> cpu;
		std::vector<uint64_t> readback;
		std::vector<uint64_t> polls;
		std::vector<uint64_t> gpu;
		std::vector<uint64_t> cpuAllocations;
		cpu.reserve(profile.Samples.size());
		readback.reserve(profile.Samples.size());
		polls.reserve(profile.Samples.size());
		gpu.reserve(profile.Samples.size());
		cpuAllocations.reserve(profile.Samples.size());
		for (const Sample &sample : profile.Samples) {
			cpu.push_back(sample.CpuNanoseconds);
			readback.push_back(sample.ReadbackNanoseconds);
			polls.push_back(sample.Polls);
			if (sample.HasGpuTiming) gpu.push_back(sample.GpuNanoseconds);
			if (sample.HasCpuAllocationCounts) cpuAllocations.push_back(sample.CpuAllocationBlocks);
		}
		const bool gpuAvailable = !gpu.empty();
		const uint64_t recordBytes = profile.Samples.empty() ? 0 : profile.Samples.front().RecordBytes;
		std::string gpuTimingReason = "unavailable(no_completed_gpu_timestamp)";
		for (const Sample &sample : profile.Samples)
			if (!sample.HasGpuTiming && !sample.GpuTimingReason.empty()) {
				gpuTimingReason = "unavailable(" + sample.GpuTimingReason + ")";
				break;
			}
		std::cout << "data-capture-gpu-profile preset=bench scene=single_emissive_plane"
				  << " backend=vulkan resolution=" << WIDTH << 'x' << HEIGHT
				  << " channels=rgb_linear_hdr warmup=" << WARMUP_CAPTURES
				  << " captures=" << profile.Samples.size() << " queue_depth_max=1"
				  << " record_bytes_per_capture=" << recordBytes << " cpu_allocation_blocks_per_capture_p50=";
		if (cpuAllocations.empty()) {
			std::cout << "unavailable(heap profiler hooks disabled)";
		} else {
			std::cout << Percentile(cpuAllocations, 50);
		}
		std::cout << " cpu_allocation_blocks_per_capture_p99=";
		if (cpuAllocations.empty()) {
			std::cout << "unavailable";
		} else {
			std::cout << Percentile(cpuAllocations, 99);
		}
		std::cout << " cpu_allocation_blocks_per_capture_max="
				  << (cpuAllocations.empty() ? 0 : *std::ranges::max_element(cpuAllocations))
				  << " gpu_buffer_allocations="
				  << profile.After.BufferAllocations -
						 std::min(profile.After.BufferAllocations, profile.Before.BufferAllocations)
				  << " gpu_texture_allocations="
				  << profile.After.TextureAllocations -
						 std::min(profile.After.TextureAllocations, profile.Before.TextureAllocations)
				  << " gpu_transfer_allocations="
				  << profile.After.TransferBufferAllocations -
						 std::min(
							 profile.After.TransferBufferAllocations, profile.Before.TransferBufferAllocations
						 )
				  << " host_reserved_bytes_per_capture="
				  << (profile.Samples.empty() ? 0 : profile.Samples.front().HostReservedBytes)
				  << " device_staging_reserved_bytes_per_capture="
				  << (profile.Samples.empty() ? 0 : profile.Samples.front().DeviceReservedBytes)
				  << " cpu_roundtrip_ns_p50=" << Percentile(cpu, 50)
				  << " cpu_roundtrip_ns_p99=" << Percentile(cpu, 99)
				  << " cpu_roundtrip_ns_max=" << (cpu.empty() ? 0 : *std::ranges::max_element(cpu))
				  << " readback_latency_ns_p50=" << Percentile(readback, 50)
				  << " readback_latency_ns_p99=" << Percentile(readback, 99) << " readback_latency_ns_max="
				  << (readback.empty() ? 0 : *std::ranges::max_element(readback))
				  << " readback_polls_p50=" << Percentile(polls, 50)
				  << " readback_polls_max=" << (polls.empty() ? 0 : *std::ranges::max_element(polls))
				  << " gpu_ns_p50=";
		if (gpuAvailable) {
			std::cout << Percentile(gpu, 50);
		} else {
			std::cout << gpuTimingReason;
		}
		std::cout << " drops=" << profile.Drops << '\n';
	}

	void PrintAtExit() {
		if (ActiveFixture != nullptr) PrintProfile(ActiveFixture->Report);
	}

	void RunProfile() {
		if (std::getenv("MONO_DATA_CAPTURE_PROFILE") == nullptr)
			throw std::runtime_error("set MONO_DATA_CAPTURE_PROFILE=1 to opt in to the GPU capture profile");
		static Fixture fixture;
		ActiveFixture = &fixture;
		static const bool reportRegistered = [] {
			std::atexit(PrintAtExit);
			return true;
		}();
		(void)reportRegistered;
		fixture.Run(CaptureCount());
		if (fixture.Report.Drops != 0 || fixture.Report.Samples.empty())
			throw std::runtime_error("GPU capture profile completed with drops or no captures");
	}
}

BENCH("Vulkan data capture | queue render poll and release", 1) {
	RunProfile();
}
