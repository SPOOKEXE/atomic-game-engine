// A release-only, headless device measurement for the texture-atlas decision.
//
// This deliberately bypasses TextureTable: the table's 512 MiB content ceiling
// is production policy, while the experiment compares sixteen 4096-square
// payloads as standalone resources and 8192-square renderer-owned pages. The
// four pages run sequentially, so the experiment stays practical on modest
// devices. No result here changes content packing or residency defaults.

#include "GpuHeap.hpp"
#include "TextureAtlasProbe.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

TEST_SUITE_ID("engine.render.bench.gpu-texture-atlas")

namespace {

	// Each page has a 256 MiB source payload and one 256 MiB staging copy. Four
	// pages therefore exercise sixteen 4k sources, while sequential execution
	// keeps the largest simultaneous working set at 512 MiB on modest devices.
	constexpr uint32_t SOURCE_COUNT = 4;
	constexpr uint32_t PAGE_COUNT = 4;
	constexpr uint32_t TOTAL_SOURCE_COUNT = SOURCE_COUNT * PAGE_COUNT;
	constexpr uint32_t SOURCE_EXTENT = 4096;
	constexpr uint64_t SOURCE_BYTES = uint64_t(SOURCE_EXTENT) * SOURCE_EXTENT * 4;

	enum class Layout { Standalone, Atlas };

	struct Report {
		const char *Name = "";
		uint32_t Pages = 0;
		uint32_t Sources = 0;
		bool Timestamps = false;
		uint64_t CpuRecordingNanoseconds = 0;
		uint64_t GpuNanoseconds = 0;
		uint64_t UploadBytes = 0;
		uint32_t UploadOperations = 0;
		uint32_t TransferOperations = 0;
		uint64_t PageAllocations = 0;
		uint64_t CopyCalls = 0;
		uint64_t CacheHits = 0;
		uint64_t CacheMisses = 0;
		engine::render::GpuMemoryStatistics Before;
		engine::render::GpuMemoryStatistics SourcePageResident;
		engine::render::GpuMemoryStatistics AfterValidation;
		engine::render::GpuMemoryStatistics AfterRelease;
		uint64_t SourcePagePeakLiveBytes = 0;
		uint64_t SourcePagePeakTextureBytes = 0;
		uint64_t SourcePagePeakTransferBytes = 0;
		uint64_t WholeProbeTextureAllocations = 0;
		uint64_t WholeProbeTransferAllocations = 0;
		uint64_t WholeProbeReleasedBytes = 0;
	};

	std::optional<Report> StandaloneReport;
	std::optional<Report> AtlasReport;

	uint64_t Delta(uint64_t after, uint64_t before) {
		return after - std::min(after, before);
	}

	void PrintReports() {
		for (const std::optional<Report> &stored : {StandaloneReport, AtlasReport}) {
			if (!stored) continue;
			const Report &report = *stored;
			std::cout << "atlas-report layout=" << report.Name << " pages=" << report.Pages
					  << " sources=" << report.Sources
					  << " extent=" << SOURCE_EXTENT << " cpu_record_ns=" << report.CpuRecordingNanoseconds
					  << " gpu_ns=";
			if (report.Timestamps) {
				std::cout << report.GpuNanoseconds;
			} else {
				std::cout << "unavailable";
			}
			std::cout
				<< " upload_bytes=" << report.UploadBytes << " upload_ops=" << report.UploadOperations
				<< " transfer_ops=" << report.TransferOperations
				<< " page_allocations=" << report.PageAllocations << " copy_calls=" << report.CopyCalls
				<< " cache_hits=" << report.CacheHits << " cache_misses=" << report.CacheMisses
				<< " source_page_peak_live_bytes=" << report.SourcePagePeakLiveBytes
				<< " source_page_peak_texture_bytes=" << report.SourcePagePeakTextureBytes
				<< " source_page_peak_transfer_bytes=" << report.SourcePagePeakTransferBytes
				<< " whole_probe_texture_allocations=" << report.WholeProbeTextureAllocations
				<< " whole_probe_transfer_allocations=" << report.WholeProbeTransferAllocations
				<< " whole_probe_released_bytes=" << report.WholeProbeReleasedBytes
				<< '\n';
		}
	}

