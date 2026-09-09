#include "RendererState.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/PortalShadowPacked.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

namespace engine::render {
	namespace {
		std::atomic<uint64_t> NextPortalShadowHandle{1};

		std::array<float, 6> BoundsOf(const core::AABB &bounds) {
			return {
				bounds.Minimum.X,
				bounds.Minimum.Y,
				bounds.Minimum.Z,
				bounds.Maximum.X,
				bounds.Maximum.Y,
				bounds.Maximum.Z
			};
		}
	}

	uint64_t
	Renderer::QueuePortalShadowImage(const PortalShadowImageBinding &binding, PortalShadowImage &&image) {
		RequireOwningThread("QueuePortalShadowImage");
		if (!State->Device || State->DepthFormat != SDL_GPU_TEXTUREFORMAT_D32_FLOAT ||
			!binding.WorldName.IsValid() || image.Depth.size() != PORTAL_SHADOW_BYTES ||
			image.Depth.capacity() >
				MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes ||
			PORTAL_SHADOW_BYTES > MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes)
			return 0;
		const auto free = std::find_if(
			State->ImportedPortalShadows.begin(), State->ImportedPortalShadows.end(), [](const auto &held) {
				return held.Handle == 0;
			}
		);
		if (free == State->ImportedPortalShadows.end() ||
			!ValidPortalShadowSnapshot(binding.ExpectedSnapshot) ||
			image.Snapshot != binding.ExpectedSnapshot || !ValidPortalShadowImage(image))
			return 0;
		if (State->PortalImportUsage.CachedTextureBytes >
			MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes - PORTAL_SHADOW_BYTES)
			State->ReleaseResidentImageCache();
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		info.width = info.height = PORTAL_SHADOW_EXTENT;
		info.layer_count_or_depth = info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		auto *texture = gpu::CreateTexture(State->Device, &info);
		if (!texture) return 0;
		const uint64_t handle = NextPortalShadowHandle.fetch_add(1, std::memory_order_relaxed);
		if (!handle) {
			gpu::ReleaseTexture(State->Device, texture);
			return 0;
		}
		free->Handle = handle;
		free->Binding = binding;
		free->Pending = std::move(image.Depth);
		free->Texture = texture;
		free->GpuBytes = PORTAL_SHADOW_BYTES;
		State->PortalImportUsage.TextureBytes += PORTAL_SHADOW_BYTES;
		State->PortalImportUsage.PendingCpuBytes += free->Pending.capacity();
		++State->PortalImportUsage.Images;
		State->ReportPortalImportUsage();
		return handle;
	}

