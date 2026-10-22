// Resident source fixtures are built during warm-up. Reports separate submission
// CPU time, completed GPU timestamps and fence-completed latency.

#include "BenchGpuPacing.hpp"
#include "GpuHeap.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.render.bench.imagegraph_gpu")

namespace {
	using namespace engine;
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
		bool HeapCaptured = false;
		core::HeapTotals HeapBefore, HeapAfter;
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
			if (report.HeapCaptured) {
				std::cout << "b0-gpu-image-heap extent=" << report.Extent
						  << " edit=" << EDIT_NAMES[static_cast<size_t>(report.Change)]
						  << " calls=" << report.Calls
						  << " boundary=measured_request_loop_through_fence_completion "
							 "cold_retirement_excluded=1 fixtures_excluded=1 readback_oracles_excluded=1 "
							 "measurement_bookkeeping_included=1 cohort=last_completed_body_invocation";
				PrintHeap(report.HeapBefore, report.HeapAfter);
			}
		}
	}

	void Check(bool accepted, std::string_view operation) {
		if (!accepted) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
	}

	struct Fixture {
		testing::BenchGpuPacing Budget;
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
			Budget.Begin();
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
			if (extent == 1) Document.Nodes = {{"solid", imagegraph::Solid{1, 1, {17, 31, 47, 255}}, {}, {}}};
			if (extent == 1) Document.Outputs = {{"main", "solid", imagegraph::OutputSpace::SRGB}};
			Check(Renderer.AddTexture(Source, Pixels, Owner), "source fixture upload");
			imagegraph::Diagnostic diagnostic;
			if (!Renderer.SetImageGraph(Owner, Graph, Document, Sources, diagnostic))
				throw std::runtime_error(diagnostic.Message);
			if (Renderer.EvaluateImageGraph(Owner, Graph, "main", Output, diagnostic) !=
				render::ImageGraphEvaluation::Updated)
				throw std::runtime_error(diagnostic.Message);
			Complete();
			SettleBudget();
		}

		void SettleBudget() {
			if (Budget.Percent)
				Check(
					SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(Renderer.Backend().Device)),
					"GPU budget drain"
				);
			Budget.Complete();
			Budget.Wait();
		}
		void PrintBudget(uint64_t before) const {
			std::cout << "b0-image-gpu-budget counter_scope=fixture_lifetime_cumulative extent="
					  << Pixels.Width << " cohort_completed_bursts=" << Budget.CompletedBursts - before;
			Budget.Print(std::cout);
			std::cout << '\n';
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
		// Direct statics register teardown after renderer dependencies initialize.
		if (extent == 256) {
			static Fixture small(256);
			return small;
		}
		static Fixture large(1024);
		return large;
	}

	imagegraph::Image Download(Fixture &fixture) {
		fixture.Budget.Begin();
		uint32_t width = 0, height = 0;
		Check(fixture.Renderer.TextureSize(fixture.Output, width, height, fixture.Owner), "output extent");
		if (!width || !height || width > 1024 || height > 1024)
			throw std::runtime_error("B0 GPU image readback extent invalid");
		auto *device = static_cast<SDL_GPUDevice *>(fixture.Renderer.Backend().Device);
		auto *texture =
			static_cast<SDL_GPUTexture *>(fixture.Renderer.TextureHandle(fixture.Output, fixture.Owner));
		Check(texture != nullptr, "output texture");
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
		Check(buffer != nullptr, "oracle transfer allocation");
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		Check(command != nullptr, "oracle copy acquisition");
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) {
			SDL_CancelGPUCommandBuffer(command);
			throw std::runtime_error("oracle copy pass: " + std::string(SDL_GetError()));
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
		Check(SDL_SubmitGPUCommandBuffer(command), "oracle copy submission");
		fixture.Complete();
		imagegraph::Image image{width, height, {}};
		image.Pixels.resize(size_t(width) * height * 4);
		const auto *mapped =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, buffer.get(), false));
		Check(mapped != nullptr, "oracle transfer mapping");
		for (size_t row = 0; row < height; ++row)
			std::memcpy(image.Pixels.data() + row * width * 4, mapped + row * pitch, width * 4);
		SDL_UnmapGPUTransferBuffer(device, buffer.get());
		fixture.SettleBudget();
		return image;
	}

	void VerifyPixels(Fixture &fixture) {
		imagegraph::Plan plan;
		imagegraph::Diagnostic diagnostic;
		imagegraph::Image expected;
		const imagegraph::SourceResolver resolve =
			[&](std::string_view path, imagegraph::Image &out, std::string &failure) {
				if (path != "source.png") {
					failure = "unexpected B0 source";
					return false;
				}
				out = {fixture.Pixels.Width, fixture.Pixels.Height, fixture.Pixels.Pixels};
				return true;
			};
		if (!imagegraph::Compile(fixture.Document, plan, diagnostic) ||
			!imagegraph::Evaluate(fixture.Document, plan, "main", resolve, expected, diagnostic))
			throw std::runtime_error("B0 GPU image CPU oracle refused: " + diagnostic.Message);
		if (fixture.Renderer.TextureSamplesSRGB(fixture.Output, fixture.Owner) !=
			(expected.Space == imagegraph::OutputSpace::SRGB))
			throw std::runtime_error("B0 GPU image published sampling encoding mismatch");
		const auto actual = Download(fixture);
		if (actual.Width != expected.Width || actual.Height != expected.Height ||
			actual.Pixels.size() != expected.Pixels.size())
			throw std::runtime_error("B0 GPU image full-pixel extent mismatch");
		for (size_t index = 0; index < actual.Pixels.size(); ++index)
			if (std::abs(
					std::to_integer<int>(actual.Pixels[index]) - std::to_integer<int>(expected.Pixels[index])
				) > 1)
				throw std::runtime_error(
					"B0 GPU image full-pixel mismatch beyond declared one-byte tolerance"
				);
	}

	void MeasureLatency(Fixture &fixture, uint32_t extent, Edit change, bool empty = false) {
		const auto budgetBefore = fixture.Budget.CompletedBursts;
		const size_t trials = LatencyOption("MONO_B0_TRIALS", 101, 1, 512);
		const size_t warmups = LatencyOption("MONO_B0_WARMUPS", 16, 0, 128);
		std::vector<uint64_t> apiTimes, completedTimes;
		std::vector<double> recordingTimes, gpuTimes;
		uint64_t dispatches = 0, commands = 0, copiedBytes = 0, allocatedBytes = 0;
		apiTimes.reserve(trials);
		completedTimes.reserve(trials);
		recordingTimes.reserve(trials);
		gpuTimes.reserve(trials);
		fixture.Budget.Begin();
		fixture.Complete();
		(void)fixture.Renderer.ImageGraphProfile();
		fixture.SettleBudget();
		for (size_t trial = 0; trial < trials + warmups; ++trial) {
			fixture.Budget.Begin();
			if (!empty && change == Edit::Cold)
				Check(fixture.Renderer.DropImageGraph(fixture.Owner, fixture.Graph), "cold graph retirement");
			const auto before = fixture.Renderer.ImageGraphProfile();
			const auto memoryBefore = fixture.Renderer.MemoryStatistics();
			const auto *priorOutput = fixture.Renderer.TextureHandle(fixture.Output, fixture.Owner);
			imagegraph::Diagnostic diagnostic;
			const auto started = std::chrono::steady_clock::now();
			if (empty) {
				if (fixture.Renderer.SetImageGraph(
						fixture.Owner, fixture.Graph, imagegraph::Document{}, {}, diagnostic
					) ||
					diagnostic.Message.empty())
					throw std::runtime_error("B0 GPU empty document did not refuse");
			} else {
				if (change == Edit::Source) {
					fixture.Pixels.Pixels[0] = std::byte{static_cast<uint8_t>(++fixture.Revision)};
					Check(
						fixture.Renderer.AddTexture(fixture.Source, fixture.Pixels, fixture.Owner),
						"source edit upload"
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
				if (result != (change == Edit::Unchanged ? render::ImageGraphEvaluation::Reused
														 : render::ImageGraphEvaluation::Updated))
					throw std::runtime_error("B0 GPU image unexpected evaluation: " + diagnostic.Message);
			}
			const auto submitted = std::chrono::steady_clock::now();
			const bool updated = !empty && change != Edit::Unchanged;
			if (updated) fixture.Complete();
			const auto completed = std::chrono::steady_clock::now();
			auto after = fixture.Renderer.ImageGraphProfile();
			if (updated) {
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (after.GpuTimingSequence < before.GpuTimingSequence + 1 &&
					   std::chrono::steady_clock::now() < deadline) {
					SDL_Delay(1);
					after = fixture.Renderer.ImageGraphProfile();
				}
				if (!after.HasGpuTimings || after.GpuTimingSequence != before.GpuTimingSequence + 1 ||
					!std::isfinite(after.GpuMicroseconds) || after.GpuMicroseconds <= 0 ||
					!std::isfinite(after.CpuRecordingMicroseconds) || after.CpuRecordingMicroseconds <= 0)
					throw std::runtime_error("B0 GPU image unique completed timing unavailable");
			} else if (after.ComputeDispatches != before.ComputeDispatches ||
					   after.CommandBuffers != before.CommandBuffers ||
					   after.CopiedBytes != before.CopiedBytes ||
					   after.GpuTimingSequence != before.GpuTimingSequence ||
					   fixture.Renderer.MemoryStatistics().AllocatedBytes != memoryBefore.AllocatedBytes ||
					   fixture.Renderer.TextureHandle(fixture.Output, fixture.Owner) != priorOutput)
				throw std::runtime_error("B0 GPU image inactive request changed output or submitted work");

			const auto memoryAfter = fixture.Renderer.MemoryStatistics();
			if (updated && (after.ComputeDispatches <= before.ComputeDispatches ||
							after.CommandBuffers <= before.CommandBuffers ||
							after.CopiedBytes - before.CopiedBytes != uint64_t{extent} * extent * 4))
				throw std::runtime_error("B0 GPU image changed request submitted incomplete work");
			if (trial >= warmups) {
				dispatches += after.ComputeDispatches - before.ComputeDispatches;
				commands += after.CommandBuffers - before.CommandBuffers;
				copiedBytes += after.CopiedBytes - before.CopiedBytes;
				allocatedBytes += memoryAfter.AllocatedBytes - memoryBefore.AllocatedBytes;
				apiTimes.push_back(
					std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - started).count()
				);
				completedTimes.push_back(
					std::chrono::duration_cast<std::chrono::nanoseconds>(completed - started).count()
				);
				if (updated) {
					recordingTimes.push_back(after.CpuRecordingMicroseconds);
					gpuTimes.push_back(after.GpuMicroseconds);
				}
			}
			fixture.SettleBudget();
			// Download and complete CPU evaluation are excluded from every duration/counter window.
			VerifyPixels(fixture);
		}
		fixture.PrintBudget(budgetBefore);
		std::cout << "b0-gpu-image-latency extent=" << extent
				  << " edit=" << (empty ? "empty_refusal" : EDIT_NAMES[static_cast<size_t>(change)])
				  << " warmups=" << warmups
				  << " one_in_flight=1 full_pixels=1 tolerance_bytes=1 oracle_outside_clock=1"
				  << " harness_summary=not_individual_latency quantile=nearest_rank heap=compiled_out "
					 "quiet_host=unverified tail_valid="
				  << (trials >= 100);
		std::cout << " dispatches=" << dispatches << " commands=" << commands
				  << " copied_bytes=" << copiedBytes << " gpu_allocated_bytes=" << allocatedBytes
				  << " gpu_memory=logical_payload_not_driver_commitment";
		PrintTails("api_ns", apiTimes);
		PrintTails("completed_ns", completedTimes);
		if (gpuTimes.empty())
			std::cout << " gpu_us=n/a record_us=n/a gpu_samples=0";
		else {
			PrintTails("record_us", recordingTimes);
			PrintTails("gpu_us", gpuTimes);
		}
		std::cout << '\n';
	}

	void Measure(uint32_t extent, Edit change, uint64_t calls = 1) {
		if (LatencyMode()) {
			if (extent == 256 && change == Edit::Cold) {
				MeasureLatency(GetFixture(256), 256, Edit::Unchanged, true);
				static Fixture simple(1);
				MeasureLatency(simple, 1, Edit::Cold);
			}
			MeasureLatency(GetFixture(extent), extent, change);
			return;
		}
		const bool memory = MemoryMode();
		static const bool reporting = [memory] {
			if (memory || std::getenv("MONO_GPU_IMAGEGRAPH_REPORT") != nullptr) std::atexit(PrintReports);
			return true;
		}();
		(void)reporting;
		auto &fixture = GetFixture(extent);
		const auto budgetBefore = fixture.Budget.CompletedBursts;
		fixture.Budget.Begin();
		imagegraph::Diagnostic diagnostic;
		if (change == Edit::Cold)
			Check(fixture.Renderer.DropImageGraph(fixture.Owner, fixture.Graph), "cold graph retirement");
		Report report;
		report.Extent = extent;
		report.Change = change;
		report.Calls = calls;
		report.Before = fixture.Renderer.ImageGraphProfile();
		report.MemoryBefore = fixture.Renderer.MemoryStatistics();
		report.HeapCaptured = memory;
		if (memory) report.HeapBefore = core::HeapProfile::Totals();
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
		const auto completed = std::chrono::steady_clock::now();
		if (memory) report.HeapAfter = core::HeapProfile::Totals();
		report.CompletedNanoseconds = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(completed - started).count()
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
		fixture.SettleBudget();
		Reports[(extent == 256 ? 0 : 4) + static_cast<size_t>(change)] = report;
		fixture.PrintBudget(budgetBefore);
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