	void ArrangeReport() {
		static const bool registered = [] {
			if (std::getenv("MONO_GPU_ATLAS_REPORT") != nullptr) std::atexit(PrintReports);
			return true;
		}();
		(void)registered;
	}

	void ReleaseTextures(SDL_GPUDevice *device, std::vector<SDL_GPUTexture *> &textures) {
		for (SDL_GPUTexture *texture : textures) {
			engine::render::gpu::ReleaseTexture(device, texture);
		}
		textures.clear();
	}

	void ReleaseTransfers(SDL_GPUDevice *device, std::vector<SDL_GPUTransferBuffer *> &transfers) {
		for (SDL_GPUTransferBuffer *transfer : transfers) {
			engine::render::gpu::ReleaseTransferBuffer(device, transfer);
		}
		transfers.clear();
	}

	std::array<uint8_t, 4> Pattern(uint32_t source) {
		return {
			static_cast<uint8_t>(31 + source * 43),
			static_cast<uint8_t>(197 - source * 29),
			static_cast<uint8_t>(53 + source * 37),
			255
		};
	}

	void FillPattern(void *mapped, uint64_t bytes, uint32_t source) {
		const std::array<uint8_t, 4> pattern = Pattern(source);
		auto *pixels = static_cast<uint8_t *>(mapped);
		for (uint64_t offset = 0; offset < bytes; offset += pattern.size()) {
			std::memcpy(pixels + offset, pattern.data(), pattern.size());
		}
	}