	uint64_t Renderer::QueuePackedPortalShadowImage(
		const PortalShadowImageBinding &binding, PortalShadowImage &&image
	) {
		RequireOwningThread("QueuePackedPortalShadowImage");
		ENGINE_PROFILE("pack retained shadow");
		const size_t cpuAvailable = MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes;
		if (!State->Device || State->DepthFormat != SDL_GPU_TEXTUREFORMAT_D32_FLOAT ||
			!binding.WorldName.IsValid() || image.Depth.capacity() > cpuAvailable ||
			image.Snapshot != binding.ExpectedSnapshot || !ValidPortalShadowImage(image))
			return 0;
		// Fragment depth output can flush subnormals. Texture transfer preserves those bits.
		for (size_t offset = 0; offset < image.Depth.size(); offset += sizeof(uint32_t)) {
			uint32_t bits = 0;
			for (size_t byte = 0; byte < sizeof(uint32_t); ++byte)
				bits |= std::to_integer<uint32_t>(image.Depth[offset + byte]) << (8 * byte);
			if (bits != 0 && bits < 0x00800000u) return QueuePortalShadowImage(binding, std::move(image));
		}
		const auto free = std::find_if(
			State->ImportedPortalShadows.begin(), State->ImportedPortalShadows.end(), [](const auto &held) {
				return held.Handle == 0;
			}
		);
		if (free == State->ImportedPortalShadows.end()) return 0;
		std::vector<uint32_t> packed;
		std::string error;
		if (!PackPortalShadow(image.Depth, cpuAvailable - image.Depth.capacity(), packed, error)) return 0;
		const size_t bytes = packed.size() * sizeof(uint32_t);
		if (packed.capacity() * sizeof(uint32_t) > cpuAvailable - image.Depth.capacity() ||
			bytes > MAX_IMPORTED_PORTAL_STAGING_BYTES ||
			bytes > MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes)
			return 0;
		if (!State->PackedShadowPipeline) {
			auto *vertex = State->LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
			auto *fragment = State->LoadShader("shadow-unpack.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 1);
			if (vertex && fragment) {
				SDL_GPUGraphicsPipelineCreateInfo info{};
				info.vertex_shader = vertex;
				info.fragment_shader = fragment;
				info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
				info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
				info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				info.rasterizer_state.enable_depth_clip = true;
				info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
				info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
				info.depth_stencil_state.enable_depth_test = true;
				info.depth_stencil_state.enable_depth_write = true;
				info.target_info.has_depth_stencil_target = true;
				info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
				State->PackedShadowPipeline = SDL_CreateGPUGraphicsPipeline(State->Device, &info);
			}
			if (vertex) SDL_ReleaseGPUShader(State->Device, vertex);
			if (fragment) SDL_ReleaseGPUShader(State->Device, fragment);
			if (!State->PackedShadowPipeline) return 0;
		}
		if (State->PortalImportUsage.CachedTextureBytes >
			MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes - bytes)
			State->ReleaseResidentImageCache();
		SDL_GPUBufferCreateInfo info{};
		info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
		info.size = static_cast<uint32_t>(bytes);
		auto *buffer = gpu::CreateBuffer(State->Device, &info);
		if (!buffer) return 0;
		const uint64_t handle = NextPortalShadowHandle.fetch_add(1, std::memory_order_relaxed);
		if (!handle) {
			gpu::ReleaseBuffer(State->Device, buffer);
			return 0;
		}
		free->Handle = handle;
		free->Binding = binding;
		free->PackedPending = std::move(packed);
		free->Packed = buffer;
		free->GpuBytes = bytes;
		std::vector<std::byte>{}.swap(image.Depth);
		State->PortalImportUsage.TextureBytes += bytes;
		State->PortalImportUsage.PendingCpuBytes += free->PackedPending.capacity() * sizeof(uint32_t);
		++State->PortalImportUsage.Images;
		State->ReportPortalImportUsage();
		return handle;
	}

	Renderer::Impl::ImportedPortalShadow *Renderer::Impl::FindPortalShadow(uint64_t handle) {
		if (!handle) return nullptr;
		for (auto &image : ImportedPortalShadows)
			if (image.Handle == handle) return &image;
		return nullptr;
	}
	const Renderer::Impl::ImportedPortalShadow *Renderer::Impl::FindPortalShadow(uint64_t handle) const {
		if (!handle) return nullptr;
		for (const auto &image : ImportedPortalShadows)
			if (image.Handle == handle) return &image;
		return nullptr;
	}

	bool Renderer::IsPortalShadowImageReady(uint64_t handle) const {
		RequireOwningThread("IsPortalShadowImageReady");
		const auto *image = State->FindPortalShadow(handle);
		return image && image->Ready;
	}
	bool Renderer::DropPortalShadowImage(uint64_t handle) {
		RequireOwningThread("DropPortalShadowImage");
		auto *image = State->FindPortalShadow(handle);
		if (!image) return false;
		State->ReleasePortalShadow(*image);
		return true;
	}
	void Renderer::Impl::ReleasePortalShadow(ImportedPortalShadow &image) {
		if (!image.Handle) return;
		gpu::ReleaseTexture(Device, image.Texture);
		gpu::ReleaseBuffer(Device, image.Packed);
		if (image.DedicatedStaging) PortalImportUsage.StagingBytes -= image.GpuBytes;
		gpu::ReleaseTransferBuffer(Device, image.Staging);
		PortalImportUsage.TextureBytes -= image.GpuBytes;
		PortalImportUsage.PendingCpuBytes -=
			image.Pending.capacity() + image.PackedPending.capacity() * sizeof(uint32_t);
		--PortalImportUsage.Images;
		image = {};
		if (!PortalImportUsage.Images) {
			ReleaseResidentImageCache();
			gpu::ReleaseTransferBuffer(Device, PortalImportStaging);
			PortalImportStaging = nullptr;
			PortalImportUsage.StagingBytes -= PortalImportStagingBytes;
			PortalImportStagingBytes = 0;
		}
		ReportPortalImportUsage();
	}

	bool Renderer::Impl::ValidPortalShadowView(const View &view, const glm::mat4 &lightProjection) const {
		const auto *image = FindPortalShadow(view.ImportedDirectionalShadow);
		if (!image || !view.DirectionalShadowBounds || image->Binding.World != view.World ||
			image->Binding.WorldName != view.WorldName || DepthFormat != SDL_GPU_TEXTUREFORMAT_D32_FLOAT)
			return false;
		const auto &snapshot = image->Binding.ExpectedSnapshot;
		if (BoundsOf(*view.DirectionalShadowBounds) != snapshot.DomainBounds) return false;
		for (size_t column = 0; column < 4; ++column)
			for (size_t row = 0; row < 4; ++row)
				if (lightProjection[column][row] != snapshot.LightViewProjection[column * 4 + row])
					return false;
		return true;
	}

	bool
	Renderer::Impl::RecordPortalShadowImport(SDL_GPUCommandBuffer *command, ImportedPortalShadow &image) {
		if (image.Ready || image.Recorded == command) return true;
		if (image.Recorded ||
			((!image.Texture || image.Pending.empty()) && (!image.Packed || image.PackedPending.empty())))
			return false;
		ENGINE_PROFILE("portal shadow upload");
		static_assert(PORTAL_SHADOW_BYTES <= MAX_IMPORTED_PORTAL_STAGING_BYTES);
		const size_t bytes = image.GpuBytes;
		if (bytes > MAX_IMPORTED_PORTAL_STAGING_BYTES) return false;
		SDL_GPUTransferBuffer *staging = image.Staging;
		if (image.DedicatedStaging && !staging) {
			if (bytes > MAX_IMPORTED_PORTAL_STAGING_BYTES - PortalImportUsage.StagingBytes) return false;
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = static_cast<uint32_t>(bytes);
			staging = image.Staging = gpu::CreateTransferBuffer(Device, &info);
			if (!staging) return false;
			PortalImportUsage.StagingBytes += bytes;
		}
		if (!image.DedicatedStaging) {
			if (PortalImportStagingBytes < bytes) {
				gpu::ReleaseTransferBuffer(Device, PortalImportStaging);
				PortalImportUsage.StagingBytes -= PortalImportStagingBytes;
				PortalImportStagingBytes = 0;
				SDL_GPUTransferBufferCreateInfo info{};
				info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
				info.size = static_cast<uint32_t>(bytes);
				PortalImportStaging = gpu::CreateTransferBuffer(Device, &info);
				if (!PortalImportStaging) return false;
				PortalImportStagingBytes = bytes;
				PortalImportUsage.StagingBytes += bytes;
			}
			staging = PortalImportStaging;
		}
		auto *mapped = static_cast<std::byte *>(SDL_MapGPUTransferBuffer(Device, staging, true));
		if (!mapped) return false;
		if (image.Packed)
			std::memcpy(mapped, image.PackedPending.data(), bytes);
		else {
			std::memcpy(mapped, image.Pending.data(), bytes);
			if constexpr (std::endian::native == std::endian::big)
				for (size_t offset = 0; offset < bytes; offset += 4)
					std::reverse(mapped + offset, mapped + offset + 4);
		}
		SDL_UnmapGPUTransferBuffer(Device, image.Staging);
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) return false;
		if (image.Packed) {
			const SDL_GPUTransferBufferLocation source{staging, 0};
			const SDL_GPUBufferRegion destination{image.Packed, 0, static_cast<uint32_t>(bytes)};
			SDL_UploadToGPUBuffer(copy, &source, &destination, false);
		} else {
			SDL_GPUTextureTransferInfo source{};
			source.transfer_buffer = staging;
			source.pixels_per_row = source.rows_per_layer = PORTAL_SHADOW_EXTENT;
			SDL_GPUTextureRegion destination{};
			destination.texture = image.Texture;
			destination.w = destination.h = PORTAL_SHADOW_EXTENT;
			destination.d = 1;
			SDL_UploadToGPUTexture(copy, &source, &destination, false);
		}
		SDL_EndGPUCopyPass(copy);
		image.Recorded = command;
		++PortalImportUsage.Uploads;
		PortalImportUsage.UploadedBytes += bytes;
		core::Metrics::Count("render.portal_import.uploads", 1);
		core::Metrics::Count("render.portal_import.upload_bytes", bytes);
		ReportPortalImportUsage();
		return true;
	}
	void Renderer::Impl::FinishPortalShadowImports(SDL_GPUCommandBuffer *command, bool submitted) {
		for (auto &image : ImportedPortalShadows) {
			if (!image.Recorded || (command && image.Recorded != command)) continue;
			image.Recorded = nullptr;
			image.Ready = submitted;
			if (!submitted) continue;
			PortalImportUsage.PendingCpuBytes -=
				image.Pending.capacity() + image.PackedPending.capacity() * sizeof(uint32_t);
			std::vector<std::byte>{}.swap(image.Pending);
			std::vector<uint32_t>{}.swap(image.PackedPending);
		}
	}
}
