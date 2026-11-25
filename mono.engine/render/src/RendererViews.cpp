// Renderer output: scene captures, view and resource textures, spatial depth
// readback, and the `Render` and `RenderView` frame entry points.

#include "DisplayColour.hpp"
#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "SurfaceScale.hpp"
#include "capture/ViewRecording.hpp"
#include "diagnostics/VulkanTimestamps.hpp"
#include "portal/PortalRendererTerminalObserver.hpp"
#include "presentation/FrameBatch.hpp"
#include "resources/MaterialSamplerAdmission.hpp"
#include "resources/ResourcePreview.hpp"
#include "resources/ShaderBinary.hpp"
#include "resources/StartupCompilation.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/capture/DataFactoryHookBind.hpp>
#include <engine/render/pipeline/GraphRunner.hpp>
#include <engine/render/portal/PortalExchange.hpp>
#include <engine/render/resources/MeshTable.hpp>
#include <engine/render/resources/MissingTexture.hpp>
#include <engine/render/resources/ShaderCompiler.hpp>
#include <engine/render/resources/ShaderLibrary.hpp>
#include <engine/render/resources/TextureTable.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/environment/Sunlight.hpp>

// `scene::ReflectCamera` and `scene::SurfacePane`: where a mirror's camera goes
// for a viewer that is not the eye, which is what makes the levels below the
// first a recursion rather than a repeat.
#include <engine/scene/Tagging.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_video.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <spirv-tools/libspirv.hpp>
#include <spirv_cross.hpp>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace engine::render {
	bool Renderer::Impl::ReadCapture(
		SDL_GPUTransferBuffer *from,
		uint32_t width,
		uint32_t height,
		SDL_GPUTextureFormat format,
		SceneCaptureResult &result
	) const {
		const bool bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
						  format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
		const bool rgba = format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ||
						  format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
		if (!bgra && !rgba) {
			ENGINE_ERROR("capture: texture format {} has no RGBA8 mapping", static_cast<int>(format));
			return false;
		}
		const void *mapped = SDL_MapGPUTransferBuffer(Device, from, false);
		if (mapped == nullptr) {
			ENGINE_ERROR("capture: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
			return false;
		}
		struct Unmap {
			SDL_GPUDevice *Device;
			SDL_GPUTransferBuffer *Transfer;
			~Unmap() {
				SDL_UnmapGPUTransferBuffer(Device, Transfer);
			}
		} unmap{Device, from};
		result.Width = width;
		result.Height = height;
		result.RowStride = static_cast<size_t>(width) * 4;
		result.Format = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB ||
								format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
							? ResourceImageFormat::RGBA8_SRGB
							: ResourceImageFormat::RGBA8_UNorm;
		result.Pixels.resize(result.RowStride * height);
		std::memcpy(result.Pixels.data(), mapped, result.Pixels.size());
		if (bgra) {
			for (size_t offset = 0; offset < result.Pixels.size(); offset += 4)
				std::swap(result.Pixels[offset], result.Pixels[offset + 2]);
		}
		result.Status = SceneCaptureStatus::Ready;
		return true;
	}

	bool
	Renderer::Impl::WriteCapture(const SceneCaptureResult &image, const std::filesystem::path &path) const {
		SDL_Surface *surface = SDL_CreateSurfaceFrom(
			static_cast<int>(image.Width),
			static_cast<int>(image.Height),
			SDL_PIXELFORMAT_RGBA32,
			const_cast<uint8_t *>(image.Pixels.data()),
			static_cast<int>(image.RowStride)
		);
		if (surface == nullptr) {
			ENGINE_ERROR("capture: SDL_CreateSurfaceFrom: {}", SDL_GetError());
			return false;
		}
		const bool wrote = SDL_SaveBMP(surface, path.string().c_str());
		if (!wrote) ENGINE_ERROR("capture: SDL_SaveBMP: {}", SDL_GetError());
		SDL_DestroySurface(surface);
		return wrote;
	}

	bool Renderer::Impl::WriteCapture(
		SDL_GPUTransferBuffer *from,
		uint32_t width,
		uint32_t height,
		SDL_GPUTextureFormat format,
		const std::filesystem::path &path
	) const {
		SceneCaptureResult image;
		return ReadCapture(from, width, height, format, image) && WriteCapture(image, path);
	}

	bool Renderer::Impl::HasSceneCaptureTransfers() const {
		return std::any_of(SceneCaptures.begin(), SceneCaptures.end(), [](const auto &capture) {
			return capture.Transfer != nullptr;
		});
	}

	void Renderer::Impl::CompleteSceneCaptures(bool succeeded) {
		for (auto &capture : SceneCaptures) {
			if (capture.Result.Token == 0 || capture.Complete) continue;
			if (succeeded && capture.Transfer != nullptr) {
				ReadCapture(
					capture.Transfer,
					capture.Result.Width,
					capture.Result.Height,
					capture.Format,
					capture.Result
				);
			}
			if (capture.Transfer != nullptr) gpu::ReleaseTransferBuffer(Device, capture.Transfer);
			capture.Transfer = nullptr;
			capture.Complete = true;
		}
	}

	uint64_t Renderer::RequestSceneCapture(const SceneCaptureRequest &request) {
		RequireOwningThread("RequestSceneCapture");
		if (!State->Device || State->BatchActive || request.MaxBytes == 0 ||
			(request.ExpectedSnapshotId && request.ExpectedSnapshotId->size() > 4096))
			return 0;
		static std::atomic<uint64_t> nextToken{1};
		for (auto &capture : State->SceneCaptures) {
			if (capture.Result.Token != 0) continue;
			uint64_t token = nextToken.load(std::memory_order_relaxed);
			while (token != 0 && !nextToken.compare_exchange_weak(
									 token, token == UINT64_MAX ? 0 : token + 1, std::memory_order_relaxed
								 )) {}
			if (token == 0) return 0;
			capture.Request = request;
			capture.Result.Token = token;
			capture.Result.ViewSlot = request.ViewSlot;
			return token;
		}
		return 0;
	}

	std::optional<SceneCaptureResult> Renderer::TakeSceneCapture(uint64_t token) {
		RequireOwningThread("TakeSceneCapture");
		for (auto &capture : State->SceneCaptures) {
			if (token == 0 || capture.Result.Token != token || !capture.Complete) continue;
			SceneCaptureResult result = std::move(capture.Result);
			capture = {};
			return result;
		}
		return std::nullopt;
	}

	bool Renderer::CancelSceneCapture(uint64_t token) {
		RequireOwningThread("CancelSceneCapture");
		for (auto &capture : State->SceneCaptures) {
			if (token == 0 || capture.Result.Token != token || capture.Transfer != nullptr) continue;
			capture = {};
			return true;
		}
		return false;
	}

	void Renderer::RequestSceneCapture(std::filesystem::path path, size_t slot) {
		State->CapturePath = std::move(path);
		State->CaptureSlot = slot;
	}

	void Renderer::RequestWindowCapture(std::filesystem::path path) {
		State->WindowCapturePath = std::move(path);
	}

	bool Renderer::CapturePending() const {
		return State != nullptr && (!State->CapturePath.empty() || !State->WindowCapturePath.empty());
	}

	bool Renderer::IsHeadless() const {
		return State->Headless();
	}

	VisibilitySnapshot Renderer::Visibility() const {
		return State == nullptr ? VisibilitySnapshot{} : State->VisibilityCompleted;
	}

	PassActivity Renderer::ViewPassActivity(size_t slot) const {
		if (!State || slot >= State->SceneSlots.size()) return {};
		const auto &scene = State->SceneSlots[slot];
		const auto *pipeline = State->Installed(scene.ForwardPipeline);
		if (!pipeline || pipeline->Revision != scene.ForwardRevision) return {};
		return scene.Activity;
	}

	void *Renderer::SceneTexture(size_t slot) const {
		if (slot >= State->SceneSlots.size()) {
			return nullptr;
		}
		const Impl::SceneSlot &scene = State->SceneSlots[slot];
		if (scene.PublishedFrame == Impl::SceneSlot::NO_RETAINED_FRAME) {
			return nullptr;
		}
		return scene.Retained[scene.PublishedFrame].Texture;
	}

	FrameResult Renderer::SceneFrameResult(size_t slot) const {
		if (slot >= State->SceneSlots.size()) {
			return {};
		}
		const Impl::SceneSlot &scene = State->SceneSlots[slot];
		if (scene.PublishedFrame == Impl::SceneSlot::NO_RETAINED_FRAME) {
			return {};
		}
		return scene.Retained[scene.PublishedFrame].Result;
	}

	SpatialDepthFrameStatus Renderer::SpatialDepthStatus(SpatialDepthFrameKey frame) {
		RequireOwningThread("SpatialDepthStatus");
		if (!State || !State->Device || !frame.Frame || frame.Renderer != State->SpatialRendererIdentity ||
			frame.Viewport >= State->SceneSlots.size())
			return SpatialDepthFrameStatus::Unknown;
		State->PollSceneFrames();
		auto &owner = State->SceneSlots[frame.Viewport].SpatialDepth;
		if (!owner) return SpatialDepthFrameStatus::Unknown;
		owner->Reap(State->Device);
		return owner->Status(frame);
	}

	std::optional<SpatialDepthFrameInfo> Renderer::SpatialDepthInfo(SpatialDepthFrameKey frame) {
		if (SpatialDepthStatus(frame) != SpatialDepthFrameStatus::Ready) return {};
		auto &owner = State->SceneSlots[frame.Viewport].SpatialDepth;
		return owner ? owner->Info(frame) : std::optional<SpatialDepthFrameInfo>{};
	}

	uint64_t Renderer::QueueSpatialDepth(SpatialDepthFrameKey frame, uint32_t x, uint32_t y) {
		if (SpatialDepthStatus(frame) != SpatialDepthFrameStatus::Ready) return 0;
		auto &owner = State->SceneSlots[frame.Viewport].SpatialDepth;
		return owner ? owner->Queue(State->Device, frame, x, y) : 0;
	}

	SpatialDepthReadbackResult Renderer::TakeSpatialDepth(SpatialDepthFrameKey frame, uint64_t token) {
		RequireOwningThread("TakeSpatialDepth");
		if (!State || !State->Device || !frame.Frame || frame.Renderer != State->SpatialRendererIdentity ||
			frame.Viewport >= State->SceneSlots.size())
			return {};
		auto &owner = State->SceneSlots[frame.Viewport].SpatialDepth;
		return owner ? owner->Take(State->Device, frame, token) : SpatialDepthReadbackResult{};
	}

	bool Renderer::CancelSpatialDepth(SpatialDepthFrameKey frame, uint64_t token) {
		RequireOwningThread("CancelSpatialDepth");
		if (!State || !State->Device || !frame.Frame || frame.Renderer != State->SpatialRendererIdentity ||
			frame.Viewport >= State->SceneSlots.size())
			return false;
		auto &owner = State->SceneSlots[frame.Viewport].SpatialDepth;
		return owner && owner->Cancel(frame, token);
	}

	uint64_t Renderer::RenderGeneration() const {
		return State ? State->RenderGeneration : 0;
	}

	void *Renderer::ResourceTexture(core::Name resource, size_t slot) const {
		if (State == nullptr || !resource.IsValid()) {
			return nullptr;
		}
		const Impl::InstalledPipeline *pipeline = State->PipelineFor(State->ActiveGraph);
		if (pipeline != nullptr) {
			if (slot < State->SceneSlots.size()) {
				const auto &scene = State->SceneSlots[slot];
				if (scene.ForwardUnavailable && scene.ForwardPipeline == pipeline->Name &&
					scene.ForwardRevision == pipeline->Revision)
					return nullptr;
			}
			const auto forwarded = State->ForwardedTexture(*pipeline, resource, slot);
			if (forwarded.IsValid()) return forwarded.Texture;
			const core::Name physical = State->GraphTargetName(*pipeline, resource);
			for (const Impl::GraphTarget &target : State->GraphResources.Targets) {
				if (target.Pipeline != pipeline->Name || target.Resource != physical ||
					target.Texture == nullptr) {
					continue;
				}
				if (target.Scope != graph::NodeScope::View || target.Owner == slot) {
					return target.Texture;
				}
			}
		}
		const Impl::ResourceRole role = State->RoleFor(resource);
		if (role == Impl::ResourceRole::PreviousFrame) {
			if (slot >= State->SceneSlots.size()) {
				return nullptr;
			}
			const Impl::SceneSlot &history = State->SceneSlots[slot];
			return history.HistoryReady ? history.History : nullptr;
		}
		if (role == Impl::ResourceRole::Scene) {
			return SceneTexture(slot);
		}
		if (role == Impl::ResourceRole::Depth) {
			return slot < State->SceneSlots.size() ? State->SceneSlots[slot].Depth : nullptr;
		}
		if (role == Impl::ResourceRole::Shadow) {
			return State->ShadowTexture;
		}
		if (role == Impl::ResourceRole::Surface && slot < State->SurfaceBanks.size()) {
			const Impl::SurfaceSlotState &surface = State->SurfaceBanks[slot].Surfaces[0];
			return surface.Ready ? surface.Texture[surface.Slot] : nullptr;
		}
		if ((role == Impl::ResourceRole::PortalImage || role == Impl::ResourceRole::PortalDisplay) &&
			slot < State->SurfaceBanks.size()) {
			const std::vector<Impl::PortalLevel> &levels = State->SurfaceBanks[slot].Portals;
			for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
				for (const Impl::PortalTarget &portal : level->Targets) {
					SDL_GPUTexture *texture =
						role == Impl::ResourceRole::PortalImage ? portal.Colour : portal.Display;
					if (texture != nullptr) {
						return texture;
					}
				}
			}
			return nullptr;
		}
		if (role == Impl::ResourceRole::PortalLight && slot < State->SurfaceBanks.size()) {
			for (const Impl::SeamLightTarget &seamLight : State->SurfaceBanks[slot].SeamLights) {
				if (seamLight.Ready && seamLight.Colour != nullptr) {
					return seamLight.Colour;
				}
			}
			return nullptr;
		}
		if (slot >= State->PbrSlots.size()) {
			return nullptr;
		}

		const Impl::PbrSlot &pbr = State->PbrSlots[slot];
		if (role == Impl::ResourceRole::Albedo) {
			return pbr.Albedo;
		}
		if (role == Impl::ResourceRole::Normal) {
			return pbr.Normal;
		}
		if (role == Impl::ResourceRole::Material) {
			return pbr.Material;
		}
		if (role == Impl::ResourceRole::Emissive) {
			return pbr.Emissive;
		}
		if (role == Impl::ResourceRole::MeshUv) {
			return pbr.MeshUv;
		}
		if (role == Impl::ResourceRole::ObjectIds) return pbr.ObjectIds;
		if (role == Impl::ResourceRole::SemanticIds) return pbr.SemanticIds;
		if (role == Impl::ResourceRole::PartIds) return pbr.PartIds;
		if (role == Impl::ResourceRole::LinearDepth) {
			return pbr.LinearDepth;
		}
		if (role == Impl::ResourceRole::FirstSurfaceValidity) {
			return pbr.FirstSurfaceValidity;
		}
		if (role == Impl::ResourceRole::CameraMotionVectors) {
			return pbr.CameraMotionVectors;
		}
		if (role == Impl::ResourceRole::SecondSurfaceZ) {
			return pbr.SecondSurfaceZ;
		}
		if (role == Impl::ResourceRole::SecondSurfaceDepth) {
			return pbr.SecondSurfaceDepth;
		}
		if (role == Impl::ResourceRole::SecondSurfaceValidity) {
			return pbr.SecondSurfaceValidity;
		}
		if (role == Impl::ResourceRole::Occlusion) {
			return pbr.Occlusion;
		}
		if (role == Impl::ResourceRole::Lit) {
			return pbr.Lit;
		}
		if (role == Impl::ResourceRole::SkyLit) {
			return pbr.SkyLit;
		}
		if (role == Impl::ResourceRole::VolumeLit) {
			return pbr.Lit;
		}
		return nullptr;
	}

	void Renderer::RefreshResourcePreview(
		core::Name pipeline, core::Name resource, size_t slot, bool reverseSpectrum
	) {
		if (State == nullptr || !pipeline.IsValid() || !resource.IsValid()) {
			return;
		}
		const ResourcePreviewRoute route{pipeline, resource, slot};
		for (Impl::ResourcePreviewTarget &preview : State->GraphResources.Previews) {
			if (preview.Route == route) {
				preview.ReverseSpectrum = reverseSpectrum;
				preview.Refresh = true;
				return;
			}
		}
		Impl::ResourcePreviewTarget preview;
		preview.Route = route;
		preview.ReverseSpectrum = reverseSpectrum;
		State->GraphResources.Previews.push_back(std::move(preview));
	}

	void *Renderer::ResourcePreviewTexture(core::Name pipeline, core::Name resource, size_t slot) const {
		if (State == nullptr || !pipeline.IsValid() || !resource.IsValid()) {
			return nullptr;
		}
		const ResourcePreviewRoute route{pipeline, resource, slot};
		for (const Impl::ResourcePreviewTarget &preview : State->GraphResources.Previews) {
			if (preview.Route == route) {
				return preview.Slots.Ready ? preview.Textures[preview.Slots.Visible] : nullptr;
			}
		}
		return nullptr;
	}

	float Renderer::ResourcePreviewAspect(core::Name pipeline, core::Name resource, size_t slot) const {
		if (State == nullptr || !pipeline.IsValid() || !resource.IsValid()) {
			return 0.0f;
		}
		const ResourcePreviewRoute route{pipeline, resource, slot};
		for (const Impl::ResourcePreviewTarget &preview : State->GraphResources.Previews) {
			if (preview.Route == route) {
				if (preview.Height == 0) {
					return 0.0f;
				}
				return static_cast<float>(preview.Width) / static_cast<float>(preview.Height);
			}
		}
		return 0.0f;
	}

	bool Renderer::CaptureSceneTexture(size_t slot, const core::Name &name) {
		if (State == nullptr || State->Device == nullptr || !name.IsValid()) {
			return false;
		}
		if (slot >= State->SceneSlots.size()) {
			return false;
		}

		const Impl::SceneSlot &source = State->SceneSlots[slot];
		if (source.Texture == nullptr || source.DrawnWidth == 0 || source.DrawnHeight == 0) {
			return false;
		}

		// **The drawn rectangle and not the whole target.** A scene target is
		// allocated in 64-pixel blocks with hysteresis, so most of it is border
		// the pass never wrote - `SceneTextureExtent` exists because of exactly
		// that. Copying the allocation would keep a picture with an unwritten
		// margin down two edges, and every consumer would then need the extent
		// as well as the handle, which is the coupling this call is meant to end.
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;

		// The source's format, for `EnsureSceneTarget`'s reason one level up: a
		// blit between mismatched formats is a validation error on the backends
		// that check and a corrupt image on the ones that do not.
		info.format = State->ColourFormat();

		// Sampled, and a colour target because `SDL_BlitGPUTexture` renders into
		// its destination rather than copying into it.
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		info.width = source.DrawnWidth;
		info.height = source.DrawnHeight;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;

		SDL_GPUTexture *copy = gpu::CreateTexture(State->Device, &info);
		if (copy == nullptr) {
			ENGINE_ERROR("SDL_CreateGPUTexture (scene capture): {}", SDL_GetError());
			return false;
		}

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(State->Device);
		if (command == nullptr) {
			ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (scene capture): {}", SDL_GetError());
			gpu::ReleaseTexture(State->Device, copy);
			return false;
		}

		// **A blit rather than a copy pass, because the source region is a
		// sub-rectangle.** `SDL_CopyGPUTextureToTexture` would work only if the
		// two agreed on size, which they deliberately do not.
		SDL_GPUBlitInfo blit{};
		blit.source.texture = source.Texture;
		blit.source.w = source.DrawnWidth;
		blit.source.h = source.DrawnHeight;
		blit.destination.texture = copy;
		blit.destination.w = source.DrawnWidth;
		blit.destination.h = source.DrawnHeight;

		// Nothing is preserved, because every texel of the destination is
		// written - saying so lets a tiler skip loading it.
		blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
		blit.filter = SDL_GPU_FILTER_NEAREST;

		SDL_BlitGPUTexture(command, &blit);
		SDL_SubmitGPUCommandBuffer(command);

		// **Four bytes a texel, which is the honest figure for every format this
		// swapchain uses.** Guessing low here would let the table's ceiling be
		// walked past by whoever captures the most.
		const size_t bytes = static_cast<size_t>(source.DrawnWidth) * source.DrawnHeight * 4;

		const bool sampledSRGB = info.format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ||
								 info.format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
		const auto capturedFormat =
			sampledSRGB ? assets::TextureFormat::RGBA8 : assets::TextureFormat::RGBA8_LINEAR;
		if (!State->Textures.Adopt(
				name, copy, source.DrawnWidth, source.DrawnHeight, bytes, {}, capturedFormat
			)) {
			// Refused, so the texture is still ours to release - `Adopt` says so.
			gpu::ReleaseTexture(State->Device, copy);
			return false;
		}

		++State->ResourceEpoch;
		return true;
	}

	SceneExtent Renderer::SceneTextureExtent(size_t slot) const {
		if (slot >= State->SceneSlots.size()) {
			return {};
		}

		const Impl::SceneSlot &target = State->SceneSlots[slot];
		if (target.PublishedFrame == Impl::SceneSlot::NO_RETAINED_FRAME) {
			return {};
		}
		const Impl::SceneSlot::RetainedFrame &frame = target.Retained[target.PublishedFrame];

		// **The whole texture when nothing has been drawn yet**, which is the
		// honest answer rather than a safe one: a caller sampling a texture no
		// pass has written is showing uninitialised memory whatever the
		// coordinates say, and a fraction of it is not better than all of it.
		if (frame.Width == 0 || frame.Height == 0) {
			return {};
		}

		return SceneExtent{
			static_cast<float>(frame.DrawnWidth) / static_cast<float>(frame.Width),
			static_cast<float>(frame.DrawnHeight) / static_cast<float>(frame.Height),
			frame.DrawnWidth,
			frame.DrawnHeight,
		};
	}

	SceneExtent Renderer::ResourceTextureExtent(core::Name resource, size_t slot) const {
		if (State == nullptr) {
			return {};
		}
		const Impl::InstalledPipeline *pipeline = State->PipelineFor(State->ActiveGraph);
		if (pipeline != nullptr) {
			if (slot < State->SceneSlots.size()) {
				const auto &scene = State->SceneSlots[slot];
				if (scene.ForwardUnavailable && scene.ForwardPipeline == pipeline->Name &&
					scene.ForwardRevision == pipeline->Revision)
					return {};
			}
			const auto forwarded = State->ForwardedTexture(*pipeline, resource, slot);
			if (forwarded.IsValid()) return SceneExtent{1.0f, 1.0f, forwarded.Width, forwarded.Height};
			const core::Name physical = State->GraphTargetName(*pipeline, resource);
			for (const Impl::GraphTarget &target : State->GraphResources.Targets) {
				if (target.Pipeline != pipeline->Name || target.Resource != physical ||
					target.Texture == nullptr ||
					(target.Scope == graph::NodeScope::View && target.Owner != slot)) {
					continue;
				}
				return SceneExtent{1.0f, 1.0f, target.Width, target.Height};
			}
		}
		const Impl::ResourceRole role = State->RoleFor(resource);
		if (role == Impl::ResourceRole::PreviousFrame || role == Impl::ResourceRole::Shadow ||
			role == Impl::ResourceRole::Surface || role == Impl::ResourceRole::PortalImage ||
			role == Impl::ResourceRole::PortalDisplay) {
			return SceneExtent{1.0f, 1.0f};
		}
		if (role == Impl::ResourceRole::Scene || role == Impl::ResourceRole::Depth) {
			return SceneTextureExtent(slot);
		}
		if (slot >= State->PbrSlots.size()) {
			return {};
		}

		const Impl::PbrSlot &pbr = State->PbrSlots[slot];
		const Impl::PbrDimensions &dimensions = pbr.Dimensions;
		if (dimensions.TargetWidth == 0 || dimensions.TargetHeight == 0 || dimensions.ViewWidth == 0 ||
			dimensions.ViewHeight == 0) {
			return {};
		}

		if (role == Impl::ResourceRole::Albedo || role == Impl::ResourceRole::Normal ||
			role == Impl::ResourceRole::Material || role == Impl::ResourceRole::Emissive) {
			return SceneExtent{
				static_cast<float>(dimensions.ViewWidth) / static_cast<float>(dimensions.TargetWidth),
				static_cast<float>(dimensions.ViewHeight) / static_cast<float>(dimensions.TargetHeight),
			};
		}
		return SceneExtent{1.0f, 1.0f};
	}

	BackendHandles Renderer::Backend() const {
		BackendHandles handles;
		if (State->Device != nullptr) {
			handles.Device = State->Device;
			handles.ColourFormat = static_cast<uint32_t>(State->ColourFormat());
		}
		return handles;
	}

	FrameResult Renderer::Render(
		std::span<const View> views,
		OverlayImage &overlay,
		FrameOverlayHook *gameInterfaceHook,
		bool present,
		FrameOverlayHook *hostOverlayHook
	) {
		ENGINE_PROFILE_CAT("Renderer::Render views", core::ProfileCategory::Render);
		RequireOwningThread("Render views");
		if (State && State->PortalTreeJob.Cancelled)
			PollPortalCaptureTreeComposition(State->PortalTreeJob.Token);
		struct CompleteCapturesOnExit {
			Impl &State;
			~CompleteCapturesOnExit() {
				State.CompleteSceneCaptures(false);
			}
		} completeCaptures{*State};
		const FrameResult result =
			FrameBatch(*this).Run(views, overlay, gameInterfaceHook, present, hostOverlayHook).Frame;
		if (State->Headless() && result.Submitted) State->ThrottleHeadlessFrames();
		return result;
	}

	void Renderer::Impl::ThrottleHeadlessFrames() {
		SDL_GPUCommandBuffer *marker = SDL_AcquireGPUCommandBuffer(Device);
		if (marker == nullptr) return;
		SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(marker);
		if (fence == nullptr) return;
		HeadlessFrames.push_back(fence);
		while (HeadlessFrames.size() > AllowedFramesInFlight) {
			SDL_GPUFence *oldest = HeadlessFrames.front();
			HeadlessFrames.pop_front();
			(void)SDL_WaitForGPUFences(Device, true, &oldest, 1);
			SDL_ReleaseGPUFence(Device, oldest);
		}
	}

	void Renderer::Impl::ReleaseHeadlessFrames() {
		for (SDL_GPUFence *fence : HeadlessFrames)
			SDL_ReleaseGPUFence(Device, fence);
		HeadlessFrames.clear();
	}

	FrameResult Renderer::RenderView(
		const core::CFrame &cameraFrame,
		const scene::Camera &camera,
		std::span<const scene::DrawInstance> instances,
		OverlayImage &overlay,
		std::span<const SurfaceView> surfaces,
		FrameOverlayHook *gameInterfaceHook,
		FrameOverlayHook *hostOverlayHook,
		const SceneTarget *sceneTarget,
		size_t targetSlot,
		const View &source,
		std::span<const ParticleBatch> particles,
		std::span<const effects::RibbonVertex> ribbonVertices,
		std::span<const effects::RibbonRun> ribbonRuns,
		std::span<const SceneLight> lights,
		std::span<const scene::DrawInstance> foreign,
		std::span<const PortalView> portals,
		bool present,
		core::Name pipeline,
		uint64_t world
	) {
		ENGINE_PROFILE_CAT("Renderer::RenderView", core::ProfileCategory::Render);

		// **The single-threaded recording contract, checked rather than
		// described.** A studio draws one viewport after another; a second one
		// recording from another thread is the failure this refuses. See
		// `IsOnOwningThread` for why that is the design and not a limitation.
		RequireOwningThread("RenderView");

		FrameResult result;

		// **What this function used to be, in four steps.** Until v0.19 the
		// whole of it was here - 5,485 lines, two fifths of the module, holding
		// two dozen node handlers as lambdas over its own locals and opening
		// eighteen render passes inline. A pass opened inline is invisible to
		// the graph, and one translation unit that size cannot be split across
		// cores, so `docs/retired/ARCH_REVIEW.md` C2 argued the two were one change.
		// This is that change: `ViewRecording` holds what the handlers closed
		// over, and each node family registers its own runners from its own
		// file. `D00016`.
		//
		// **The recording is declared before the table and outlives it**, which
		// the handlers depend on: each of them captures the recording and is
		// called while the graph runs.
		ViewRecording recording(*this, *State, result);
		struct ReleaseViewBinding {
			GpuQueueBatch &Queues;
			SDL_GPUCommandBuffer **Command;
			~ReleaseViewBinding() {
				Queues.UnbindView(Command);
			}
		} releaseViewBinding{State->GpuQueues, &recording.Command};

		// **Named rather than positional**, because nineteen arguments in a row
		// is nineteen chances to hand `foreign` to the parameter that wanted
		// `instances` - two spans of the same type, and the compiler would have
		// nothing to say about it.
		PresentationDamage damage = source.Damage;
		damage.Scene = damage.Scene || (damage.GameInterface && gameInterfaceHook != nullptr &&
										gameInterfaceHook->AffectsScene());
		const ViewRequest request{
			.CameraFrame = cameraFrame,
			.VisibilityCameraFrame = source.VisibilityCameraFrame(),
			.Camera = camera,
			.Instances = instances,
			.JointFrames = source.JointFrames,
			.Overlay = &overlay,
			.Surfaces = surfaces,
			.GameInterfaceHook = gameInterfaceHook,
			.HostOverlayHook = hostOverlayHook,
			.Target = sceneTarget,
			.TargetSlot = targetSlot,
			.Source = &source,
			.Particles = particles,
			.RibbonVertices = ribbonVertices,
			.RibbonRuns = ribbonRuns,
			.Lights = lights,
			.Foreign = foreign,
			.ForeignJointFrames = source.ForeignJointFrames,
			.Portals = portals,
			.Present = present,
			.Pipeline = pipeline,
			.World = world,
			.Damage = damage,
		};
		if (recording.Begin(request) != ViewStart::Recording) {
			return result;
		}
		const ViewMutationIdentity captureIdentity{
			.WorldName = std::string(source.WorldName.Text()),
			.SnapshotId = source.SnapshotId,
			.Pipeline = pipeline,
			.PipelineRevision = recording.Pipeline->Revision,
			.ViewSlot = targetSlot,
		};
		if (Hooks().ApplyLocalLightCapture(captureIdentity, recording)) recording.Request.Damage.Scene = true;

		recording.Finish();
		Hooks().CompleteLocalLightCapture(captureIdentity, recording);
		return result;
	}
}
