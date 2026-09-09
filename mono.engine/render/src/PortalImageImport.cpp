#include "PortalImageSamples.hpp"
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
		constexpr uint8_t SPATIAL_OVERLAY_LAYER = MAX_PORTAL_TRANSPARENT_LAYERS + 1;

		bool SameOwner(const PortalImageBinding &left, const PortalImageBinding &right) {
			return left.World == right.World && left.WorldName == right.WorldName &&
				   left.ViewSlot == right.ViewSlot && left.Portal == right.Portal &&
				   left.Index == right.Index && left.Layer == right.Layer;
		}

		bool ValidImage(const PortalImageBinding &binding, const PortalImageReply &reply) {
			const size_t pixels = size_t(reply.Width) * reply.Height;
			const bool ambient = !reply.Normal.empty();
			const bool directional = !reply.DirectionalResponse.empty();
			if ((directional &&
				 (binding.Layer != 0 || !ambient || reply.DirectionalResponse.size() != pixels * 16)) ||
				(!directional && !reply.DirectionalResponseHash.IsZero()) ||
				reply.DirectionalResponse.capacity() > MAX_IMPORTED_PORTAL_STAGING_BYTES)
				return false;
			if (ambient != !reply.AmbientResponse.empty() || ambient != !reply.LightingBaseline.empty() ||
				(ambient && (reply.Depth.empty() || reply.Normal.size() != pixels * 4 ||
							 reply.AmbientResponse.size() != pixels * 16 ||
							 reply.LightingBaseline.size() != pixels * 16)) ||
				(!ambient && (!reply.NormalHash.IsZero() || !reply.AmbientResponseHash.IsZero() ||
							  !reply.LightingBaselineHash.IsZero())) ||
				reply.Normal.capacity() > MAX_IMPORTED_PORTAL_STAGING_BYTES ||
				reply.AmbientResponse.capacity() > MAX_IMPORTED_PORTAL_STAGING_BYTES ||
				reply.LightingBaseline.capacity() > MAX_IMPORTED_PORTAL_STAGING_BYTES ||
				reply.Pixels.capacity() + reply.Depth.capacity() + reply.Normal.capacity() +
						reply.AmbientResponse.capacity() + reply.LightingBaseline.capacity() +
						reply.DirectionalResponse.capacity() >
					MAX_IMPORTED_PORTAL_STAGING_BYTES)
				return false;
			if (directional &&
				(assets::Hasher::Of(reply.DirectionalResponse) != reply.DirectionalResponseHash ||
				 !ValidPortalResponseSamples(reply.DirectionalResponse)))
				return false;
			if (ambient) {
				if (reply.Scope != PortalImageScope::OpaqueLighting || !reply.CaptureLighting) return false;
				if (assets::Hasher::Of(reply.Normal) != reply.NormalHash ||
					assets::Hasher::Of(reply.AmbientResponse) != reply.AmbientResponseHash ||
					assets::Hasher::Of(reply.LightingBaseline) != reply.LightingBaselineHash)
					return false;
				if (!ValidPortalBaselineSamples(reply.LightingBaseline) ||
					!ValidPortalResponseSamples(reply.AmbientResponse))
					return false;
			}
			if (binding.Layer > SPATIAL_OVERLAY_LAYER ||
				(binding.Layer != 0 && (binding.ExpectedScope != PortalImageScope::OpaqueLighting ||
										(reply.Depth.empty() != (binding.Layer == SPATIAL_OVERLAY_LAYER)))) ||
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
		if (State->Device == nullptr || binding.Layer == SPATIAL_OVERLAY_LAYER ||
			!ValidImage(binding, reply)) {
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
			image.NormalHash == reply.NormalHash && image.AmbientResponseHash == reply.AmbientResponseHash &&
			image.LightingBaselineHash == reply.LightingBaselineHash &&
			image.DirectionalResponseHash == reply.DirectionalResponseHash &&
			(!reply.Depth.empty() ==
			 (!image.Pending.empty() ? !image.PendingDepth.empty() : image.DepthTexture != nullptr));
		if (samePixels) {
			const bool sameView = image.Binding.Sampling == binding.Sampling &&
								  image.Binding.Expected.CameraRevision == binding.Expected.CameraRevision &&
								  image.Binding.Expected.SeamRevision == binding.Expected.SeamRevision;
			image.Binding = binding;
			image.CaptureTick = reply.CaptureTick;
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
		const size_t texturePeak =
			State->PortalImportUsage.TextureBytes + (replacement ? reply.Pixels.size() : 0) +
			((replacement || !image.DepthTexture) ? reply.Depth.size() : 0) +
			((replacement || !image.NormalTexture) ? reply.Normal.size() : 0) +
			((replacement || !image.AmbientResponseTexture) ? reply.AmbientResponse.size() : 0) +
			((replacement || !image.LightingBaselineTexture) ? reply.LightingBaseline.size() : 0) +
			((replacement || !image.DirectionalResponseTexture) ? reply.DirectionalResponse.size() : 0);
		const size_t cpuBytes =
			State->PortalImportUsage.PendingCpuBytes - image.Pending.capacity() -
			image.PendingDepth.capacity() - image.PendingNormal.capacity() -
			image.PendingAmbientResponse.capacity() -
			(image.PendingLightingBaseline.capacity() + image.PendingDirectionalResponse.capacity()) +
			reply.Pixels.capacity() + reply.Depth.capacity() + reply.Normal.capacity() +
			reply.AmbientResponse.capacity() + reply.LightingBaseline.capacity() +
			reply.DirectionalResponse.capacity();
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
			if (!ValidPortalDepthSamples(reply.Depth)) return 0;
			if (assets::Hasher::Of(reply.Depth) != reply.DepthHash) return 0;
		}
		if (image.Handle == 0) {
			State->PortalImportUsage.Images++;
		}
		image.Handle = NextPortalImageHandle.fetch_add(1, std::memory_order_relaxed);
		image.Binding = binding;
		image.PixelHash = reply.PixelHash;
		image.DepthHash = reply.DepthHash;
		image.NormalHash = reply.NormalHash;
		image.AmbientResponseHash = reply.AmbientResponseHash;
		image.LightingBaselineHash = reply.LightingBaselineHash;
		image.DirectionalResponseHash = reply.DirectionalResponseHash;
		image.CaptureTick = reply.CaptureTick;
		image.ContentRevision = reply.ContentRevision;
		image.LightingRevision = reply.LightingRevision;
		image.Width = reply.Width;
		image.Height = reply.Height;
		image.Pending = std::move(reply.Pixels);
		image.PendingDepth = std::move(reply.Depth);
		image.PendingNormal = std::move(reply.Normal);
		image.PendingAmbientResponse = std::move(reply.AmbientResponse);
		image.PendingLightingBaseline = std::move(reply.LightingBaseline);
		image.PendingDirectionalResponse = std::move(reply.DirectionalResponse);
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
		const size_t pairedCount = layers.Transparent.size() + 1;
		const size_t count = pairedCount + layers.SpatialOverlay.has_value();
		if (!State->Device || binding.Layer != 0 || handles.size() != count ||
			!ValidPortalImageLayerSet(layers))
			return false;
		std::array<Impl::ImportedPortalImage, MAX_PORTAL_TRANSPARENT_LAYERS + 2> prepared{};
		std::array<Impl::ImportedPortalImage *, MAX_PORTAL_TRANSPARENT_LAYERS + 2> destinations{};
		const auto member = [&](size_t index) -> PortalImageReply & {
			if (index == 0) return layers.Opaque;
			if (index < pairedCount) return layers.Transparent[index - 1];
			return *layers.SpatialOverlay;
		};
		size_t free = 0;
		for (auto &image : State->ImportedPortals) {
			if (image.Handle != 0) continue;
			destinations[free++] = &image;
			if (free == count) break;
		}
		if (free != count) return false;
		size_t cpuBytes = 0, textureBytes = 0;
		for (size_t index = 0; index < count; ++index) {
			const auto &reply = member(index);
			auto &image = prepared[index];
			image.Binding = binding;
			image.Binding.Layer = index < pairedCount ? static_cast<uint8_t>(index) : SPATIAL_OVERLAY_LAYER;
			if (!ValidImage(image.Binding, reply)) return false;
			image.Width = image.TextureWidth = reply.Width;
			image.Height = image.TextureHeight = reply.Height;
			image.PixelHash = reply.PixelHash;
			image.DepthHash = reply.DepthHash;
			image.NormalHash = reply.NormalHash;
			image.AmbientResponseHash = reply.AmbientResponseHash;
			image.LightingBaselineHash = reply.LightingBaselineHash;
			image.DirectionalResponseHash = reply.DirectionalResponseHash;
			image.CaptureTick = reply.CaptureTick;
			image.ContentRevision = reply.ContentRevision;
			image.LightingRevision = reply.LightingRevision;
			image.TextureBytes = reply.Pixels.size() + reply.Depth.size() + reply.Normal.size() +
								 reply.AmbientResponse.size() + reply.LightingBaseline.size() +
								 reply.DirectionalResponse.size();
			textureBytes += image.TextureBytes;
			cpuBytes += reply.Pixels.capacity() + reply.Depth.capacity() + reply.Normal.capacity() +
						reply.AmbientResponse.capacity() + reply.LightingBaseline.capacity() +
						reply.DirectionalResponse.capacity();
		}
		if (textureBytes > MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes ||
			cpuBytes > MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes)
			return false;
		if (State->PortalImportUsage.CachedTextureBytes >
			MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes - textureBytes)
			State->ReleaseResidentImageCache();
		const auto releasePrepared = [&]() {
			for (auto &image : prepared) {
				gpu::ReleaseTexture(State->Device, image.Texture);
				gpu::ReleaseTexture(State->Device, image.DepthTexture);
				gpu::ReleaseTexture(State->Device, image.NormalTexture);
				gpu::ReleaseTexture(State->Device, image.AmbientResponseTexture);
				gpu::ReleaseTexture(State->Device, image.LightingBaselineTexture);
				gpu::ReleaseTexture(State->Device, image.DirectionalResponseTexture);
			}
		};
		for (size_t index = 0; index < count; ++index) {
			auto &image = prepared[index];
			const bool paired = index < pairedCount;
			const bool ambient = !member(index).Normal.empty();
			const bool directional = !member(index).DirectionalResponse.empty();
			Impl::ResourceImageSlot cached;
			if (State->ReuseResidentImage(cached, image.Width, image.Height, paired, ambient, directional)) {
				image.Texture = cached.Resident;
				image.DepthTexture = cached.ResidentDepth;
				image.NormalTexture = cached.ResidentNormal;
				image.AmbientResponseTexture = cached.ResidentAmbientResponse;
				image.LightingBaselineTexture = cached.ResidentLightingBaseline;
				image.DirectionalResponseTexture = cached.ResidentDirectionalResponse;
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
			if (paired) image.DepthTexture = gpu::CreateTexture(State->Device, &info);
			if (ambient) {
				info.format = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
				image.NormalTexture = gpu::CreateTexture(State->Device, &info);
				info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
				image.AmbientResponseTexture = gpu::CreateTexture(State->Device, &info);
				image.LightingBaselineTexture = gpu::CreateTexture(State->Device, &info);
				if (directional) image.DirectionalResponseTexture = gpu::CreateTexture(State->Device, &info);
			}
			if (!image.Texture || (directional && !image.DirectionalResponseTexture) ||
				(paired && !image.DepthTexture) ||
				(ambient &&
				 (!image.NormalTexture || !image.AmbientResponseTexture || !image.LightingBaselineTexture))) {
				releasePrepared();
				return false;
			}
		}
		// All allocations succeeded. Move bytes and publish all ownership together.
		const uint64_t base = NextPortalImageHandle.fetch_add(count, std::memory_order_relaxed);
		for (size_t index = 0; index < count; ++index) {
			auto &reply = member(index);
			auto &image = prepared[index];
			image.Handle = base + index;
			image.LayerSet = base;
			image.LayerSetCount = static_cast<uint8_t>(count);
			image.Pending = std::move(reply.Pixels);
			image.PendingDepth = std::move(reply.Depth);
			image.PendingNormal = std::move(reply.Normal);
			image.PendingAmbientResponse = std::move(reply.AmbientResponse);
			image.PendingLightingBaseline = std::move(reply.LightingBaseline);
			image.PendingDirectionalResponse = std::move(reply.DirectionalResponse);
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
		return AdoptResourceImagesInternal(tokens, bindings, handles, true);
	}

	bool Renderer::AdoptResourceImagesInternal(
		std::span<const uint64_t> tokens,
		std::span<const PortalImageBinding> bindings,
		std::span<uint64_t> handles,
		bool replaceExisting
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
		std::array<Adoption, Impl::RESOURCE_IMAGE_CAPACITY> planned{};
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
				if (replaceExisting && image.Handle != 0 && SameOwner(image.Binding, binding)) {
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
			adoption.Bytes = size_t(capture.Width) * capture.Height *
							 (adoption.Capture->ResidentDirectionalResponse ? 64
							  : adoption.Capture->ResidentNormal			? 48
							  : adoption.Capture->ResidentDepth				? 12
																			: 8);
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
			// A local GPU frame has no authenticated producer simulation tick.
			image.CaptureTick.reset();
			image.Texture = std::exchange(slot.Resident, nullptr);
			image.DepthTexture = std::exchange(slot.ResidentDepth, nullptr);
			image.NormalTexture = std::exchange(slot.ResidentNormal, nullptr);
			image.AmbientResponseTexture = std::exchange(slot.ResidentAmbientResponse, nullptr);
			image.LightingBaselineTexture = std::exchange(slot.ResidentLightingBaseline, nullptr);
			image.DirectionalResponseTexture = std::exchange(slot.ResidentDirectionalResponse, nullptr);
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
		if (handle != 0)
			for (const auto &tree : State->ImportedPortalTrees)
				if (tree.Token != 0)
					for (const auto &node : tree.Nodes)
						if (std::find(node.Images.begin(), node.Images.end(), handle) != node.Images.end()) {
							DropPortalCaptureTree(tree.Token);
							return true;
						}
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
			PortalImportUsage.CachedTextureBytes -= size_t(found->Width) * found->Height *
													(found->DirectionalResponse ? 64
													 : found->Normal			? 48
													 : found->Depth				? 12
																				: 8);
			gpu::ReleaseTexture(Device, found->Colour);
			gpu::ReleaseTexture(Device, found->Depth);
			gpu::ReleaseTexture(Device, found->Normal);
			gpu::ReleaseTexture(Device, found->AmbientResponse);
			gpu::ReleaseTexture(Device, found->LightingBaseline);
			gpu::ReleaseTexture(Device, found->DirectionalResponse);
		}
		*found = {
			std::exchange(image.Texture, nullptr),
			std::exchange(image.DepthTexture, nullptr),
			std::exchange(image.NormalTexture, nullptr),
			std::exchange(image.AmbientResponseTexture, nullptr),
			std::exchange(image.LightingBaselineTexture, nullptr),
			std::exchange(image.DirectionalResponseTexture, nullptr),
			image.TextureWidth,
			image.TextureHeight
		};
		PortalImportUsage.CachedTextureBytes += size_t(found->Width) * found->Height *
												(found->DirectionalResponse ? 64
												 : found->Normal			? 48
												 : found->Depth				? 12
																			: 8);
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
		gpu::ReleaseTexture(Device, image.NormalTexture);
		gpu::ReleaseTexture(Device, image.AmbientResponseTexture);
		gpu::ReleaseTexture(Device, image.LightingBaselineTexture);
		gpu::ReleaseTexture(Device, image.DirectionalResponseTexture);
		PortalImportUsage.TextureBytes -= image.TextureBytes;
		PortalImportUsage.PendingCpuBytes -=
			image.Pending.capacity() + image.PendingDepth.capacity() + image.PendingNormal.capacity() +
			image.PendingAmbientResponse.capacity() +
			(image.PendingLightingBaseline.capacity() + image.PendingDirectionalResponse.capacity());
		PortalImportUsage.Images--;
		image = {};
		if (PortalImportUsage.Images == 0) {
			if (!keepResidentCache) ReleaseResidentImageCache();
			gpu::ReleaseTransferBuffer(Device, PortalImportStaging);
			PortalImportStaging = nullptr;
			PortalImportUsage.StagingBytes -= PortalImportStagingBytes;
			PortalImportStagingBytes = 0;
		}
		ReportPortalImportUsage();
	}

	void
	Renderer::Impl::RecordPortalImports(SDL_GPUCommandBuffer *command, const View &view, size_t viewSlot) {
		ENGINE_PROFILE("portal image upload");
		for (auto &image : ImportedPortals) {
			if (image.Pending.empty() || image.Recorded ||
				(image.LayerSet == 0 &&
				 (image.Binding.World != view.World || image.Binding.WorldName != view.WorldName ||
				  image.Binding.ViewSlot != viewSlot)))
				continue;
			const std::array<const std::vector<std::byte> *, 6> planes{
				&image.Pending,
				&image.PendingDepth,
				&image.PendingNormal,
				&image.PendingAmbientResponse,
				&image.PendingLightingBaseline,
				&image.PendingDirectionalResponse
			};
			const std::array<SDL_GPUTexture **, 6> destinations{
				&image.Texture,
				&image.DepthTexture,
				&image.NormalTexture,
				&image.AmbientResponseTexture,
				&image.LightingBaselineTexture,
				&image.DirectionalResponseTexture
			};
			const std::array<uint32_t, 6> pixelBytes{8, 4, 4, 16, 16, 16};
			const std::array<SDL_GPUTextureFormat, 6> formats{
				SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
				SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
				SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM,
				SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
				SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
				SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT
			};
			std::array<uint32_t, 6> strides{}, offsets{};
			std::array<bool, 6> replace{};
			size_t bytes = 0, allocationBytes = 0;
			uint32_t stagingBytes = 0, count = 0;
			const bool resized = image.TextureWidth != image.Width || image.TextureHeight != image.Height;
			for (size_t plane = 0; plane < planes.size(); ++plane) {
				if (planes[plane]->empty()) continue;
				++count;
				bytes += planes[plane]->size();
				strides[plane] = (image.Width * pixelBytes[plane] + 255) / 256 * 256;
				offsets[plane] = (stagingBytes + 511) / 512 * 512;
				stagingBytes = offsets[plane] + strides[plane] * image.Height;
				replace[plane] = !*destinations[plane] || resized;
				if (replace[plane]) allocationBytes += planes[plane]->size();
			}
			if (stagingBytes > MAX_IMPORTED_PORTAL_STAGING_BYTES ||
				allocationBytes > MAX_IMPORTED_PORTAL_TEXTURE_BYTES - PortalImportUsage.TextureBytes)
				continue;
			if (PortalImportStagingBytes < stagingBytes) {
				gpu::ReleaseTransferBuffer(Device, PortalImportStaging);
				PortalImportStaging = nullptr;
				PortalImportUsage.StagingBytes -= PortalImportStagingBytes;
				PortalImportStagingBytes = 0;
				SDL_GPUTransferBufferCreateInfo info{};
				info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
				info.size = stagingBytes;
				PortalImportStaging = gpu::CreateTransferBuffer(Device, &info);
				if (!PortalImportStaging) continue;
				PortalImportStagingBytes = stagingBytes;
				PortalImportUsage.StagingBytes += stagingBytes;
			}
			if (PortalImportUsage.CachedTextureBytes >
				MAX_IMPORTED_PORTAL_TEXTURE_BYTES - PortalImportUsage.TextureBytes - allocationBytes)
				ReleaseResidentImageCache();
			std::array<SDL_GPUTexture *, 6> textures{};
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			info.width = image.Width;
			info.height = image.Height;
			info.layer_count_or_depth = info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			bool allocated = true;
			for (size_t plane = 0; plane < planes.size(); ++plane) {
				if (planes[plane]->empty()) continue;
				info.format = formats[plane];
				textures[plane] = replace[plane] ? gpu::CreateTexture(Device, &info) : *destinations[plane];
				if (!textures[plane]) {
					allocated = false;
					break;
				}
			}
			if (!allocated) {
				for (size_t plane = 0; plane < planes.size(); ++plane)
					if (replace[plane]) gpu::ReleaseTexture(Device, textures[plane]);
				continue;
			}
			// The complete attachment set exists before replacing any old plane.
			for (size_t plane = 0; plane < planes.size(); ++plane) {
				if (replace[plane] || planes[plane]->empty())
					gpu::ReleaseTexture(Device, *destinations[plane]);
				*destinations[plane] = textures[plane];
			}
			PortalImportUsage.TextureBytes = PortalImportUsage.TextureBytes - image.TextureBytes + bytes;
			image.TextureBytes = bytes;
			image.TextureWidth = image.Width;
			image.TextureHeight = image.Height;
			auto *mapped =
				static_cast<std::byte *>(SDL_MapGPUTransferBuffer(Device, PortalImportStaging, true));
			if (!mapped) continue;
			for (size_t plane = 0; plane < planes.size(); ++plane) {
				if (planes[plane]->empty()) continue;
				const size_t rowBytes = size_t(image.Width) * pixelBytes[plane];
				for (uint32_t row = 0; row < image.Height; ++row) {
					auto *destination = mapped + offsets[plane] + size_t(row) * strides[plane];
					std::memcpy(destination, planes[plane]->data() + row * rowBytes, rowBytes);
					if constexpr (std::endian::native == std::endian::big) {
						const size_t wordBytes = plane == 0 ? 2 : 4;
						for (size_t offset = 0; offset < rowBytes; offset += wordBytes)
							std::reverse(destination + offset, destination + offset + wordBytes);
					}
				}
			}
			SDL_UnmapGPUTransferBuffer(Device, PortalImportStaging);
			auto *copy = SDL_BeginGPUCopyPass(command);
			if (!copy) continue;
			for (size_t plane = 0; plane < planes.size(); ++plane) {
				if (planes[plane]->empty()) continue;
				SDL_GPUTextureTransferInfo from{};
				from.transfer_buffer = PortalImportStaging;
				from.offset = offsets[plane];
				from.pixels_per_row = strides[plane] / pixelBytes[plane];
				from.rows_per_layer = image.Height;
				SDL_GPUTextureRegion to{};
				to.texture = textures[plane];
				to.w = image.Width;
				to.h = image.Height;
				to.d = 1;
				SDL_UploadToGPUTexture(copy, &from, &to, true);
			}
			SDL_EndGPUCopyPass(copy);
			image.Recorded = command;
			PortalImportUsage.Uploads += count;
			PortalImportUsage.UploadedBytes += bytes;
			core::Metrics::Count("render.portal_import.uploads", count);
			core::Metrics::Count("render.portal_import.upload_bytes", double(bytes));
		}
		ReportPortalImportUsage();
	}

	void Renderer::Impl::FinishPortalImports(SDL_GPUCommandBuffer *command, bool submitted) {
		FinishPortalShadowImports(command, submitted);
		for (auto &image : ImportedPortals) {
			if (image.Recorded == nullptr || (command != nullptr && image.Recorded != command)) {
				continue;
			}
			image.Recorded = nullptr;
			image.Ready = submitted;
			if (submitted) {
				PortalImportUsage.PendingCpuBytes -=
					image.Pending.capacity() + image.PendingDepth.capacity() +
					image.PendingNormal.capacity() + image.PendingAmbientResponse.capacity() +
					(image.PendingLightingBaseline.capacity() + image.PendingDirectionalResponse.capacity());
				std::vector<std::byte>{}.swap(image.Pending);
				std::vector<std::byte>{}.swap(image.PendingDepth);
				std::vector<std::byte>{}.swap(image.PendingNormal);
				std::vector<std::byte>{}.swap(image.PendingAmbientResponse);
				std::vector<std::byte>{}.swap(image.PendingLightingBaseline);
				std::vector<std::byte>{}.swap(image.PendingDirectionalResponse);
			}
		}
		ReportPortalImportUsage();
	}

	uint64_t Renderer::ComposePortalBodyImage(const PortalImageCapture &capture, const View &body) {
		if (capture.Tree != 0) return ComposePortalCaptureTree(capture.Tree, body);
		return ComposePortalBodyImageInternal(capture, body, capture.Binding);
	}

	uint64_t Renderer::ComposePortalBodyImageInternal(
		const PortalImageCapture &capture,
		const View &body,
		const PortalImageBinding &output,
		std::span<const scene::DrawInstance> apertureRows,
		std::span<const core::CFrame> apertureJoints,
		std::span<const PortalView> apertures,
		bool freshOutput
	) {
		RequireOwningThread("ComposePortalBodyImage");
		ENGINE_PROFILE("compose current portal body");
		if (!State->Device || !body.Target || !capture.CaptureLighting || capture.Tree != 0 ||
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
		const auto opaqueImage = std::find_if(
			State->ImportedPortals.begin(), State->ImportedPortals.end(), [&](const auto &image) {
				return image.Handle == capture.Image;
			}
		);
		if (opaqueImage == State->ImportedPortals.end()) return 0;
		const bool retainedAmbient = opaqueImage->NormalTexture && opaqueImage->AmbientResponseTexture &&
									 opaqueImage->LightingBaselineTexture;
		const bool retainedDirectional = body.ImportedDirectionalShadow != 0;
		if (retainedDirectional) {
			const auto *shadow = State->FindPortalShadow(body.ImportedDirectionalShadow);
			if (!shadow || !retainedAmbient || !opaqueImage->DirectionalResponseTexture) return 0;
			const auto &snapshot = shadow->Binding.ExpectedSnapshot;
			if (!opaqueImage->CaptureTick || snapshot.CaptureTick != *opaqueImage->CaptureTick ||
				snapshot.Eye != capture.Binding.Expected || snapshot.EyePixelHash != opaqueImage->PixelHash ||
				snapshot.ContentRevision != opaqueImage->ContentRevision ||
				snapshot.LightingRevision != opaqueImage->LightingRevision ||
				snapshot.Producer.World != capture.Producer.World ||
				snapshot.Producer.Channel != capture.Producer.Channel ||
				snapshot.Producer.Session != capture.Producer.Session ||
				snapshot.Producer.Generation != capture.Producer.Generation ||
				snapshot.ExcludedPlayer != capture.RetainedBodyPlayer)
				return 0;
		}
		const bool spatialOverlay = capture.SpatialOverlayImage != 0;
		const std::array<uint64_t, 4> allHandles{
			capture.Image,
			capture.TransparentImages[0],
			capture.TransparentImages[1],
			capture.SpatialOverlayImage
		};
		if (capture.TransparentImages[0] == 0 && capture.TransparentImages[1] != 0) return 0;
		const size_t physical =
			size_t(capture.TransparentImages[0] != 0) + size_t(capture.TransparentImages[1] != 0);
		const size_t count = 1 + physical + spatialOverlay;
		for (size_t layer = 0; layer < allHandles.size(); ++layer) {
			if (allHandles[layer] == 0) continue;
			const auto image = std::find_if(
				State->ImportedPortals.begin(), State->ImportedPortals.end(), [&](const auto &entry) {
					return entry.Handle == allHandles[layer];
				}
			);
			auto binding = capture.Binding;
			binding.Layer = static_cast<uint8_t>(layer);
			if (image == State->ImportedPortals.end() || image->LayerSet != capture.Image ||
				image->LayerSetCount != count || !SameOwner(image->Binding, binding) ||
				image->Binding.Expected != binding.Expected ||
				image->Binding.ExpectedProjection != binding.ExpectedProjection ||
				image->Binding.Sampling != binding.Sampling || image->Width != capture.Width ||
				image->Height != capture.Height)
				return 0;
		}
		auto view = body;
		std::vector<scene::DrawInstance> combinedRows;
		std::vector<core::CFrame> combinedJoints;
		if (!apertures.empty()) {
			combinedRows.assign(body.Instances.begin(), body.Instances.end());
			combinedJoints.assign(body.JointFrames.begin(), body.JointFrames.end());
			const auto jointOffset = combinedJoints.size();
			combinedJoints.insert(combinedJoints.end(), apertureJoints.begin(), apertureJoints.end());
			for (auto row : apertureRows) {
				if (row.SkinCount != 0) row.SkinFirst += static_cast<uint32_t>(jointOffset);
				combinedRows.push_back(row);
			}
			view.Instances = combinedRows;
			view.JointFrames = combinedJoints;
			view.Portals = apertures;
		}
		std::array<SceneLight, MAX_PORTAL_CAPTURE_LIGHTS> lights{};
		if (!ResolvePortalCaptureCamera(capture.Camera, capture.Binding.ExpectedProjection, view) ||
			!ResolvePortalCaptureLighting(*capture.CaptureLighting, view, lights))
			return 0;
		if (!ValidPortalCaptureLenses(capture.Lenses)) return 0;
		const bool shaderLenses = !capture.Lenses.Entries.empty();
		auto lensOwner = body.ContentOwnerOf(core::Name(capture.Producer.World));
		if (capture.LensPrograms != 0) {
			const auto group = std::find_if(
				State->PortalLensPrograms.begin(),
				State->PortalLensPrograms.end(),
				[&](const auto &candidate) { return candidate.Token == capture.LensPrograms; }
			);
			if (group == State->PortalLensPrograms.end()) return 0;
			for (const auto &binding : group->Bindings) {
				if (std::none_of(
						capture.Lenses.Entries.begin(), capture.Lenses.Entries.end(), [&](const auto &lens) {
							return binding.Shader.Text() == lens.Shader && binding.Hash == lens.ProgramHash;
						}
					))
					return 0;
			}
			for (const auto &lens : capture.Lenses.Entries) {
				if (std::none_of(group->Bindings.begin(), group->Bindings.end(), [&](const auto &binding) {
						return binding.Shader.Text() == lens.Shader && binding.Hash == lens.ProgramHash;
					}))
					return 0;
			}
		}
		if (shaderLenses &&
			(capture.Producer.World.empty() || (capture.LensPrograms == 0 && !lensOwner.IsValid())))
			return 0;
		view.Lighting.ShaderLensCount = 0;
		for (const auto &lens : capture.Lenses.Entries) {
			const core::Name shader(lens.Shader);
			if (capture.LensPrograms == 0 && LensShaderHash(shader, lensOwner) != lens.ProgramHash) return 0;
			const auto &position = lens.Position;
			const auto &orientation = lens.Orientation;
			view.Lighting.ShaderLenses[view.Lighting.ShaderLensCount++] = {
				.Frame = core::CFrame(
					core::Vector3(position[0], position[1], position[2]),
					glm::quat(orientation[3], orientation[0], orientation[1], orientation[2])
				),
				.Shader = shader,
				.Radius = lens.Radius,
				.InnerRadius = lens.InnerRadius,
				.Falloff = lens.Falloff,
				.Strength = lens.Strength,
				.Spin = lens.Spin,
				.Priority = lens.Priority,
				.Shape = static_cast<scene::LensShape>(lens.Shape),
			};
		}
		view.LensTimeSeconds = capture.Lenses.TimeSeconds;
		view.LensContentOwner = lensOwner;
		view.LensPrograms = capture.LensPrograms;
		const bool seam = capture.Binding.ExpectedProjection == PortalImageProjection::Seam;
		const core::Name pipelineName(
			std::string(seam ? "portal-body-seam-image/" : "portal-body-eye-image/") +
			(apertures.empty() ? "" : "nested/") + (spatialOverlay ? "overlay/" : "") +
			(shaderLenses ? "lenses/" : "") + (retainedAmbient ? "ambient/" : "") +
			(retainedDirectional ? "shadow/" : "") + "layers-" + std::to_string(physical) + "/" +
			std::to_string(body.Slot)
		);
		if (std::none_of(
				State->NamedPipelines.begin(), State->NamedPipelines.end(), [&](const auto &pipeline) {
					return pipeline.Name == pipelineName;
				}
			)) {
			graph::PipelineDocument document;
			const auto base = graph::DefaultPortalBodyDocument(
				seam,
				physical != 0,
				spatialOverlay,
				shaderLenses,
				!apertures.empty(),
				physical,
				retainedAmbient,
				retainedDirectional
			);
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
		view.EyeSpatialOverlayImage = capture.SpatialOverlayImage;
		const auto token = QueueResourceImage(
			pipelineName, core::Name("export"), body.Slot, ResourceImageDelivery::Resident
		);
		if (token == 0) return 0;
		OverlayImage overlay;
		const auto rendered = Render(std::span(&view, 1), overlay, nullptr, false);
		uint64_t image = 0;
		if (rendered.Ran(core::Name("export")))
			AdoptResourceImagesInternal(
				std::span(&token, 1), std::span(&output, 1), std::span(&image, 1), !freshOutput
			);
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
