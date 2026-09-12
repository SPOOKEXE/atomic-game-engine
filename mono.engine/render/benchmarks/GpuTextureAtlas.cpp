// A release-only, headless device measurement for the texture-atlas decision.
//
// This deliberately bypasses TextureTable: the table's 512 MiB content ceiling
// is production policy, while the experiment needs to compare the same four
// 4096-square payloads as four resources and as one 8192-square renderer-owned
// page. No result here changes content packing or residency defaults.

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

	constexpr uint32_t SOURCE_COUNT = 4;
	constexpr uint32_t SOURCE_EXTENT = 4096;
	constexpr uint64_t SOURCE_BYTES = uint64_t(SOURCE_EXTENT) * SOURCE_EXTENT * 4;

	enum class Layout { Standalone, Atlas };

	struct Report {
		const char *Name = "";
		bool Timestamps = false;
		uint64_t CpuRecordingNanoseconds = 0;
		uint64_t GpuNanoseconds = 0;
		uint64_t UploadBytes = 0;
		uint32_t UploadOperations = 0;
		uint32_t TransferOperations = 0;
		uint32_t CacheHits = 0;
		engine::render::GpuMemoryStatistics Before;
		engine::render::GpuMemoryStatistics Resident;
		engine::render::GpuMemoryStatistics Released;
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
			std::cout << "atlas-report layout=" << report.Name << " sources=" << SOURCE_COUNT
					  << " extent=" << SOURCE_EXTENT << " cpu_record_ns=" << report.CpuRecordingNanoseconds
					  << " gpu_ns=";
			if (report.Timestamps) {
				std::cout << report.GpuNanoseconds;
			} else {
				std::cout << "unavailable";
			}
			std::cout
				<< " upload_bytes=" << report.UploadBytes << " upload_ops=" << report.UploadOperations
				<< " transfer_ops=" << report.TransferOperations << " cache_hits=" << report.CacheHits
				<< " resident_live_bytes=" << Delta(report.Resident.LiveBytes, report.Before.LiveBytes)
				<< " resident_texture_bytes="
				<< Delta(report.Resident.TextureBytes, report.Before.TextureBytes)
				<< " resident_transfer_bytes="
				<< Delta(report.Resident.TransferBufferBytes, report.Before.TransferBufferBytes)
				<< " texture_allocations="
				<< Delta(report.Resident.TextureAllocations, report.Before.TextureAllocations)
				<< " transfer_allocations="
				<< Delta(report.Resident.TransferBufferAllocations, report.Before.TransferBufferAllocations)
				<< " released_bytes=" << Delta(report.Released.ReleasedBytes, report.Before.ReleasedBytes)
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

	Report Measure(Layout layout) {
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
			std::memset(mapped, 0x7f, transferInfo.size);
			SDL_UnmapGPUTransferBuffer(device, transfer);
			transfers.push_back(transfer);
		}
		report.Resident = renderer.MemoryStatistics();

		engine::render::VulkanTimestamps timestamps;
		report.Timestamps = timestamps.Probe(device);
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
			SDL_GPUTextureTransferInfo source{};
			source.transfer_buffer = transfers[atlas ? 0 : index];
			source.offset = atlas ? index * SOURCE_BYTES : 0;
			source.pixels_per_row = SOURCE_EXTENT;
			source.rows_per_layer = SOURCE_EXTENT;
			SDL_GPUTextureRegion destination{};
			destination.texture = textures[atlas ? 0 : index];
			destination.x = atlas ? plan.Rects[index].X : 0;
			destination.y = atlas ? plan.Rects[index].Y : 0;
			destination.w = SOURCE_EXTENT;
			destination.h = SOURCE_EXTENT;
			destination.d = 1;
			SDL_UploadToGPUTexture(copy, &source, &destination, false);
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
		if (report.Timestamps) {
			double times[engine::render::VulkanTimestamps::MARKS]{};
			uint32_t count = 0;
			if (timestamps.Collect(slot, times, count) && opened < count && closed < count) {
				report.GpuNanoseconds =
					static_cast<uint64_t>(engine::render::VulkanTimestamps::Between(times, opened, closed));
			} else {
				report.Timestamps = false;
			}
		}

		// A second request with this unchanged plan reuses every resident page.
		// It records no copy, creates no resource, and is the cache result this
		// experiment reports rather than inventing a default eviction policy.
		std::unordered_map<uint32_t, SDL_GPUTexture *> resident;
		for (uint32_t index = 0; index < SOURCE_COUNT; index++) {
			resident.emplace(index, textures[atlas ? 0 : index]);
		}
		for (uint32_t index = 0; index < SOURCE_COUNT; index++) {
			if (resident.contains(index)) report.CacheHits++;
		}
		engine::core::Metrics::Count("render.gpu_atlas_probe.upload_bytes", report.UploadBytes);
		engine::core::Metrics::Count("render.gpu_atlas_probe.upload_operations", report.UploadOperations);
		engine::core::Metrics::Count("render.gpu_atlas_probe.transfer_operations", report.TransferOperations);
		engine::core::Metrics::Count("render.gpu_atlas_probe.cache_hits", report.CacheHits);
		engine::core::Metrics::CountTime(
			"render.gpu_atlas_probe.cpu_recording", report.CpuRecordingNanoseconds
		);
		if (report.Timestamps && report.GpuNanoseconds > 0) {
			engine::core::Metrics::CountTime("render.gpu_atlas_probe.gpu_work", report.GpuNanoseconds);
		}
		engine::core::Metrics::SetGauge(
			"render.gpu_atlas_probe.resident_bytes", Delta(report.Resident.LiveBytes, report.Before.LiveBytes)
		);

		ReleaseTransfers(device, transfers);
		ReleaseTextures(device, textures);
		report.Released = renderer.MemoryStatistics();
		return report;
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

BENCH("GPU upload | four 4096x4096 RGBA8 standalone textures", 1) {
	Store(Layout::Standalone, Measure(Layout::Standalone));
}

BENCH("GPU upload | four 4096x4096 RGBA8 cells in one atlas", 1) {
	Store(Layout::Atlas, Measure(Layout::Atlas));
}
