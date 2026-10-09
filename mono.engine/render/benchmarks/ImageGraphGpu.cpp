// Resident source fixtures are built during warm-up. Reports separate submission
// CPU time, completed GPU timestamps and fence-completed latency.

#include <engine/assets/Texture.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.render.bench.imagegraph_gpu")

namespace {
	using namespace engine;
	enum class Edit : uint8_t { Cold, Source, Transform, Unchanged };
	constexpr std::array<std::string_view, 4> EDIT_NAMES{
		"cold", "source-edit", "transform-edit", "cache-hit"
	};

	struct Report {
		uint32_t Extent = 0;
		Edit Change = Edit::Cold;
		uint64_t Calls = 0;
		uint64_t CompletedNanoseconds = 0;
		render::ImageGraphStatistics Before;
		render::ImageGraphStatistics After;
		render::GpuMemoryStatistics MemoryBefore;
		render::GpuMemoryStatistics MemoryAfter;
	};
	std::array<std::optional<Report>, 8> Reports;

	void PrintReports() {
		for (const auto &stored : Reports) {
			if (!stored) continue;
			const auto &report = *stored;
			std::cout << "gpu-imagegraph-report extent=" << report.Extent
					  << " edit=" << EDIT_NAMES[static_cast<size_t>(report.Change)]
					  << " calls=" << report.Calls
					  << " completed_ns_per_call=" << report.CompletedNanoseconds / report.Calls
					  << " cpu_record_us=";
			if (report.Change == Edit::Unchanged)
				std::cout << "n/a";
			else
				std::cout << report.After.CpuRecordingMicroseconds;
			std::cout << " gpu_us=";
			if (report.Change == Edit::Unchanged)
				std::cout << "n/a";
			else if (report.After.HasGpuTimings &&
					 report.After.GpuTimingSequence > report.Before.GpuTimingSequence)
				std::cout << report.After.GpuMicroseconds;
			else
				std::cout << "unavailable";
			std::cout << " dispatches=" << report.After.ComputeDispatches - report.Before.ComputeDispatches
					  << " commands=" << report.After.CommandBuffers - report.Before.CommandBuffers
					  << " copied_bytes=" << report.After.CopiedBytes - report.Before.CopiedBytes
					  << " graph_allocated_bytes="
					  << report.After.AllocatedBytes - report.Before.AllocatedBytes
					  << " device_allocated_bytes="
					  << report.MemoryAfter.AllocatedBytes - report.MemoryBefore.AllocatedBytes
					  << " graph_resident_bytes=" << report.After.ResidentBytes
					  << " cache_hits=" << report.After.CacheHits - report.Before.CacheHits << '\n';
		}
	}

	void Check(bool accepted, std::string_view operation) {
		if (!accepted) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
	}

	struct Fixture {
		render::Renderer Renderer;
		core::Name Owner;
		core::Name Graph;
		core::Name Source;
		core::Name Output;
		assets::TextureData Pixels;
		imagegraph::Document Document;
		std::array<render::ImageGraphSourceBinding, 1> Sources;
		uint64_t Revision = 0;

		explicit Fixture(uint32_t extent)
			: Owner("imagegraph-benchmark-" + std::to_string(extent)), Graph("composer"),
			  Source("source.png"), Output("output.png") {
			Check(SDL_Init(SDL_INIT_VIDEO), "SDL video initialization");
			Check(Renderer.Initialise(nullptr), "headless renderer initialization");
			Renderer.SetProfiling(render::ProfilingTier::Full);
			Pixels.Width = extent;
			Pixels.Height = extent;
			Pixels.Format = assets::TextureFormat::RGBA8;
			Pixels.Pixels.resize(static_cast<size_t>(extent) * extent * 4);
			for (uint32_t y = 0; y < extent; y++)
				for (uint32_t x = 0; x < extent; x++) {
					const size_t offset = (static_cast<size_t>(y) * extent + x) * 4;
					Pixels.Pixels[offset] = std::byte{static_cast<uint8_t>(x)};
					Pixels.Pixels[offset + 1] = std::byte{static_cast<uint8_t>(y)};
					Pixels.Pixels[offset + 2] = std::byte{static_cast<uint8_t>(x ^ y)};
					Pixels.Pixels[offset + 3] = std::byte{static_cast<uint8_t>(64 + (x + y) % 192)};
				}
			Sources[0] = {"source.png", Source, Owner, imagegraph::SourceInterpretation::Colour};
			Document = {
				.Nodes =
					{{"source", imagegraph::Source{"source.png"}, {}, {}},
					 {"transform",
					  imagegraph::Transform{
						  extent,
						  extent,
						  .25,
						  -.25,
						  .9,
						  1.05,
						  17,
						  extent * .5,
						  extent * .5,
						  imagegraph::Sampling::Bilinear
					  },
					  {"source"},
					  {}},
					 {"blend", imagegraph::Blend{.7}, {"source", "transform"}, {}}},
				.Outputs = {{"main", "blend", imagegraph::OutputSpace::SRGB}},
				.Parameters = {},
				.Bindings = {}
			};
			Check(Renderer.AddTexture(Source, Pixels, Owner), "source fixture upload");
			imagegraph::Diagnostic diagnostic;
			if (!Renderer.SetImageGraph(Owner, Graph, Document, Sources, diagnostic))
				throw std::runtime_error(diagnostic.Message);
			if (Renderer.EvaluateImageGraph(Owner, Graph, "main", Output, diagnostic) !=
				render::ImageGraphEvaluation::Updated)
				throw std::runtime_error(diagnostic.Message);
			Complete();
		}

