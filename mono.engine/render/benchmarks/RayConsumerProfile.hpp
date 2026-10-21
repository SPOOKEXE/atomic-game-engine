#pragma once

// Benchmark-only recording, completed device timing and pixel readback. Device
// waits and readbacks never enter the CPU recording interval.

#include <engine/assets/ContentHash.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/render/Renderer.hpp>

#include <SDL3/SDL.h>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ray_consumer_bench {
	using namespace engine;
	using Clock = std::chrono::steady_clock;
	struct ExitReport {
		std::string Key;
		std::string Text;
		uint64_t CompletedInvocations = 0;
	};
	inline std::array<ExitReport, 128> &ExitReports() {
		static std::array<ExitReport, 128> reports;
		return reports;
	}
	inline void PrintExitReports() {
		for (const auto &report : ExitReports()) {
			if (!report.CompletedInvocations) continue;
			std::cout << "# retained-benchmark-report key=" << report.Key
					  << " cohort=latest_completed_body_invocation completed_invocations="
					  << report.CompletedInvocations
					  << " invocation_count_includes_runner_warmup=1 report_capacity=128 "
						 "text_capacity_bytes=32768\n"
					  << report.Text;
		}
	}
	// Keep one bounded completed cohort per scenario. Repeated runner warmups
	// replace it; they never append unbounded diagnostic output or sample rows.
	inline void RetainCompletedReport(std::string key, std::string text) {
		if (key.empty() || key.size() > 512 || text.size() > 32768)
			throw std::runtime_error("ray benchmark report exceeds its bounded key/text capacity");
		auto &reports = ExitReports();
		static const int registered = std::atexit(PrintExitReports);
		if (registered != 0) throw std::runtime_error("could not register ray benchmark exit report");
		for (auto &report : reports) {
			if (report.CompletedInvocations && report.Key != key) continue;
			report.Key = std::move(key);
			report.Text = std::move(text);
			++report.CompletedInvocations;
			return;
		}
		throw std::runtime_error("ray benchmark report scenario capacity exceeded");
	}
	inline void Require(bool valid, std::string_view reason) {
		if (!valid) throw std::runtime_error(std::string(reason) + ": " + SDL_GetError());
	}
	inline uint32_t Setting(const char *name, uint32_t fallback, uint32_t maximum) {
		const char *configured = std::getenv(name);
		if (!configured) return fallback;
		const std::string_view input(configured);
		uint32_t number = 0;
		const auto parsed = std::from_chars(input.data(), input.data() + input.size(), number);
		Require(
			parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() && number > 0 &&
				number <= maximum,
			std::string(name) + " is outside its positive bounded integer range"
		);
		return number;
	}
	inline std::string_view Setting(const char *name, std::string_view fallback) {
		const char *configured = std::getenv(name);
		return configured ? std::string_view(configured) : fallback;
	}
	inline uint64_t Elapsed(Clock::time_point start) {
		return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	}
	inline uint64_t Percentile(std::vector<uint64_t> samples, size_t percent) {
		if (samples.empty()) return 0;
		std::sort(samples.begin(), samples.end());
		return samples[(percent * (samples.size() - 1) + 99) / 100];
	}
	inline void
	PrintTails(std::string_view name, const std::vector<uint64_t> &samples, std::ostream &out = std::cout) {
		out << ' ' << name << "_samples=" << samples.size();
		out << ' ' << name << "_tail_insufficient=" << (samples.size() < 100 ? 1 : 0);
		if (samples.empty()) {
			out << ' ' << name << "=unavailable";
			return;
		}
		out << ' ' << name << "_ns_p50=" << Percentile(samples, 50) << ' ' << name
			<< "_ns_p95=" << Percentile(samples, 95) << ' ' << name << "_ns_p99=" << Percentile(samples, 99)
			<< ' ' << name << "_ns_max=" << *std::max_element(samples.begin(), samples.end());
	}
	inline double Counter(std::string_view name) {
		const auto metric = core::Metrics::Get(std::string(name));
		return metric ? metric->Value : 0;
	}
	struct Device {
		bool Video = SDL_Init(SDL_INIT_VIDEO);
		render::Renderer Renderer;
		SDL_GPUDevice *Handle = nullptr;
		Device() {
			Require(Video, "SDL video unavailable");
			Require(Renderer.Initialise(nullptr), "headless renderer unavailable");
			Handle = static_cast<SDL_GPUDevice *>(Renderer.Backend().Device);
			Require(Handle != nullptr, "renderer has no GPU device");
			Renderer.SetProfiling(render::ProfilingTier::Full);
			Renderer.SetAnimationTime(0);
		}
		~Device() {
			Renderer.Shutdown();
			if (Video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
		void Wait() {
			Require(SDL_WaitForGPUIdle(Handle), "device completion failed");
		}
	};
	struct Image {
		uint32_t Width = 0, Height = 0, BytesPerPixel = 0;
		std::vector<std::byte> Bytes;
		bool Scalar = false;
		glm::vec4 Pixel(uint32_t x, uint32_t y) const {
			const auto *pixel = Bytes.data() + (size_t(y) * Width + x) * BytesPerPixel;
			if (Scalar) {
				float number = 0;
				std::memcpy(&number, pixel, 4);
				return {number, 0, 0, 1};
			}
			if (BytesPerPixel == 8) {
				std::array<uint16_t, 4> words{};
				std::memcpy(words.data(), pixel, 8);
				return {
					glm::unpackHalf1x16(words[0]),
					glm::unpackHalf1x16(words[1]),
					glm::unpackHalf1x16(words[2]),
					glm::unpackHalf1x16(words[3])
				};
			}
			return {
				float(std::to_integer<uint8_t>(pixel[0])) / 255,
				float(std::to_integer<uint8_t>(pixel[1])) / 255,
				float(std::to_integer<uint8_t>(pixel[2])) / 255,
				float(std::to_integer<uint8_t>(pixel[3])) / 255
			};
		}
	};
	inline Image Read(
		Device &device,
		core::Name resource,
		size_t slot,
		uint32_t width,
		uint32_t height,
		uint32_t bytesPerPixel = 8
	) {
		Require(
			width && height && width <= 3840 && height <= 2160 && (bytesPerPixel == 4 || bytesPerPixel == 8),
			"readback extent/format refused"
		);
		auto *texture = static_cast<SDL_GPUTexture *>(device.Renderer.ResourceTexture(resource, slot));
		Require(texture != nullptr, std::string("missing pinned resource ") + std::string(resource.Text()));
		const size_t row = (size_t(width) * bytesPerPixel + 255) / 256 * 256;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = uint32_t(row * height);
		const auto release = [&](SDL_GPUTransferBuffer *buffer) {
			SDL_ReleaseGPUTransferBuffer(device.Handle, buffer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(release)> transfer(
			SDL_CreateGPUTransferBuffer(device.Handle, &info), release
		);
		Require(bool(transfer), "readback transfer allocation failed");
		auto *command = SDL_AcquireGPUCommandBuffer(device.Handle);
		Require(command != nullptr, "readback command allocation failed");
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) {
			SDL_CancelGPUCommandBuffer(command);
			Require(false, "readback copy pass failed");
		}
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = width;
		source.h = height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer.get();
		destination.pixels_per_row = uint32_t(row / bytesPerPixel);
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [&](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device.Handle, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command), releaseFence
		);
		Require(bool(fence), "readback submission failed");
		const auto deadline = Clock::now() + std::chrono::seconds(30);
		while (!SDL_QueryGPUFence(device.Handle, fence.get()) && Clock::now() < deadline)
			SDL_Delay(1);
		Require(SDL_QueryGPUFence(device.Handle, fence.get()), "readback completion timed out");
		const void *mapped = SDL_MapGPUTransferBuffer(device.Handle, transfer.get(), false);
		Require(mapped != nullptr, "readback mapping failed");
		Image image{
			width, height, bytesPerPixel, std::vector<std::byte>(size_t(width) * height * bytesPerPixel)
		};
		for (uint32_t y = 0; y < height; ++y)
			std::memcpy(
				image.Bytes.data() + size_t(y) * width * bytesPerPixel,
				static_cast<const std::byte *>(mapped) + y * row,
				size_t(width) * bytesPerPixel
			);
		SDL_UnmapGPUTransferBuffer(device.Handle, transfer.get());
		return image;
	}
	inline double Difference(const Image &first, const Image &second) {
		Require(
			first.Width == second.Width && first.Height == second.Height, "parity images differ in extent"
		);
		double maximum = 0;
		for (uint32_t y = 0; y < first.Height; ++y)
			for (uint32_t x = 0; x < first.Width; ++x) {
				const auto a = first.Pixel(x, y), b = second.Pixel(x, y);
				for (uint32_t channel = 0; channel < 4; ++channel) {
					Require(std::isfinite(a[channel]) && std::isfinite(b[channel]), "nonfinite oracle pixel");
					maximum = std::max(maximum, double(std::abs(a[channel] - b[channel])));
				}
			}
		return maximum;
	}
	struct Report {
		std::vector<uint64_t> Recording, Completion;
		std::vector<std::vector<uint64_t>> Gpu;
		std::vector<std::string> Passes;
		core::HeapTotals HeapBefore, HeapAfter;
		render::GpuMemoryStatistics Before, After;
		uint64_t Dispatches = 0, Draws = 0, UploadBytes = 0, PortalPasses = 0, SurfacePasses = 0;
		uint64_t FramesWithSurfaceCaptures = 0;
		size_t MissingQueries = 0, DroppedMarks = 0;
	};
	// One outstanding submission makes the next resolved sequence unambiguous.
	// The final drain frame owes no measured work or heap bytes to this report.
	template <class Advance>
	Report Measure(
		Device &device,
		std::span<render::View> views,
		render::OverlayImage &overlay,
		uint32_t frames,
		std::span<const char *const> passNames,
		Advance advance
	) {
		Report report;
		report.Recording.reserve(frames);
		report.Completion.reserve(frames);
		for (const char *name : passNames) {
			report.Passes.emplace_back(name);
			report.Gpu.emplace_back();
			report.Gpu.back().reserve(frames);
		}
		for (uint32_t frame = 0; frame < 8; ++frame) {
			advance(frame);
			Require(
				device.Renderer.Render(views, overlay, nullptr, false).Submitted,
				"ray profile warmup did not submit"
			);
			device.Wait();
		}
		uint64_t sequence = device.Renderer.PassTimingSequence();
		report.Before = device.Renderer.MemoryStatistics();
		report.HeapBefore = core::HeapProfile::Totals();
		for (uint32_t frame = 0; frame <= frames; ++frame) {
			if (frame == frames) {
				report.After = device.Renderer.MemoryStatistics();
				report.HeapAfter = core::HeapProfile::Totals();
			}
			advance(8 + frame);
			const auto started = Clock::now();
			const auto result = device.Renderer.Render(views, overlay, nullptr, false);
			const uint64_t recording = Elapsed(started);
			Require(result.Submitted, "ray profile did not submit");
			if (frame == 0) sequence = device.Renderer.PassTimingSequence();
			if (frame > 0) {
				const uint64_t completed = device.Renderer.PassTimingSequence();
				if (completed == sequence)
					++report.MissingQueries;
				else {
					sequence = completed;
					for (size_t index = 0; index < report.Passes.size(); ++index) {
						const auto found =
							device.Renderer.PassTimings().find(core::Name(report.Passes[index]).Id());
						if (found != device.Renderer.PassTimings().end())
							report.Gpu[index].push_back(uint64_t(found->second * 1000));
					}
				}
			}
			device.Wait();
			if (frame == frames) break;
			report.Recording.push_back(recording);
			report.Completion.push_back(Elapsed(started));
			report.Dispatches += result.ComputeDispatches;
			report.Draws += result.DrawCalls;
			report.UploadBytes += result.UploadedBytes;
			report.PortalPasses += result.PortalPasses;
			report.SurfacePasses += result.SurfacePasses;
			if (result.SurfacePasses > 0) ++report.FramesWithSurfaceCaptures;
			report.DroppedMarks += device.Renderer.DroppedProfileMarks();
		}
		return report;
	}
	inline void Print(
		Device &device,
		const Report &report,
		std::string_view family,
		std::string_view scene,
		std::string_view projection,
		uint32_t width,
		uint32_t height,
		std::ostream &out = std::cout,
		std::string_view workload = {}
	) {
		const auto properties = SDL_GetGPUDeviceProperties(device.Handle);
		const auto &caps = device.Renderer.Capabilities();
		out << "# ray-consumer case=" << family << " scene=" << scene << " projection=" << projection
			<< " backend=" << device.Renderer.BackendName() << " device="
			<< std::quoted(SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unavailable"))
			<< " resolution=" << width << 'x' << height
			<< " environment_extent=1024x512 warmup=8 frames=" << report.Recording.size()
			<< " compute=" << caps.HasCompute << " storage_textures=" << caps.HasStorageTextures
			<< " timestamps=" << device.Renderer.Timed() << " unified_queue=" << caps.UnifiedQueue
			<< " dispatches=" << report.Dispatches << " draws=" << report.Draws
			<< " upload_bytes=" << report.UploadBytes << " portal_passes=" << report.PortalPasses
			<< " mirror_passes=" << report.SurfacePasses;
		if (!workload.empty()) {
			out << " workload=" << workload;
		}
		if (family == "mirror" && !workload.empty()) {
			const std::string_view weatherMedium = scene == "inactive" ? "inactive"
												   : scene == "empty"  ? "active_empty"
																	   : "active";
			out << " weather_medium=" << weatherMedium << " mirror_capture=enabled"
				<< " frames_with_surface_captures=" << report.FramesWithSurfaceCaptures;
		}
		PrintTails("recording", report.Recording, out);
		PrintTails("completion", report.Completion, out);
		const bool complete = device.Renderer.Timed() && report.DroppedMarks == 0;
		out << " gpu_timing=" << (complete ? "completed_sampled_sequences" : "unavailable_or_incomplete")
			<< " dropped_marks=" << report.DroppedMarks
			<< " frames_without_new_query=" << report.MissingQueries
			<< " gpu_sampling=renderer_schedule_actual_counts";
		for (size_t index = 0; index < report.Passes.size(); ++index)
			PrintTails(report.Passes[index], complete ? report.Gpu[index] : std::vector<uint64_t>{}, out);
		out << " live_bytes=" << report.After.LiveBytes << " peak_bytes=" << report.After.PeakBytes
			<< " allocated_bytes=" << report.After.AllocatedBytes - report.Before.AllocatedBytes
			<< " released_bytes=" << report.After.ReleasedBytes - report.Before.ReleasedBytes
			<< " created_buffers=" << report.After.BufferAllocations - report.Before.BufferAllocations
			<< " created_transfers="
			<< report.After.TransferBufferAllocations - report.Before.TransferBufferAllocations
			<< " created_textures=" << report.After.TextureAllocations - report.Before.TextureAllocations
			<< " cpu_heap=" << (core::HeapProfile::IsCompiledIn() ? "available" : "unavailable");
		if (core::HeapProfile::IsCompiledIn())
			out << " cpu_allocated_bytes=" << report.HeapAfter.TotalBytes - report.HeapBefore.TotalBytes
				<< " cpu_allocated_blocks=" << report.HeapAfter.TotalBlocks - report.HeapBefore.TotalBlocks
				<< " cpu_live_change=" << report.HeapAfter.LiveBytes - report.HeapBefore.LiveBytes
				<< " cpu_live_bytes=" << report.HeapAfter.LiveBytes
				<< " cpu_live_blocks=" << report.HeapAfter.LiveBlocks
				<< " cpu_peak_lifetime_bytes=" << report.HeapAfter.PeakBytes
				<< " cpu_peak_lifetime_scope=process_lifetime_including_preflight_not_interval_peak"
				<< " cpu_header_overhead_model_bytes=" << report.HeapAfter.OverheadBytes
				<< " cpu_block_overhead_bytes=" << core::HeapProfile::BlockOverhead()
				<< " cpu_dropped_scopes=" << report.HeapAfter.DroppedScopes - report.HeapBefore.DroppedScopes
				<< " cpu_foreign_frees=" << report.HeapAfter.ForeignFrees - report.HeapBefore.ForeignFrees;
		out << " memory=logical_payload_not_driver_commitment scheduling=one_outstanding_submission "
			   "step_counts=configured_bounds_not_executed_samples\n";
	}
}