	void ValidateAtlasSamples(
		SDL_GPUDevice *device,
		SDL_GPUTexture *atlas,
		const engine::render::TextureAtlasPlan &plan,
		uint32_t sourceBase
	) {
		constexpr uint32_t SAMPLES_PER_SOURCE = 4;
		const uint32_t sampleCount = SOURCE_COUNT * SAMPLES_PER_SOURCE;
		SDL_GPUTextureCreateInfo targetInfo{};
		targetInfo.type = SDL_GPU_TEXTURETYPE_2D;
		targetInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		targetInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		targetInfo.width = sampleCount;
		targetInfo.height = targetInfo.layer_count_or_depth = targetInfo.num_levels = 1;
		auto *target = engine::render::gpu::CreateTexture(device, &targetInfo);
		if (target == nullptr) throw std::runtime_error("atlas sample target allocation failed");
		SDL_GPUTransferBufferCreateInfo downloadInfo{};
		downloadInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		downloadInfo.size = sampleCount * 4;
		auto *download = engine::render::gpu::CreateTransferBuffer(device, &downloadInfo);
		if (download == nullptr) {
			engine::render::gpu::ReleaseTexture(device, target);
			throw std::runtime_error("atlas sample download allocation failed");
		}
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		if (command == nullptr) throw std::runtime_error("atlas sample command acquisition failed");
		for (uint32_t index = 0; index < SOURCE_COUNT; index++) {
			const engine::render::TextureAtlasRect &rect = plan.Rects[index];
			const std::array<std::array<uint32_t, 2>, SAMPLES_PER_SOURCE> corners = {{
				{rect.X, rect.Y},
				{rect.X + rect.Width - 1, rect.Y},
				{rect.X, rect.Y + rect.Height - 1},
				{rect.X + rect.Width - 1, rect.Y + rect.Height - 1},
			}};
			for (uint32_t corner = 0; corner < corners.size(); corner++) {
				SDL_GPUBlitInfo blit{};
				blit.source.texture = atlas;
				blit.source.x = corners[corner][0];
				blit.source.y = corners[corner][1];
				blit.source.w = blit.source.h = 1;
				blit.destination.texture = target;
				blit.destination.x = index * SAMPLES_PER_SOURCE + corner;
				blit.destination.w = blit.destination.h = 1;
				blit.load_op = index == 0 && corner == 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
				blit.filter = SDL_GPU_FILTER_NEAREST;
				SDL_BlitGPUTexture(command, &blit);
			}
		}
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (copy == nullptr) throw std::runtime_error("atlas sample copy pass failed");
		SDL_GPUTextureRegion source{};
		source.texture = target;
		source.w = sampleCount;
		source.h = source.d = 1;
		SDL_GPUTextureTransferInfo output{};
		output.transfer_buffer = download;
		output.pixels_per_row = sampleCount;
		output.rows_per_layer = 1;
		SDL_DownloadFromGPUTexture(copy, &source, &output);
		SDL_EndGPUCopyPass(copy);
		auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		if (fence == nullptr || !SDL_WaitForGPUFences(device, true, &fence, 1)) {
			if (fence != nullptr) SDL_ReleaseGPUFence(device, fence);
			engine::render::gpu::ReleaseTransferBuffer(device, download);
			engine::render::gpu::ReleaseTexture(device, target);
			throw std::runtime_error("atlas sample readback failed");
		}
		SDL_ReleaseGPUFence(device, fence);
		const auto *pixels = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device, download, false));
		if (pixels == nullptr) throw std::runtime_error("atlas sample map failed");
		for (uint32_t index = 0; index < SOURCE_COUNT; index++) {
			const std::array<uint8_t, 4> expected = Pattern(sourceBase + index);
			for (uint32_t corner = 0; corner < SAMPLES_PER_SOURCE; corner++) {
				const uint32_t sample = index * SAMPLES_PER_SOURCE + corner;
				if (!std::equal(expected.begin(), expected.end(), pixels + sample * expected.size())) {
					SDL_UnmapGPUTransferBuffer(device, download);
					throw std::runtime_error("atlas sample did not match source pattern");
				}
			}
		}
		SDL_UnmapGPUTransferBuffer(device, download);
		engine::render::gpu::ReleaseTransferBuffer(device, download);
		engine::render::gpu::ReleaseTexture(device, target);
	}

	Report MeasurePage(Layout layout, uint32_t sourceBase) {
		ArrangeReport();
		if (!SDL_Init(SDL_INIT_VIDEO)) {
			throw std::runtime_error(std::string("SDL video init failed: ") + SDL_GetError());
		}
		struct VideoQuit {
			~VideoQuit() {
				SDL_QuitSubSystem(SDL_INIT_VIDEO);
			}
		} videoQuit;

		engine::render::Renderer renderer;
		if (!renderer.Initialise(nullptr)) {
			throw std::runtime_error(std::string("headless renderer init failed: ") + SDL_GetError());
		}
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		if (device == nullptr) throw std::runtime_error("headless renderer returned no GPU device");

		const engine::render::TextureAtlasPlan plan =
			engine::render::PlanTextureAtlas(SOURCE_COUNT, SOURCE_EXTENT);
		if (!plan.Valid()) throw std::runtime_error("4k texture atlas plan is invalid");
		engine::render::TextureAtlasResidency residency(plan);
		const bool atlas = layout == Layout::Atlas;
		const uint32_t textureCount = atlas ? 1 : SOURCE_COUNT;
		const uint32_t transferCount = atlas ? 1 : SOURCE_COUNT;
		const uint32_t textureWidth = atlas ? plan.PageWidth : SOURCE_EXTENT;
		const uint32_t textureHeight = atlas ? plan.PageHeight : SOURCE_EXTENT;

		Report report;
		report.Name = atlas ? "atlas" : "standalone";
		report.Before = renderer.MemoryStatistics();
		std::vector<SDL_GPUTexture *> textures;
		std::vector<SDL_GPUTransferBuffer *> transfers;
		textures.reserve(textureCount);
		transfers.reserve(transferCount);

		SDL_GPUTextureCreateInfo textureInfo{};
		textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
		textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		textureInfo.width = textureWidth;
		textureInfo.height = textureHeight;
		textureInfo.layer_count_or_depth = 1;
		textureInfo.num_levels = 1;
		textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
		for (uint32_t index = 0; index < textureCount; index++) {
			auto *texture = engine::render::gpu::CreateTexture(device, &textureInfo);
			if (texture == nullptr) {
				ReleaseTextures(device, textures);
				throw std::runtime_error(std::string("texture allocation failed: ") + SDL_GetError());
			}
			textures.push_back(texture);
			if (atlas) residency.PageAllocated(0);
		}

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = static_cast<uint32_t>(atlas ? SOURCE_BYTES * SOURCE_COUNT : SOURCE_BYTES);
		for (uint32_t index = 0; index < transferCount; index++) {
			auto *transfer = engine::render::gpu::CreateTransferBuffer(device, &transferInfo);
			if (transfer == nullptr) {
				ReleaseTransfers(device, transfers);
				ReleaseTextures(device, textures);
				throw std::runtime_error(std::string("transfer allocation failed: ") + SDL_GetError());
			}
			void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
			if (mapped == nullptr) {
				engine::render::gpu::ReleaseTransferBuffer(device, transfer);
				ReleaseTransfers(device, transfers);
				ReleaseTextures(device, textures);
				throw std::runtime_error(std::string("transfer map failed: ") + SDL_GetError());
			}
			if (atlas) {
				for (uint32_t source = 0; source < SOURCE_COUNT; source++) {
					FillPattern(
						static_cast<uint8_t *>(mapped) + source * SOURCE_BYTES, SOURCE_BYTES, sourceBase + source
					);
				}
			} else {
				FillPattern(mapped, SOURCE_BYTES, sourceBase + index);
			}
			SDL_UnmapGPUTransferBuffer(device, transfer);
			transfers.push_back(transfer);
		}
		// The source textures and staging buffers coexist here. This is the
		// bounded page footprint, separate from validation's transient readback.
		report.SourcePageResident = renderer.MemoryStatistics();

		engine::render::VulkanTimestamps timestamps;
		if (!timestamps.Probe(device)) {
			throw std::runtime_error(
				"GPU timestamps unavailable; gpu-texture-atlas-bench requires Vulkan timestamps"
			);
		}
		report.Timestamps = true;
		const auto recordingStarted = std::chrono::steady_clock::now();
		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
		if (command == nullptr)
			throw std::runtime_error(std::string("command acquisition failed: ") + SDL_GetError());
		const uint32_t slot = timestamps.Begin(command);
		const uint32_t opened = timestamps.Mark(command);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
		if (copy == nullptr) {
			SDL_CancelGPUCommandBuffer(command);
			ReleaseTransfers(device, transfers);
			ReleaseTextures(device, textures);
			throw std::runtime_error(std::string("copy pass failed: ") + SDL_GetError());
		}
		for (uint32_t index = 0; index < SOURCE_COUNT; index++) {
			const engine::render::TextureAtlasRequest request =
				atlas ? residency.Request(index)
					  : engine::render::TextureAtlasRequest{
							.Rect = {.Width = SOURCE_EXTENT, .Height = SOURCE_EXTENT},
							.Upload = true,
						};
			if (!request.Valid() || !request.Upload) {
				continue;
			}
			SDL_GPUTextureTransferInfo source{};
			source.transfer_buffer = transfers[atlas ? 0 : index];
			source.offset = atlas ? index * SOURCE_BYTES : 0;
			source.pixels_per_row = SOURCE_EXTENT;
			source.rows_per_layer = SOURCE_EXTENT;
			SDL_GPUTextureRegion destination{};
			destination.texture = textures[atlas ? 0 : index];
			destination.x = atlas ? request.Rect.X : 0;
			destination.y = atlas ? request.Rect.Y : 0;
			destination.w = SOURCE_EXTENT;
			destination.h = SOURCE_EXTENT;
			destination.d = 1;
			SDL_UploadToGPUTexture(copy, &source, &destination, false);
			if (atlas) residency.Copied(index);
		}
		SDL_EndGPUCopyPass(copy);
		const uint32_t closed = timestamps.Mark(command);
		SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		const auto recordingEnded = std::chrono::steady_clock::now();
		if (fence == nullptr) {
			ReleaseTransfers(device, transfers);
			ReleaseTextures(device, textures);
			throw std::runtime_error(std::string("copy submit failed: ") + SDL_GetError());
		}
		timestamps.Submitted(slot);
		report.CpuRecordingNanoseconds = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(recordingEnded - recordingStarted).count()
		);
		report.UploadBytes = SOURCE_BYTES * SOURCE_COUNT;
		report.UploadOperations = SOURCE_COUNT;
		report.TransferOperations = transferCount;

		if (!SDL_WaitForGPUFences(device, true, &fence, 1)) {
			SDL_ReleaseGPUFence(device, fence);
			ReleaseTransfers(device, transfers);
			ReleaseTextures(device, textures);
			throw std::runtime_error(std::string("copy fence wait failed: ") + SDL_GetError());
		}
		SDL_ReleaseGPUFence(device, fence);
		if (atlas) ValidateAtlasSamples(device, textures.front(), plan, sourceBase);
		// Validation owns temporary resources, so cumulative allocation counters
		// begin before the source page and finish after validation completes.
		report.AfterValidation = renderer.MemoryStatistics();
		double times[engine::render::VulkanTimestamps::MARKS]{};
		uint32_t count = 0;
		if (!timestamps.Collect(slot, times, count) || opened >= count || closed >= count) {
			ReleaseTransfers(device, transfers);
			ReleaseTextures(device, textures);
			throw std::runtime_error("GPU timestamp readback failed");
		}
		report.GpuNanoseconds =
			static_cast<uint64_t>(engine::render::VulkanTimestamps::Between(times, opened, closed));
		if (report.GpuNanoseconds == 0) {
			ReleaseTransfers(device, transfers);
			ReleaseTextures(device, textures);
			throw std::runtime_error("GPU timestamp measurement was zero");
		}

		// A warm request must traverse the same residency object. It has no copy
		// pass to record and is counted only after the cold uploads committed.
		if (atlas) {
			for (uint32_t index = 0; index < SOURCE_COUNT; index++) {
				const engine::render::TextureAtlasRequest warm = residency.Request(index);
				if (!warm.Valid() || warm.Upload || warm.AllocatePage) {
					throw std::runtime_error("atlas warm request was not resident");
				}
			}
			const engine::render::TextureAtlasUsage &usage = residency.Usage();
			report.PageAllocations = usage.PageAllocations;
			report.CopyCalls = usage.CopyCalls;
			report.CacheHits = usage.Hits;
			report.CacheMisses = usage.Misses;
		}
		engine::core::Metrics::Count("render.gpu_atlas_probe.upload_bytes", report.UploadBytes);
		engine::core::Metrics::Count("render.gpu_atlas_probe.upload_operations", report.UploadOperations);
		engine::core::Metrics::Count("render.gpu_atlas_probe.transfer_operations", report.TransferOperations);
		engine::core::Metrics::Count("render.gpu_atlas_probe.page_allocations", report.PageAllocations);
		engine::core::Metrics::Count("render.gpu_atlas_probe.copy_calls", report.CopyCalls);
		engine::core::Metrics::Count("render.gpu_atlas_probe.cache_hits", report.CacheHits);
		engine::core::Metrics::Count("render.gpu_atlas_probe.cache_misses", report.CacheMisses);
		engine::core::Metrics::CountTime(
			"render.gpu_atlas_probe.cpu_recording", report.CpuRecordingNanoseconds
		);
		if (report.Timestamps && report.GpuNanoseconds > 0) {
			engine::core::Metrics::CountTime("render.gpu_atlas_probe.gpu_work", report.GpuNanoseconds);
		}
		engine::core::Metrics::SetGauge(
			"render.gpu_atlas_probe.source_page_peak_bytes",
			Delta(report.SourcePageResident.LiveBytes, report.Before.LiveBytes)
		);

		ReleaseTransfers(device, transfers);
		ReleaseTextures(device, textures);
		report.AfterRelease = renderer.MemoryStatistics();
		report.Pages = 1;
		report.Sources = SOURCE_COUNT;
		report.SourcePagePeakLiveBytes =
			Delta(report.SourcePageResident.LiveBytes, report.Before.LiveBytes);
		report.SourcePagePeakTextureBytes =
			Delta(report.SourcePageResident.TextureBytes, report.Before.TextureBytes);
		report.SourcePagePeakTransferBytes =
			Delta(report.SourcePageResident.TransferBufferBytes, report.Before.TransferBufferBytes);
		report.WholeProbeTextureAllocations =
			Delta(report.AfterValidation.TextureAllocations, report.Before.TextureAllocations);
		report.WholeProbeTransferAllocations =
			Delta(report.AfterValidation.TransferBufferAllocations, report.Before.TransferBufferAllocations);
		report.WholeProbeReleasedBytes =
			Delta(report.AfterRelease.ReleasedBytes, report.Before.ReleasedBytes);
		return report;
	}

	Report Measure(Layout layout) {
		Report total;
		for (uint32_t page = 0; page < PAGE_COUNT; page++) {
			const Report pageReport = MeasurePage(layout, page * SOURCE_COUNT);
			if (page == 0) {
				total.Name = pageReport.Name;
				total.Timestamps = pageReport.Timestamps;
			}
			total.Pages += pageReport.Pages;
			total.Sources += pageReport.Sources;
			total.CpuRecordingNanoseconds += pageReport.CpuRecordingNanoseconds;
			total.GpuNanoseconds += pageReport.GpuNanoseconds;
			total.UploadBytes += pageReport.UploadBytes;
			total.UploadOperations += pageReport.UploadOperations;
			total.TransferOperations += pageReport.TransferOperations;
			total.PageAllocations += pageReport.PageAllocations;
			total.CopyCalls += pageReport.CopyCalls;
			total.CacheHits += pageReport.CacheHits;
			total.CacheMisses += pageReport.CacheMisses;
			total.SourcePagePeakLiveBytes = std::max(total.SourcePagePeakLiveBytes, pageReport.SourcePagePeakLiveBytes);
			total.SourcePagePeakTextureBytes =
				std::max(total.SourcePagePeakTextureBytes, pageReport.SourcePagePeakTextureBytes);
			total.SourcePagePeakTransferBytes =
				std::max(total.SourcePagePeakTransferBytes, pageReport.SourcePagePeakTransferBytes);
			total.WholeProbeTextureAllocations += pageReport.WholeProbeTextureAllocations;
			total.WholeProbeTransferAllocations += pageReport.WholeProbeTransferAllocations;
			total.WholeProbeReleasedBytes += pageReport.WholeProbeReleasedBytes;
		}
		if (total.Sources != TOTAL_SOURCE_COUNT || total.Pages != PAGE_COUNT) {
			throw std::runtime_error("4k atlas sweep did not measure every page");
		}
		return total;
	}

	void Store(Layout layout, Report report) {
		if (layout == Layout::Atlas) {
			AtlasReport = report;
		} else {
			StandaloneReport = report;
		}
		engine::testing::Consume(report.CpuRecordingNanoseconds);
	}
}

BENCH("GPU upload | sixteen 4096x4096 RGBA8 standalone textures across four sequential pages", 1) {
	Store(Layout::Standalone, Measure(Layout::Standalone));
}

BENCH("GPU upload | sixteen 4096x4096 RGBA8 cells across four sequential atlas pages", 1) {
	Store(Layout::Atlas, Measure(Layout::Atlas));
}
