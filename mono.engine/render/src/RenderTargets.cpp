// Every texture this module allocates, and when it lets one go.
//
// **The sizes are decided elsewhere and the allocation is decided here.**
// `ViewRecording::Begin` works out how big each image has to be; these
// functions turn that into device memory, keep it while it is the right size,
// and retire it at a frame boundary rather than under a draw list that may
// still name it. `render/AGENTS.md` carries what each pass may assume of them -
// the shadow map's two usages, the surface pair, and why a portal level is
// indexed by level *and* slot.

#include "DisplayColour.hpp"
#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace engine::render {

	bool Renderer::Impl::EnsureScene(uint32_t width, uint32_t height) {
		SceneSlot &target = SlotAt(ActiveSlot);

		// **Recorded before anything decides whether to allocate.** This is the
		// rectangle the pass draws and the rectangle `SceneTextureExtent`
		// reports, and it is the panel's size whether or not the texture under
		// it changed - which is the whole point of keeping the two apart.
		target.DrawnWidth = width;
		target.DrawnHeight = height;

		if (width == 0 || height == 0) {
			// Nobody wants a picture. Retired rather than released for the
			// reason below: an interface hook may already have recorded a bind
			// of it for the frame in progress.
			if (target.Texture) {
				RetiredScenes.push_back(target.Texture);
				target.Texture = nullptr;
			}

			// **Released outright rather than retired.** Nothing samples a depth
			// buffer - no interface hook can have recorded a bind of it - so the
			// grace period the colour target needs does not apply, and a closed
			// panel should not go on holding a megabyte of it.
			if (target.Depth) {
				gpu::ReleaseTexture(Device, target.Depth);
				target.Depth = nullptr;
			}

			target.Width = 0;
			target.Height = 0;
			target.DepthWidth = 0;
			target.DepthHeight = 0;
			return false;
		}

		const uint32_t wantWidth = BlockUp(width);
		const uint32_t wantHeight = BlockUp(height);

		// **Kept when it is big enough, and only replaced when it is far too
		// big.** Growing is forced - a texture smaller than the rectangle would
		// clip the world - but shrinking is not, and refusing to shrink for a
		// factor of two is what stops a drag from reallocating on the way back
		// down as well as on the way up. See `SCENE_TARGET_BLOCK`.
		if (target.Texture && wantWidth <= target.Width && wantHeight <= target.Height) {
			const bool wasteful = target.Width >= wantWidth * 2 || target.Height >= wantHeight * 2;
			if (!wasteful) {
				return true;
			}
		}

		if (target.Texture) {
			// **Retired rather than released.** An interface hook has already
			// recorded a bind of this texture for the frame in progress - that
			// is what "the image is last frame's texture" means - so freeing it
			// here is a use-after-free that lands inside SDL's Vulkan backend.
			// `DrainRetiredScenes` frees it at the top of the next frame.
			RetiredScenes.push_back(target.Texture);
			target.Texture = nullptr;
		}

		target.Width = wantWidth;
		target.Height = wantHeight;

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;

		// **The swapchain's format, and that is a requirement rather than a
		// convenience.** The opaque and transparent pipelines were built with
		// the swapchain's colour format; a target in another format is a
		// validation error at bind time on the backends that check and a
		// corrupt image on the ones that do not.
		info.format = ColourFormat();

		// Sampled as well as drawn into, because the whole point is that
		// something shows it afterwards.
		info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

		info.width = wantWidth;
		info.height = wantHeight;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;

		target.Texture = gpu::CreateTexture(Device, &info);
		if (!target.Texture) {
			ENGINE_ERROR("SDL_CreateGPUTexture (scene target): {}", SDL_GetError());
			target.Width = 0;
			target.Height = 0;

			// The drawn rectangle goes with it. Leaving it set would have
			// `SceneTextureExtent` divide by a texture that does not exist.
			target.DrawnWidth = 0;
			target.DrawnHeight = 0;
			return false;
		}
		return true;
	}

	bool Renderer::Impl::EnsureHistory(size_t slot, uint32_t width, uint32_t height) {
		SceneSlot &history = SlotAt(slot);
		if (history.History != nullptr && history.HistoryWidth == width && history.HistoryHeight == height) {
			return true;
		}

		if (history.History != nullptr) {
			// The texture may still be sampled by work submitted for the previous
			// frame. Retiring it follows the same frame-boundary ownership rule as
			// a resized viewport target.
			RetiredScenes.push_back(history.History);
			history.History = nullptr;
		}
		history.HistoryWidth = 0;
		history.HistoryHeight = 0;
		history.HistoryReady = false;
		if (width == 0 || height == 0) {
			return false;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = ColourFormat();
		info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		history.History = gpu::CreateTexture(Device, &info);
		if (history.History == nullptr) {
			ENGINE_ERROR("SDL_CreateGPUTexture (frame history): {}", SDL_GetError());
			return false;
		}
		history.HistoryWidth = width;
		history.HistoryHeight = height;
		return true;
	}

	bool Renderer::Impl::EnsureDepthIn(
		SDL_GPUTexture *&texture, uint32_t &haveWidth, uint32_t &haveHeight, uint32_t width, uint32_t height
	) {
		if (texture && width == haveWidth && height == haveHeight) {
			return true;
		}

		if (texture) {
			gpu::ReleaseTexture(Device, texture);
			texture = nullptr;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = DepthFormat;
		// Deferred depth-linearisation samples the same depth the geometry pass
		// writes. The chosen format was probed for both usages at initialisation.
		info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		texture = gpu::CreateTexture(Device, &info);
		if (!texture) {
			ENGINE_ERROR("depth texture {}x{}: {}", width, height, SDL_GetError());
			haveWidth = 0;
			haveHeight = 0;
			return false;
		}

		haveWidth = width;
		haveHeight = height;
		return true;
	}

	bool Renderer::Impl::EnsureDepth(uint32_t width, uint32_t height) {
		return EnsureDepthIn(DepthTexture, DepthWidth, DepthHeight, width, height);
	}

	void Renderer::Impl::ReleasePbr(PbrSlot &slot) {
		for (SDL_GPUTexture *texture :
			 {slot.Albedo,
			  slot.Normal,
			  slot.Material,
			  slot.Emissive,
			  slot.LinearDepth,
			  slot.Occlusion,
			  slot.Lit}) {
			if (texture != nullptr) {
				gpu::ReleaseTexture(Device, texture);
			}
		}
		slot = {};
	}

	bool Renderer::Impl::EnsurePbr(size_t index, const PbrDimensions &dimensions) {
		PbrSlot &slot = PbrAt(index);
		if (slot.Albedo != nullptr && slot.Dimensions == dimensions) {
			return true;
		}
		if (dimensions.TargetWidth == 0 || dimensions.TargetHeight == 0 || dimensions.ViewWidth == 0 ||
			dimensions.ViewHeight == 0 || dimensions.LinearWidth == 0 || dimensions.LinearHeight == 0 ||
			dimensions.OcclusionWidth == 0 || dimensions.OcclusionHeight == 0 || dimensions.LitWidth == 0 ||
			dimensions.LitHeight == 0 || !EnsureSurfaceSampler()) {
			return false;
		}

		PbrSlot made;
		made.Dimensions = dimensions;
		const auto texture = [&](SDL_GPUTextureFormat format, uint32_t textureWidth, uint32_t textureHeight) {
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = format;
			info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
			info.width = textureWidth;
			info.height = textureHeight;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			return gpu::CreateTexture(Device, &info);
		};

		made.Albedo = texture(
			SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, dimensions.TargetWidth, dimensions.TargetHeight
		);
		made.Normal =
			texture(SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM, dimensions.TargetWidth, dimensions.TargetHeight);
		made.Material =
			texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, dimensions.TargetWidth, dimensions.TargetHeight);
		made.Emissive = texture(
			SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, dimensions.TargetWidth, dimensions.TargetHeight
		);
		made.LinearDepth =
			texture(SDL_GPU_TEXTUREFORMAT_R32_FLOAT, dimensions.LinearWidth, dimensions.LinearHeight);
		made.Occlusion =
			texture(SDL_GPU_TEXTUREFORMAT_R8_UNORM, dimensions.OcclusionWidth, dimensions.OcclusionHeight);
		made.Lit =
			texture(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, dimensions.LitWidth, dimensions.LitHeight);

		if (made.Albedo == nullptr || made.Normal == nullptr || made.Material == nullptr ||
			made.Emissive == nullptr || made.LinearDepth == nullptr || made.Occlusion == nullptr ||
			made.Lit == nullptr) {
			ENGINE_ERROR(
				"render graph targets for {}x{} view: {}",
				dimensions.ViewWidth,
				dimensions.ViewHeight,
				SDL_GetError()
			);
			ReleasePbr(made);
			return slot.Albedo != nullptr;
		}

		ReleasePbr(slot);
		slot = made;
		return true;
	}

	graph::NodeScope
	Renderer::Impl::ResourceScope(const NamedPipeline &pipeline, graph::ResourceId resource) const {
		graph::NodeScope found = graph::NodeScope::Frame;
		for (uint32_t value = 1; value <= pipeline.Graph.Count(); value++) {
			const graph::Node *node = pipeline.Graph.Find(graph::NodeId{value});
			if (node == nullptr ||
				std::find(node->Writes.begin(), node->Writes.end(), resource) == node->Writes.end()) {
				continue;
			}
			if (node->Scope == graph::NodeScope::View) {
				return graph::NodeScope::View;
			}
			if (node->Scope == graph::NodeScope::World) {
				found = graph::NodeScope::World;
			}
		}
		return found;
	}

	Renderer::Impl::NamedTexture Renderer::Impl::FindGraphTarget(
		const NamedPipeline &pipeline, core::Name resource, graph::NodeScope scope, uint64_t owner
	) const {
		for (const GraphTarget &target : GraphTargets) {
			if (target.Pipeline == pipeline.Name && target.Resource == resource && target.Scope == scope &&
				target.Owner == owner) {
				return NamedTexture{target.Texture, target.Width, target.Height, target.Format};
			}
		}
		return {};
	}

	Renderer::Impl::NamedTexture Renderer::Impl::EnsureGraphTarget(
		const NamedPipeline &pipeline,
		graph::ResourceId resource,
		uint64_t owner,
		uint32_t viewWidth,
		uint32_t viewHeight
	) {
		const graph::ResourceDesc *desc = pipeline.Graph.FindResource(resource);
		if (desc == nullptr || desc->Kind == graph::ResourceKind::Texture ||
			desc->Kind == graph::ResourceKind::Buffer || desc->Kind == graph::ResourceKind::Camera ||
			desc->Kind == graph::ResourceKind::Entities) {
			return {};
		}

		bool presentationImage = false;
		for (uint32_t value = 1; value <= pipeline.Graph.Count(); value++) {
			const graph::Node *writer = pipeline.Graph.Find(graph::NodeId{value});
			if (writer == nullptr ||
				std::find(writer->Writes.begin(), writer->Writes.end(), resource) == writer->Writes.end()) {
				continue;
			}
			presentationImage = writer->Kind == core::Name("present") ||
								writer->Kind == core::Name("interface") ||
								writer->Kind == core::Name("overlay");
			break;
		}
		if (desc->External && !presentationImage) {
			return {};
		}
		const SDL_GPUTextureFormat format = presentationImage ? ColourFormat() : DeviceFormat(desc->Format);
		if (format == SDL_GPU_TEXTUREFORMAT_INVALID) {
			ENGINE_WARN("graph resource '{}' uses a format the renderer cannot allocate", desc->Name.Text());
			return {};
		}

		uint32_t width = 0;
		uint32_t height = 0;
		desc->Resolve(viewWidth, viewHeight, width, height);
		const graph::NodeScope scope = ResourceScope(pipeline, resource);
		for (GraphTarget &target : GraphTargets) {
			if (target.Pipeline != pipeline.Name || target.Resource != desc->Name || target.Scope != scope ||
				target.Owner != owner) {
				continue;
			}
			if (target.Texture != nullptr && target.Format == format && target.Width == width &&
				target.Height == height) {
				return NamedTexture{target.Texture, width, height, format};
			}
			if (target.Texture != nullptr) {
				gpu::ReleaseTexture(Device, target.Texture);
				target.Texture = nullptr;
			}

			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = format;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			if (desc->Kind == graph::ResourceKind::Storage) {
				info.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			} else if (desc->Kind == graph::ResourceKind::Depth) {
				info.usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
			} else {
				info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
			}
			info.width = width;
			info.height = height;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			target.Texture = gpu::CreateTexture(Device, &info);
			target.Format = format;
			target.Width = width;
			target.Height = height;
			if (target.Texture == nullptr) {
				ENGINE_ERROR("graph target '{}': {}", desc->Name.Text(), SDL_GetError());
				return {};
			}
			return NamedTexture{target.Texture, width, height, format};
		}

		GraphTargets.push_back(GraphTarget{pipeline.Name, desc->Name, scope, owner});
		return EnsureGraphTarget(pipeline, resource, owner, viewWidth, viewHeight);
	}

	bool Renderer::Impl::EnsureBeams() {
		if (BeamTexture != nullptr) {
			return true;
		}

		// The shadow map's own sampler and format, so the two are read the same
		// way - and `EnsureShadow` is what makes both.
		if (!EnsureShadow()) {
			return false;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = DepthFormat;
		info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

		// **The world map's resolution for the whole atlas**, so one beam gets a
		// quarter of it in each direction. A beam covers one doorway rather than
		// a scene, so a quarter of the texels over a hundredth of the area is
		// several times the density the world map has.
		info.width = SHADOW_RESOLUTION;
		info.height = SHADOW_RESOLUTION;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		BeamTexture = gpu::CreateTexture(Device, &info);
		if (!BeamTexture) {
			ENGINE_ERROR("portal beam texture: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	bool Renderer::Impl::EnsureShadow() {
		if (ShadowTexture != nullptr) {
			return true;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = DepthFormat;

		// **Both usages, and the sampler one is the point.** A depth attachment
		// that is only a target cannot be read, and a shadow map that cannot be
		// read is a pass that costs a draw and changes nothing.
		info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = SHADOW_RESOLUTION;
		info.height = SHADOW_RESOLUTION;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		ShadowTexture = gpu::CreateTexture(Device, &info);
		if (!ShadowTexture) {
			ENGINE_ERROR("shadow texture: {}", SDL_GetError());
			return false;
		}

		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;

		// **Clamped to the edge, and the fragment shader also range-checks.**
		// Either alone would do; both, because a wrap mode would tile the map
		// across the world and the range check is what makes "outside the map is
		// lit" a stated rule rather than a property of a sampler setting.
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

		ShadowSampler = SDL_CreateGPUSampler(Device, &sampler);
		if (!ShadowSampler) {
			ENGINE_ERROR("shadow sampler: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	bool Renderer::Impl::EnsureSurfaceSampler() {
		if (SurfaceSampler != nullptr) {
			return true;
		}

		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

		SurfaceSampler = SDL_CreateGPUSampler(Device, &sampler);
		if (SurfaceSampler == nullptr) {
			ENGINE_ERROR("surface sampler: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	bool Renderer::Impl::EnsureSurface(size_t viewport, size_t index, uint32_t width, uint32_t height) {
		if (index >= scene::MAX_SURFACES) {
			return false;
		}

		SurfaceSlotState &state = SurfacesAt(viewport).Surfaces[index];

		if (state.Texture[0] != nullptr && width == state.Width && height == state.Height) {
			return true;
		}

		if (!EnsureSurfaceSampler()) {
			return false;
		}

		// **Built beside what the slot already holds, and swapped in only once
		// all four exist.** Releasing first is what turned a device that could
		// not honour the new size into a pane with no texture at all: the slot
		// kept its old `Width`, so every later frame asked for the same size,
		// failed the same way, and the pane drew its own flat tint for the rest
		// of the run. A resize that cannot be made is a resize that does not
		// happen, and the picture the slot has is still a picture.
		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = ColourFormat();
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = width;
		colour.height = height;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = DepthFormat;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = width;
		depth.height = height;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;

		SDL_GPUTexture *made[2] = {nullptr, nullptr};
		SDL_GPUTexture *madeDepth = nullptr;

		const auto abandon = [&]() {
			for (SDL_GPUTexture *texture : made) {
				if (texture != nullptr) {
					gpu::ReleaseTexture(Device, texture);
				}
			}
			if (madeDepth != nullptr) {
				gpu::ReleaseTexture(Device, madeDepth);
			}
			// **True when the slot still has its old pair**, because the caller's
			// question is "may this surface be rendered", not "was it resized".
			return state.Texture[0] != nullptr;
		};

		for (SDL_GPUTexture *&texture : made) {
			texture = gpu::CreateTexture(Device, &colour);
			if (texture == nullptr) {
				ENGINE_ERROR(
					"viewport {} surface {} texture {}x{}: {}", viewport, index, width, height, SDL_GetError()
				);
				return abandon();
			}
		}

		madeDepth = gpu::CreateTexture(Device, &depth);
		if (madeDepth == nullptr) {
			ENGINE_ERROR(
				"viewport {} surface {} depth {}x{}: {}", viewport, index, width, height, SDL_GetError()
			);
			return abandon();
		}

		for (SDL_GPUTexture *&texture : state.Texture) {
			if (texture != nullptr) {
				gpu::ReleaseTexture(Device, texture);
			}
		}
		if (state.Depth != nullptr) {
			gpu::ReleaseTexture(Device, state.Depth);
		}

		state.Texture[0] = made[0];
		state.Texture[1] = made[1];
		state.Depth = madeDepth;

		// Resized, so whatever it held is gone. A mirror that showed the last
		// frame at the old resolution stretched across the new one would be a
		// visible artefact on exactly the frame a window was dragged.
		state.Ready = false;

		// And it owes a draw immediately rather than at the end of its next
		// interval, which is what a resized slot with a live stamp would do:
		// hold an empty texture for up to a frame's worth of cap while the pane
		// showed its own tint.
		state.Drawn = -1.0;

		state.Width = width;
		state.Height = height;
		return true;
	}

	Renderer::Impl::PortalTarget *Renderer::Impl::EnsurePortal(
		size_t viewport, uint32_t level, size_t index, uint32_t width, uint32_t height
	) {
		if (index >= scene::MAX_SURFACES || level >= MAX_PORTAL_DEPTH || width == 0 || height == 0) {
			return nullptr;
		}

		SurfaceBank &bank = SurfacesAt(viewport);
		if (bank.Portals.size() <= level) {
			bank.Portals.resize(level + 1);
		}

		PortalTarget &target = bank.Portals[level].Targets[index];
		if (target.Colour != nullptr && target.Display != nullptr && target.Depth != nullptr &&
			width == target.Width && height == target.Height) {
			return &target;
		}

		if (target.Colour != nullptr) {
			gpu::ReleaseTexture(Device, target.Colour);
			target.Colour = nullptr;
		}
		if (target.Display != nullptr) {
			gpu::ReleaseTexture(Device, target.Display);
			target.Display = nullptr;
		}
		if (target.Depth != nullptr) {
			gpu::ReleaseTexture(Device, target.Depth);
			target.Depth = nullptr;
		}
		target.Width = 0;
		target.Height = 0;

		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = ColourFormat();
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = width;
		colour.height = height;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Colour = gpu::CreateTexture(Device, &colour);
		if (target.Colour == nullptr) {
			ENGINE_ERROR(
				"viewport {} portal level {} slot {} colour {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			return nullptr;
		}
		target.Display = gpu::CreateTexture(Device, &colour);
		if (target.Display == nullptr) {
			ENGINE_ERROR(
				"viewport {} portal level {} slot {} display {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			gpu::ReleaseTexture(Device, target.Colour);
			target.Colour = nullptr;
			target.Display = nullptr;
			return nullptr;
		}

		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = DepthFormat;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = width;
		depth.height = height;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Depth = gpu::CreateTexture(Device, &depth);
		if (target.Depth == nullptr) {
			ENGINE_ERROR(
				"viewport {} portal level {} slot {} depth {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			gpu::ReleaseTexture(Device, target.Colour);
			gpu::ReleaseTexture(Device, target.Display);
			target.Colour = nullptr;
			target.Display = nullptr;
			return nullptr;
		}

		// **Shared with the surface path rather than a second one.** A portal
		// level is sampled exactly as a mirror's texture is.
		if (!EnsureSurfaceSampler()) {
			gpu::ReleaseTexture(Device, target.Colour);
			gpu::ReleaseTexture(Device, target.Display);
			gpu::ReleaseTexture(Device, target.Depth);
			target.Colour = nullptr;
			target.Display = nullptr;
			target.Depth = nullptr;
			return nullptr;
		}

		target.Width = width;
		target.Height = height;
		return &target;
	}

	Renderer::Impl::MirrorTarget *Renderer::Impl::EnsureMirror(
		size_t viewport, uint32_t level, size_t index, uint32_t width, uint32_t height
	) {
		if (index >= scene::MAX_SURFACES || level >= MAX_SURFACE_DEPTH || width == 0 || height == 0) {
			return nullptr;
		}

		SurfaceBank &bank = SurfacesAt(viewport);
		if (bank.Mirrors.size() <= level) {
			bank.Mirrors.resize(level + 1);
		}

		MirrorTarget &target = bank.Mirrors[level].Targets[index];
		if (target.Colour != nullptr && width == target.Width && height == target.Height) {
			return &target;
		}

		if (target.Colour != nullptr) {
			gpu::ReleaseTexture(Device, target.Colour);
			target.Colour = nullptr;
		}
		if (target.Depth != nullptr) {
			gpu::ReleaseTexture(Device, target.Depth);
			target.Depth = nullptr;
		}
		target.Width = 0;
		target.Height = 0;

		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = ColourFormat();
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = width;
		colour.height = height;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Colour = gpu::CreateTexture(Device, &colour);
		if (target.Colour == nullptr) {
			ENGINE_ERROR(
				"viewport {} mirror level {} slot {} colour {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			return nullptr;
		}

		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = DepthFormat;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = width;
		depth.height = height;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Depth = gpu::CreateTexture(Device, &depth);
		if (target.Depth == nullptr) {
			ENGINE_ERROR(
				"viewport {} mirror level {} slot {} depth {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			gpu::ReleaseTexture(Device, target.Colour);
			target.Colour = nullptr;
			return nullptr;
		}

		// The one sampler every projected image is read through, for
		// `EnsurePortal`'s reason: a scene of nothing but mirror levels would
		// otherwise take a null one into `SDL_BindGPUFragmentSamplers`.
		if (!EnsureSurfaceSampler()) {
			return nullptr;
		}

		target.Width = width;
		target.Height = height;
		return &target;
	}

	Renderer::Impl::SeamLightTarget *Renderer::Impl::EnsureSeamLight(size_t viewport, size_t index) {
		if (index >= scene::MAX_SURFACES) {
			return nullptr;
		}

		SeamLightTarget &target = SurfacesAt(viewport).SeamLights[index];
		if (target.Colour != nullptr && target.Depth != nullptr) {
			return &target;
		}

		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = ColourFormat();
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = SEAM_LIGHT_RESOLUTION;
		colour.height = SEAM_LIGHT_RESOLUTION;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Colour = gpu::CreateTexture(Device, &colour);
		if (target.Colour == nullptr) {
			ENGINE_ERROR("viewport {} seam light {} colour: {}", viewport, index, SDL_GetError());
			return nullptr;
		}

		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = DepthFormat;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = SEAM_LIGHT_RESOLUTION;
		depth.height = SEAM_LIGHT_RESOLUTION;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Depth = gpu::CreateTexture(Device, &depth);
		if (target.Depth == nullptr) {
			ENGINE_ERROR("viewport {} seam light {} depth: {}", viewport, index, SDL_GetError());
			gpu::ReleaseTexture(Device, target.Colour);
			target.Colour = nullptr;
			return nullptr;
		}

		target.Width = SEAM_LIGHT_RESOLUTION;
		target.Height = SEAM_LIGHT_RESOLUTION;
		return &target;
	}

	bool Renderer::Impl::EnsureOverlay(int width, int height) {
		if (OverlayTexture && width == OverlayWidth && height == OverlayHeight) {
			return true;
		}

		if (OverlayTexture) {
			gpu::ReleaseTexture(Device, OverlayTexture);
			OverlayTexture = nullptr;
		}
		if (OverlayTransfer) {
			gpu::ReleaseTransferBuffer(Device, OverlayTransfer);
			OverlayTransfer = nullptr;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = static_cast<uint32_t>(width);
		info.height = static_cast<uint32_t>(height);
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		OverlayTexture = gpu::CreateTexture(Device, &info);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size =
			static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * OverlayImage::BYTES_PER_PIXEL;
		OverlayTransfer = gpu::CreateTransferBuffer(Device, &transferInfo);

		if (!OverlayTexture || !OverlayTransfer) {
			ENGINE_ERROR("overlay texture {}x{}: {}", width, height, SDL_GetError());
			return false;
		}

		OverlayWidth = width;
		OverlayHeight = height;

		// A new texture holds whatever the driver had lying around. That did not
		// matter while every frame uploaded the whole image; now that a frame
		// uploads only the panels, everything outside them would be garbage
		// until something happened to draw over it. The next upload covers the
		// whole texture once to settle it.
		OverlayUninitialised = true;
		return true;
	}
}
