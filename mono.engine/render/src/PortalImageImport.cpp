#include "RendererState.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PortalGeometry.hpp>
#include <engine/render/PortalImageRuntime.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace engine::render {
	namespace {
		std::atomic<uint64_t> NextPortalImageHandle{1};

		bool SameOwner(const PortalImageBinding &left, const PortalImageBinding &right) {
			return left.World == right.World && left.WorldName == right.WorldName &&
				   left.ViewSlot == right.ViewSlot && left.Portal == right.Portal &&
				   left.Index == right.Index && left.Layer == right.Layer;
		}

		bool ValidImage(const PortalImageBinding &binding, const PortalImageReply &reply) {
			if (binding.Layer > MAX_PORTAL_TRANSPARENT_LAYERS ||
				(binding.Layer != 0 &&
				 (binding.ExpectedScope != PortalImageScope::OpaqueLighting || reply.Depth.empty())) ||
				!binding.WorldName.IsValid() || !binding.Portal.IsValid() || binding.Index < 0 ||
				static_cast<size_t>(binding.Index) >= scene::MAX_SURFACES ||
				binding.Expected.RequestId == 0 || binding.Expected.PortalKey.empty() ||
				binding.Expected.PortalKey.size() > 256 ||
				binding.Expected.PortalKey.find('\0') != std::string::npos ||
				binding.Expected.PortalKey != binding.Portal.Text() || reply.Key != binding.Expected ||
				reply.Scope != binding.ExpectedScope || reply.Status != PortalImageStatus::Ok ||
				reply.Width == 0 || reply.Height == 0 || reply.Width > MAX_PORTAL_IMAGE_EXTENT ||
				reply.Height > MAX_PORTAL_IMAGE_EXTENT || reply.RowStride != reply.Width * 8 ||
				reply.Pixels.size() != size_t(reply.RowStride) * reply.Height ||
				reply.Pixels.capacity() > MAX_IMPORTED_PORTAL_STAGING_BYTES ||
				reply.Depth.capacity() > MAX_IMPORTED_PORTAL_STAGING_BYTES ||
				reply.Pixels.capacity() + reply.Depth.capacity() > MAX_IMPORTED_PORTAL_STAGING_BYTES ||
				(!reply.Depth.empty() && reply.Depth.size() != size_t(reply.Width) * reply.Height * 4) ||
				(reply.Depth.empty() && !reply.DepthHash.IsZero())) {
				return false;
			}
			for (int column = 0; column < 4; column++) {
				for (int row = 0; row < 4; row++) {
					if (!std::isfinite(binding.Sampling[column][row])) {
						return false;
					}
				}
			}
			const float determinant = glm::determinant(binding.Sampling);
			return std::isfinite(determinant) && determinant != 0;
		}
	}

	uint64_t Renderer::QueuePortalImage(const PortalImageBinding &binding, PortalImageReply &&reply) {
		RequireOwningThread("QueuePortalImage");
		if (State->Device == nullptr || !ValidImage(binding, reply)) {
			return 0;
		}
		Impl::ImportedPortalImage *selected = nullptr;
		for (auto &image : State->ImportedPortals) {
			if (image.LayerSet != 0) continue;
			if (image.Handle != 0 && SameOwner(image.Binding, binding)) {
				selected = &image;
				break;
			}
			if (image.Handle == 0 && selected == nullptr) {
				selected = &image;
			}
		}
		if (selected == nullptr || selected->Recorded != nullptr) {
			core::Metrics::Count("render.portal_import.refused", 1);
			return 0;
		}
		auto &image = *selected;
		const bool samePixels =
			image.Handle != 0 && image.Width == reply.Width && image.Height == reply.Height &&
			image.PixelHash == reply.PixelHash && image.DepthHash == reply.DepthHash &&
			(!reply.Depth.empty() ==
			 (!image.Pending.empty() ? !image.PendingDepth.empty() : image.DepthTexture != nullptr));
		if (samePixels) {
			const bool sameView = image.Binding.Sampling == binding.Sampling &&
								  image.Binding.Expected.CameraRevision == binding.Expected.CameraRevision &&
								  image.Binding.Expected.SeamRevision == binding.Expected.SeamRevision;
			image.Binding = binding;
			image.ContentRevision = reply.ContentRevision;
			image.LightingRevision = reply.LightingRevision;
			if (!sameView) {
				image.Handle = NextPortalImageHandle.fetch_add(1, std::memory_order_relaxed);
			}
			State->PortalImportUsage.Reuses++;
			core::Metrics::Count("render.portal_import.reuses", 1);
			return image.Handle;
		}
		const bool replacement = image.Texture == nullptr || image.TextureWidth != reply.Width ||
								 image.TextureHeight != reply.Height;
		const size_t texturePeak = State->PortalImportUsage.TextureBytes +
								   (replacement ? reply.Pixels.size() : 0) +
								   ((replacement || !image.DepthTexture) ? reply.Depth.size() : 0);
		const size_t cpuBytes = State->PortalImportUsage.PendingCpuBytes - image.Pending.capacity() -
								image.PendingDepth.capacity() + reply.Pixels.capacity() +
								reply.Depth.capacity();
		if (texturePeak > MAX_IMPORTED_PORTAL_TEXTURE_BYTES || cpuBytes > MAX_IMPORTED_PORTAL_CPU_BYTES) {
			core::Metrics::Count("render.portal_import.refused", 1);
			return 0;
		}
		// The transport validated these bytes already. This public ownership boundary
		// also rejects malformed direct callers before creating device resources.
		for (size_t offset = 0; offset < reply.Pixels.size(); offset += 2) {
			if ((std::to_integer<uint8_t>(reply.Pixels[offset + 1]) & 0x7c) == 0x7c) {
				return 0;
			}
		}
		if (assets::Hasher::Of(reply.Pixels) != reply.PixelHash) {
			return 0;
		}
		if (!reply.Depth.empty()) {
			core::ByteReader samples(reply.Depth);
			while (!samples.AtEnd()) {
				const float distance = samples.ReadFloat();
				if (!std::isfinite(distance) || std::signbit(distance)) return 0;
			}
			if (assets::Hasher::Of(reply.Depth) != reply.DepthHash) return 0;
		}
		if (image.Handle == 0) {
			State->PortalImportUsage.Images++;
		}
		image.Handle = NextPortalImageHandle.fetch_add(1, std::memory_order_relaxed);
		image.Binding = binding;
		image.PixelHash = reply.PixelHash;
		image.DepthHash = reply.DepthHash;
		image.ContentRevision = reply.ContentRevision;
		image.LightingRevision = reply.LightingRevision;
		image.Width = reply.Width;
		image.Height = reply.Height;
		image.Pending = std::move(reply.Pixels);
		image.PendingDepth = std::move(reply.Depth);
		image.Ready = false;
		State->PortalImportUsage.PendingCpuBytes = cpuBytes;
		State->ReportPortalImportUsage();
		return image.Handle;
	}

	bool Renderer::QueuePortalImageLayerSet(
		const PortalImageBinding &binding, PortalImageLayerSet &&layers, std::span<uint64_t> handles
	) {
		RequireOwningThread("QueuePortalImageLayerSet");
		ENGINE_PROFILE("portal layer import prepare");
		const size_t count = layers.Transparent.size() + 1;
		if (!State->Device || binding.Layer != 0 || handles.size() != count ||
			!ValidPortalImageLayerSet(layers))
			return false;
		std::array<Impl::ImportedPortalImage, MAX_PORTAL_TRANSPARENT_LAYERS + 1> prepared{};
		std::array<Impl::ImportedPortalImage *, MAX_PORTAL_TRANSPARENT_LAYERS + 1> destinations{};
		size_t free = 0;
		for (auto &image : State->ImportedPortals) {
			if (image.Handle != 0) continue;
			destinations[free++] = &image;
			if (free == count) break;
		}
		if (free != count) return false;
		size_t cpuBytes = 0, textureBytes = 0;
		for (size_t index = 0; index < count; ++index) {
			const auto &reply = index == 0 ? layers.Opaque : layers.Transparent[index - 1];
			auto &image = prepared[index];
			image.Binding = binding;
			image.Binding.Layer = static_cast<uint8_t>(index);
			if (!ValidImage(image.Binding, reply)) return false;
			image.Width = image.TextureWidth = reply.Width;
			image.Height = image.TextureHeight = reply.Height;
			image.PixelHash = reply.PixelHash;
			image.DepthHash = reply.DepthHash;
			image.ContentRevision = reply.ContentRevision;
			image.LightingRevision = reply.LightingRevision;
			image.TextureBytes = reply.Pixels.size() + reply.Depth.size();
			textureBytes += image.TextureBytes;
			cpuBytes += reply.Pixels.capacity() + reply.Depth.capacity();
		}
		if (textureBytes > MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes ||
			cpuBytes > MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes)
			return false;
		size_t reusable = 0;
		for (const auto &pair : State->ResidentImageCache) {
			if (pair.Colour && pair.Depth && pair.Width == layers.Opaque.Width &&
				pair.Height == layers.Opaque.Height)
				++reusable;
		}
		const size_t reusableBytes = std::min(reusable, count) * prepared[0].TextureBytes;
		if (State->PortalImportUsage.CachedTextureBytes > MAX_IMPORTED_PORTAL_TEXTURE_BYTES -
															  State->PortalImportUsage.TextureBytes -
															  (textureBytes - reusableBytes))
			State->ReleaseResidentImageCache();
		const auto releasePrepared = [&]() {
			for (auto &image : prepared) {
				gpu::ReleaseTexture(State->Device, image.Texture);
				gpu::ReleaseTexture(State->Device, image.DepthTexture);
			}
		};
		for (size_t index = 0; index < count; ++index) {
			auto &image = prepared[index];
			Impl::ResourceImageSlot cached;
			if (State->ReuseResidentImage(cached, image.Width, image.Height, true)) {
				image.Texture = cached.Resident;
				image.DepthTexture = cached.ResidentDepth;
				continue;
			}
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			info.width = image.Width;
			info.height = image.Height;
			info.layer_count_or_depth = info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			image.Texture = gpu::CreateTexture(State->Device, &info);
			info.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
			image.DepthTexture = gpu::CreateTexture(State->Device, &info);
			if (!image.Texture || !image.DepthTexture) {
				releasePrepared();
				return false;
			}
		}
		// All allocations succeeded. Move bytes and publish all ownership together.
		const uint64_t base = NextPortalImageHandle.fetch_add(count, std::memory_order_relaxed);
		for (size_t index = 0; index < count; ++index) {
			auto &reply = index == 0 ? layers.Opaque : layers.Transparent[index - 1];
			auto &image = prepared[index];
			image.Handle = base + index;
			image.LayerSet = base;
			image.LayerSetCount = static_cast<uint8_t>(count);
			image.Pending = std::move(reply.Pixels);
			image.PendingDepth = std::move(reply.Depth);
			*destinations[index] = std::move(image);
			handles[index] = base + index;
		}
		State->PortalImportUsage.Images += count;
		State->PortalImportUsage.TextureBytes += textureBytes;
		State->PortalImportUsage.PendingCpuBytes += cpuBytes;
		State->ReportPortalImportUsage();
		return true;
	}

	bool Renderer::PortalImageLayerSetReady(uint64_t base) const {
		RequireOwningThread("PortalImageLayerSetReady");
		if (base == 0) return false;
		size_t expected = 0, ready = 0;
		for (const auto &image : State->ImportedPortals) {
			if (image.LayerSet != base) continue;
			if (!image.Ready) return false;
			if (image.Handle == base) expected = image.LayerSetCount;
			++ready;
		}
		return expected != 0 && ready == expected;
	}

	uint64_t Renderer::AdoptResourceImage(uint64_t token, const PortalImageBinding &binding) {
		uint64_t handle = 0;
		AdoptResourceImages(std::span(&token, 1), std::span(&binding, 1), std::span(&handle, 1));
		return handle;
	}

	bool Renderer::AdoptResourceImages(
		std::span<const uint64_t> tokens,
		std::span<const PortalImageBinding> bindings,
		std::span<uint64_t> handles
	) {
		RequireOwningThread("AdoptResourceImages");
		if (State->Device == nullptr || tokens.empty() || tokens.size() > State->ResourceImages.size() ||
			tokens.size() != bindings.size() || tokens.size() != handles.size())
			return false;
		struct Adoption {
			Impl::ResourceImageSlot *Capture = nullptr;
			Impl::ImportedPortalImage *Destination = nullptr;
			PortalImageBinding Binding;
			size_t Bytes = 0;
			uint64_t Token = 0;
			uint64_t Handle = 0;
		};
		std::array<Adoption, 4> planned{};
		size_t totalBytes = 0;
		for (size_t index = 0; index < tokens.size(); ++index) {
			const auto &binding = bindings[index];
			if (binding.Layer > MAX_PORTAL_TRANSPARENT_LAYERS ||
				(binding.Layer != 0 && binding.ExpectedScope != PortalImageScope::OpaqueLighting) ||
				tokens[index] == 0 || !binding.WorldName.IsValid() || !binding.Portal.IsValid() ||
				binding.Index < 0 || size_t(binding.Index) >= scene::MAX_SURFACES ||
				binding.Expected.RequestId == 0 || binding.Expected.PortalKey != binding.Portal.Text())
				return false;
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					if (!std::isfinite(binding.Sampling[column][row])) return false;
			const float determinant = glm::determinant(binding.Sampling);
			if (!std::isfinite(determinant) || determinant == 0) return false;
			for (size_t previous = 0; previous < index; ++previous)
				if (tokens[previous] == tokens[index] || SameOwner(bindings[previous], binding)) return false;
			auto &adoption = planned[index];
			for (auto &slot : State->ResourceImages) {
				if (slot.Image.Request.Token == tokens[index] && !slot.Cancelled && slot.Resident &&
					slot.Image.Request.Delivery == ResourceImageDelivery::Resident &&
					slot.Image.Status == ResourceImageStatus::Ok && slot.Image.CaptureFrame != 0 &&
					(slot.Phase == Impl::ResourceImagePhase::Submitted ||
					 slot.Phase == Impl::ResourceImagePhase::Ready)) {
					adoption.Capture = &slot;
					break;
				}
			}
			if (!adoption.Capture) return false;
			if (binding.Layer != 0 && !adoption.Capture->ResidentDepth) return false;
			const auto &capture = adoption.Capture->Image;
			const auto &first = planned.front().Capture->Image;
			if (capture.CaptureFrame != first.CaptureFrame ||
				capture.Request.Pipeline != first.Request.Pipeline ||
				capture.Request.ViewSlot != first.Request.ViewSlot || capture.Width != first.Width ||
				capture.Height != first.Height)
				return false;
			for (auto &image : State->ImportedPortals) {
				if (image.LayerSet != 0) continue;
				if (image.Handle != 0 && SameOwner(image.Binding, binding)) {
					adoption.Destination = &image;
					break;
				}
				const bool reserved =
					std::any_of(planned.begin(), planned.begin() + index, [&](const Adoption &previous) {
						return previous.Destination == &image;
					});
				if (!image.Handle && !reserved && !adoption.Destination) adoption.Destination = &image;
			}
			if (!adoption.Destination || adoption.Destination->Recorded) return false;
			adoption.Binding = binding;
			adoption.Token = tokens[index];
			adoption.Bytes =
				size_t(capture.Width) * capture.Height * (adoption.Capture->ResidentDepth ? 12 : 8);
			totalBytes += adoption.Bytes;
		}
		if (totalBytes > MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes)
			return false;
		// Reserve the whole peak before moving any ownership, including replacements.
		if (State->PortalImportUsage.CachedTextureBytes >
			MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes - totalBytes)
			State->ReleaseResidentImageCache();
		for (size_t index = 0; index < tokens.size(); ++index) {
			auto &adoption = planned[index];
			auto &slot = *adoption.Capture;
			auto &image = *adoption.Destination;
			State->CacheResidentImage(image);
			State->ReleasePortalImport(image, true);
			image.Handle = NextPortalImageHandle.fetch_add(1, std::memory_order_relaxed);
			image.Binding = std::move(adoption.Binding);
			image.Texture = std::exchange(slot.Resident, nullptr);
			image.DepthTexture = std::exchange(slot.ResidentDepth, nullptr);
			image.Width = image.TextureWidth = slot.Image.Width;
			image.Height = image.TextureHeight = slot.Image.Height;
			image.TextureBytes = adoption.Bytes;
			image.Ready = true;
			State->PortalImportUsage.Images++;
			State->PortalImportUsage.TextureBytes += adoption.Bytes;
			adoption.Handle = image.Handle;
			CancelResourceImage(adoption.Token);
		}
		State->ReportPortalImportUsage();
		for (size_t index = 0; index < tokens.size(); ++index)
			handles[index] = planned[index].Handle;
		return true;
	}

	PortalImageImportUsage Renderer::PortalImageUsage() const {
		return State->PortalImportUsage;
	}

	bool Renderer::DropPortalImage(uint64_t handle) {
		RequireOwningThread("DropPortalImage");
		for (auto &image : State->ImportedPortals) {
			if (handle != 0 && image.Handle == handle) {
				const auto group = image.LayerSet;
				if (group == 0)
					State->ReleasePortalImport(image);
				else
					for (auto &member : State->ImportedPortals)
						if (member.LayerSet == group) {
							State->CacheResidentImage(member);
							State->ReleasePortalImport(member);
						}
				return true;
			}
		}
		return false;
	}

	void Renderer::Impl::CacheResidentImage(ImportedPortalImage &image) {
		if (!image.Texture) return;
		auto found = std::find_if(
			ResidentImageCache.begin(), ResidentImageCache.end(), [](const ResidentImagePair &pair) {
				return pair.Colour == nullptr;
			}
		);
		if (found == ResidentImageCache.end()) {
			found = ResidentImageCache.begin() + NextResidentCache;
			NextResidentCache = (NextResidentCache + 1) % ResidentImageCache.size();
			PortalImportUsage.CachedTextureBytes -=
				size_t(found->Width) * found->Height * (found->Depth ? 12 : 8);
			gpu::ReleaseTexture(Device, found->Colour);
			gpu::ReleaseTexture(Device, found->Depth);
		}
		*found = {
			std::exchange(image.Texture, nullptr),
			std::exchange(image.DepthTexture, nullptr),
			image.TextureWidth,
			image.TextureHeight
		};
		PortalImportUsage.CachedTextureBytes +=
			size_t(found->Width) * found->Height * (found->Depth ? 12 : 8);
	}

	void Renderer::Impl::ReportPortalImportUsage() {
		core::Metrics::SetGauge(
			"render.portal_import.cached_texture_bytes", double(PortalImportUsage.CachedTextureBytes)
		);
		core::Metrics::SetGauge("render.portal_import.images", double(PortalImportUsage.Images));
		core::Metrics::SetGauge(
			"render.portal_import.pending_cpu_bytes", double(PortalImportUsage.PendingCpuBytes)
		);
		core::Metrics::SetGauge("render.portal_import.texture_bytes", double(PortalImportUsage.TextureBytes));
		core::Metrics::SetGauge("render.portal_import.staging_bytes", double(PortalImportUsage.StagingBytes));
	}

	void Renderer::Impl::ReleasePortalImport(ImportedPortalImage &image, bool keepResidentCache) {
		if (image.Handle == 0) {
			return;
		}
		gpu::ReleaseTexture(Device, image.Texture);
		gpu::ReleaseTexture(Device, image.DepthTexture);
		PortalImportUsage.TextureBytes -= image.TextureBytes;
		PortalImportUsage.PendingCpuBytes -= image.Pending.capacity() + image.PendingDepth.capacity();
		PortalImportUsage.Images--;
		image = {};
		if (PortalImportUsage.Images == 0) {
			if (!keepResidentCache) ReleaseResidentImageCache();
			gpu::ReleaseTransferBuffer(Device, PortalImportStaging);
			PortalImportStaging = nullptr;
			PortalImportUsage.StagingBytes = 0;
		}
		ReportPortalImportUsage();
	}

	void
	Renderer::Impl::RecordPortalImports(SDL_GPUCommandBuffer *command, const View &view, size_t viewSlot) {
		ENGINE_PROFILE("portal image upload");
		for (auto &image : ImportedPortals) {
			if (image.Pending.empty() || image.Recorded != nullptr ||
				(image.LayerSet == 0 &&
				 (image.Binding.World != view.World || image.Binding.WorldName != view.WorldName ||
				  image.Binding.ViewSlot != viewSlot))) {
				continue;
			}
			const size_t bytes = image.Pending.size() + image.PendingDepth.size();
			const bool withDepth = !image.PendingDepth.empty();
			const uint32_t colorStride = (image.Width * 8 + 255) / 256 * 256;
			const uint32_t depthStride = (image.Width * 4 + 255) / 256 * 256;
			const uint32_t depthOffset = (colorStride * image.Height + 511) / 512 * 512;
			const uint32_t stagingBytes =
				withDepth ? depthOffset + depthStride * image.Height : colorStride * image.Height;
			if (stagingBytes > MAX_IMPORTED_PORTAL_STAGING_BYTES) continue;
			if (PortalImportUsage.StagingBytes < stagingBytes) {
				gpu::ReleaseTransferBuffer(Device, PortalImportStaging);
				PortalImportStaging = nullptr;
				PortalImportUsage.StagingBytes = 0;
				SDL_GPUTransferBufferCreateInfo info{};
				info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
				info.size = stagingBytes;
				PortalImportStaging = gpu::CreateTransferBuffer(Device, &info);
				if (PortalImportStaging == nullptr) continue;
				PortalImportUsage.StagingBytes = stagingBytes;
			}
			const bool resized = image.TextureWidth != image.Width || image.TextureHeight != image.Height;
			const bool replaceColor = !image.Texture || resized;
			const bool replaceDepth = withDepth && (!image.DepthTexture || resized);
			const size_t allocationBytes =
				(replaceColor ? image.Pending.size() : 0) + (replaceDepth ? image.PendingDepth.size() : 0);
			if (allocationBytes > MAX_IMPORTED_PORTAL_TEXTURE_BYTES - PortalImportUsage.TextureBytes)
				continue;
			if (PortalImportUsage.CachedTextureBytes >
				MAX_IMPORTED_PORTAL_TEXTURE_BYTES - PortalImportUsage.TextureBytes - allocationBytes)
				ReleaseResidentImageCache();
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			info.width = image.Width;
			info.height = image.Height;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			auto *color = replaceColor ? gpu::CreateTexture(Device, &info) : image.Texture;
			if (!color) continue;
			info.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
			auto *depth = replaceDepth ? gpu::CreateTexture(Device, &info) : image.DepthTexture;
			if (withDepth && !depth) {
				if (replaceColor) gpu::ReleaseTexture(Device, color);
				continue;
			}
			// Both allocations succeed before either old attachment is released.
			if (replaceColor) gpu::ReleaseTexture(Device, image.Texture);
			if (replaceDepth || !withDepth) gpu::ReleaseTexture(Device, image.DepthTexture);
			image.Texture = color;
			image.DepthTexture = withDepth ? depth : nullptr;
			PortalImportUsage.TextureBytes = PortalImportUsage.TextureBytes - image.TextureBytes + bytes;
			image.TextureBytes = bytes;
			image.TextureWidth = image.Width;
			image.TextureHeight = image.Height;
			auto *mapped =
				static_cast<std::byte *>(SDL_MapGPUTransferBuffer(Device, PortalImportStaging, true));
			if (!mapped) continue;
			for (uint32_t row = 0; row < image.Height; ++row) {
				std::memcpy(
					mapped + size_t(row) * colorStride,
					image.Pending.data() + size_t(row) * image.Width * 8,
					size_t(image.Width) * 8
				);
				if (withDepth)
					std::memcpy(
						mapped + depthOffset + size_t(row) * depthStride,
						image.PendingDepth.data() + size_t(row) * image.Width * 4,
						size_t(image.Width) * 4
					);
				if constexpr (std::endian::native == std::endian::big) {
					for (size_t x = 0; x < size_t(image.Width) * 8; x += 2)
						std::swap(
							mapped[size_t(row) * colorStride + x], mapped[size_t(row) * colorStride + x + 1]
						);
					if (withDepth)
						for (size_t x = 0; x < size_t(image.Width) * 4; x += 4) {
							auto *sample = mapped + depthOffset + size_t(row) * depthStride + x;
							std::swap(sample[0], sample[3]);
							std::swap(sample[1], sample[2]);
						}
				}
			}
			SDL_UnmapGPUTransferBuffer(Device, PortalImportStaging);
			auto *copy = SDL_BeginGPUCopyPass(command);
			if (!copy) continue;
			SDL_GPUTextureTransferInfo from{};
			from.transfer_buffer = PortalImportStaging;
			from.pixels_per_row = colorStride / 8;
			from.rows_per_layer = image.Height;
			SDL_GPUTextureRegion to{};
			to.texture = image.Texture;
			to.w = image.Width;
			to.h = image.Height;
			to.d = 1;
			SDL_UploadToGPUTexture(copy, &from, &to, true);
			if (withDepth) {
				from.offset = depthOffset;
				from.pixels_per_row = depthStride / 4;
				to.texture = image.DepthTexture;
				SDL_UploadToGPUTexture(copy, &from, &to, true);
			}
			SDL_EndGPUCopyPass(copy);
			image.Recorded = command;
			PortalImportUsage.Uploads += withDepth ? 2 : 1;
			PortalImportUsage.UploadedBytes += bytes;
			core::Metrics::Count("render.portal_import.uploads", withDepth ? 2 : 1);
			core::Metrics::Count("render.portal_import.upload_bytes", double(bytes));
		}
		ReportPortalImportUsage();
	}

	void Renderer::Impl::FinishPortalImports(SDL_GPUCommandBuffer *command, bool submitted) {
		for (auto &image : ImportedPortals) {
			if (image.Recorded == nullptr || (command != nullptr && image.Recorded != command)) {
				continue;
			}
			image.Recorded = nullptr;
			image.Ready = submitted;
			if (submitted) {
				PortalImportUsage.PendingCpuBytes -= image.Pending.capacity() + image.PendingDepth.capacity();
				std::vector<std::byte>{}.swap(image.Pending);
				std::vector<std::byte>{}.swap(image.PendingDepth);
			}
		}
		ReportPortalImportUsage();
	}

	uint64_t Renderer::ComposePortalBodyImage(const PortalImageCapture &capture, const View &body) {
		RequireOwningThread("ComposePortalBodyImage");
		ENGINE_PROFILE("compose current portal body");
		if (!State->Device || !body.Target || !capture.CaptureLighting ||
			capture.Binding.ExpectedScope != PortalImageScope::OpaqueLighting || capture.Binding.Layer != 0 ||
			body.World != capture.Binding.World || body.WorldName != capture.Binding.WorldName ||
			body.Slot != capture.Binding.ViewSlot || !PortalImageLayerSetReady(capture.Image))
			return 0;
		if (body.Instances.size() > MAX_PORTAL_GEOMETRY_ROWS ||
			body.JointFrames.size() > MAX_PORTAL_GEOMETRY_JOINTS ||
			std::any_of(body.Instances.begin(), body.Instances.end(), [](const auto &row) {
				return row.Transparency != 0 || row.Alpha == scene::AlphaMode::Transparency ||
					   row.Surface >= 0 || row.Shader.IsValid();
			}))
			return 0;
		const std::array<uint64_t, 3> handles{
			capture.Image, capture.TransparentImages[0], capture.TransparentImages[1]
		};
		for (size_t layer = 0; layer < handles.size(); ++layer) {
			const auto image = std::find_if(
				State->ImportedPortals.begin(), State->ImportedPortals.end(), [&](const auto &entry) {
					return entry.Handle == handles[layer];
				}
			);
			auto binding = capture.Binding;
			binding.Layer = static_cast<uint8_t>(layer);
			if (image == State->ImportedPortals.end() || image->LayerSet != capture.Image ||
				image->LayerSetCount != handles.size() || !SameOwner(image->Binding, binding) ||
				image->Binding.Expected != binding.Expected ||
				image->Binding.ExpectedProjection != binding.ExpectedProjection ||
				image->Binding.Sampling != binding.Sampling || image->Width != capture.Width ||
				image->Height != capture.Height)
				return 0;
		}
		auto view = body;
		std::array<SceneLight, MAX_PORTAL_CAPTURE_LIGHTS> lights{};
		if (!ResolvePortalCaptureCamera(capture.Camera, capture.Binding.ExpectedProjection, view) ||
			!ResolvePortalCaptureLighting(*capture.CaptureLighting, view, lights))
			return 0;
		const bool seam = capture.Binding.ExpectedProjection == PortalImageProjection::Seam;
		const core::Name pipelineName(
			std::string(seam ? "portal-body-seam-image/" : "portal-body-eye-image/") +
			std::to_string(body.Slot)
		);
		if (std::none_of(
				State->NamedPipelines.begin(), State->NamedPipelines.end(), [&](const auto &pipeline) {
					return pipeline.Name == pipelineName;
				}
			)) {
			graph::PipelineDocument document;
			const auto base = graph::DefaultPortalBodyDocument(seam, true);
			for (const auto &edit : base.Edits()) {
				document.Record(edit);
				if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("export"))
					document.Record(
						{.Kind = graph::EditKind::Set,
						 .Key = core::Name("view"),
						 .Value = std::to_string(body.Slot)}
					);
			}
			graph::RenderGraph pipeline;
			core::Name offender;
			if (graph::Build(document, pipeline, offender) != graph::PipelineDocumentStatus::Ok ||
				!SetPipeline(pipelineName, pipeline))
				return 0;
		}
		view.Pipeline = pipelineName;
		view.Damage = {.Scene = true, .Objects = true, .Environment = true, .Portals = true};
		view.EyeImage = capture.Image;
		view.EyeImageKey = capture.Binding.Portal;
		view.EyeTransparentImages = capture.TransparentImages;
		const auto token = QueueResourceImage(
			pipelineName, core::Name("export"), body.Slot, ResourceImageDelivery::Resident
		);
		if (token == 0) return 0;
		OverlayImage overlay;
		const auto rendered = Render(std::span(&view, 1), overlay, nullptr, false);
		const auto image =
			rendered.Ran(core::Name("export")) ? AdoptResourceImage(token, capture.Binding) : 0;
		CancelResourceImage(token);
		// This subset replaced the slot's source rows, even when the parent world is unchanged.
		if (body.Slot < State->SceneSlots.size()) State->SceneSlots[body.Slot].InstanceSourcesReady = false;
		return image;
	}

	Renderer::Impl::ImportedPortalImage *Renderer::Impl::FindPortalImport(
		const View &view, size_t viewSlot, const PortalView &portal, SDL_GPUCommandBuffer *command
	) {
		for (auto &image : ImportedPortals) {
			if (portal.ExternalImage && portal.ImportedImage != 0 && image.Handle == portal.ImportedImage &&
				image.Binding.World == view.World && image.Binding.WorldName == view.WorldName &&
				image.Binding.ViewSlot == viewSlot && image.Binding.Index == portal.Index &&
				image.Binding.Portal == portal.ImagePortal && image.Binding.Layer == 0 &&
				image.Texture != nullptr && (image.Ready || image.Recorded == command)) {
				return &image;
			}
		}
		return nullptr;
	}
}