		~Fixture() {
			Renderer.Shutdown();
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}

		// An empty later submission fences earlier producer work on SDL's ordered
		// queue. Benchmarks include completion, without a readback allocation.
		void Complete() {
			auto *device = static_cast<SDL_GPUDevice *>(Renderer.Backend().Device);
			auto *command = SDL_AcquireGPUCommandBuffer(device);
			Check(command != nullptr, "completion command acquisition");
			const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
			std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
				SDL_SubmitGPUCommandBufferAndAcquireFence(command), releaseFence
			);
			Check(fence != nullptr, "completion command submission");
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
				SDL_Delay(1);
			Check(SDL_QueryGPUFence(device, fence.get()), "GPU completion deadline exceeded");
		}
	};

	Fixture &GetFixture(uint32_t extent) {
		// Lazy fixtures initialize only when this opt-in suite runs. Harness warm-up
		// pays decoding-pattern construction and device/pipeline initialization once.
		static std::array<std::unique_ptr<Fixture>, 2> fixtures;
		const size_t index = extent == 256 ? 0 : 1;
		if (!fixtures[index]) fixtures[index] = std::make_unique<Fixture>(extent);
		return *fixtures[index];
	}

	void Measure(uint32_t extent, Edit change, uint64_t calls = 1) {
		static const bool reporting = [] {
			if (std::getenv("MONO_GPU_IMAGEGRAPH_REPORT") != nullptr) std::atexit(PrintReports);
			return true;
		}();
		(void)reporting;
		auto &fixture = GetFixture(extent);
		imagegraph::Diagnostic diagnostic;
		if (change == Edit::Cold)
			Check(fixture.Renderer.DropImageGraph(fixture.Owner, fixture.Graph), "cold graph retirement");
		Report report;
		report.Extent = extent;
		report.Change = change;
		report.Calls = calls;
		report.Before = fixture.Renderer.ImageGraphProfile();
		report.MemoryBefore = fixture.Renderer.MemoryStatistics();
		const auto started = std::chrono::steady_clock::now();
		for (uint64_t call = 0; call < calls; call++) {
			if (change == Edit::Source) {
				fixture.Pixels.Pixels[0] = std::byte{static_cast<uint8_t>(++fixture.Revision)};
				Check(
					fixture.Renderer.AddTexture(fixture.Source, fixture.Pixels, fixture.Owner),
					"source revision upload"
				);
			}
			if (change == Edit::Transform) {
				auto &transform = std::get<imagegraph::Transform>(fixture.Document.Nodes[1].Value);
				transform.Degrees = transform.Degrees == 17 ? 19 : 17;
			}
			if (change == Edit::Cold || change == Edit::Transform)
				if (!fixture.Renderer.SetImageGraph(
						fixture.Owner, fixture.Graph, fixture.Document, fixture.Sources, diagnostic
					))
					throw std::runtime_error(diagnostic.Message);
			const auto result = fixture.Renderer.EvaluateImageGraph(
				fixture.Owner, fixture.Graph, "main", fixture.Output, diagnostic
			);
			const auto expected = change == Edit::Unchanged ? render::ImageGraphEvaluation::Reused
															: render::ImageGraphEvaluation::Updated;
			if (result != expected)
				throw std::runtime_error("unexpected imagegraph evaluation: " + diagnostic.Message);
			if (change != Edit::Unchanged) fixture.Complete();
			testing::Consume(result);
		}
		report.CompletedNanoseconds = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
				.count()
		);
		report.After = fixture.Renderer.ImageGraphProfile();
		report.MemoryAfter = fixture.Renderer.MemoryStatistics();
		if (change == Edit::Unchanged &&
			(report.After.ComputeDispatches != report.Before.ComputeDispatches ||
			 report.After.CommandBuffers != report.Before.CommandBuffers ||
			 report.After.CopiedBytes != report.Before.CopiedBytes ||
			 report.MemoryAfter.AllocatedBytes != report.MemoryBefore.AllocatedBytes))
			throw std::runtime_error("unchanged imagegraph submitted work or allocated GPU memory");
		if (change != Edit::Unchanged && (!report.After.HasGpuTimings ||
										  report.After.GpuTimingSequence <= report.Before.GpuTimingSequence ||
										  report.After.GpuMicroseconds <= 0))
			throw std::runtime_error("completed Vulkan imagegraph timestamp measurement unavailable");
		Reports[(extent == 256 ? 0 : 4) + static_cast<size_t>(change)] = report;
		testing::Consume(report.CompletedNanoseconds);
	}
}

BENCH("GPU ImageGraph | 256x256 | cold resident-source composition", 1) {
	Measure(256, Edit::Cold);
}
BENCH("GPU ImageGraph | 256x256 | source replacement and composition", 1) {
	Measure(256, Edit::Source);
}
BENCH("GPU ImageGraph | 256x256 | transform edit and dependent blend", 1) {
	Measure(256, Edit::Transform);
}
BENCH("GPU ImageGraph | 256x256 | unchanged resident output", 1000) {
	Measure(256, Edit::Unchanged, 1000);
}
BENCH("GPU ImageGraph | 1024x1024 | cold resident-source composition", 1) {
	Measure(1024, Edit::Cold);
}
BENCH("GPU ImageGraph | 1024x1024 | source replacement and composition", 1) {
	Measure(1024, Edit::Source);
}
BENCH("GPU ImageGraph | 1024x1024 | transform edit and dependent blend", 1) {
	Measure(1024, Edit::Transform);
}
BENCH("GPU ImageGraph | 1024x1024 | unchanged resident output", 1000) {
	Measure(1024, Edit::Unchanged, 1000);
}
