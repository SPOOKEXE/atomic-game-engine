#include "RendererState.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/render/ImageGraphTransform3D.hpp>
#include <engine/render/LiveImagePublisher.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
namespace engine::render {
	SourceTextureStatus
	Renderer::SourceOutputStatus(core::Name owner, core::Name name, uint64_t generation) const {
		RequireOwningThread("SourceOutputStatus");
		if (!State || !owner.IsValid() || !name.IsValid() || !generation) return SourceTextureStatus::Absent;
		for (const auto &published : State->GraphResources.PublishedTransform3DOutputs)
			if (published.Owner == owner && published.Name == name && published.Generation == generation) {
				uint32_t width = 0, height = 0;
				if (State->Textures.SizeOf(name, width, height, owner)) return SourceTextureStatus::Ready;
			}
		for (const auto &slot : State->GraphResources.Transform3D) {
			if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Free || slot.Cancelled ||
				slot.Owner != owner)
				continue;
			if (slot.CameraRequest) {
				for (const auto &binding : slot.CameraBindings)
					if (!binding.Cancelled && binding.Name == name && binding.Generation == generation)
						return SourceTextureStatus::Pending;
			} else if (slot.Name == name && slot.Generation == generation)
				return SourceTextureStatus::Pending;
		}
		return SourceTextureStatus::Absent;
	}
	bool Renderer::StageSourceTexture(
		core::Name owner, const SourceTexturePublication &target, const assets::TextureData &texture
	) {
		RequireOwningThread("StageSourceTexture");
		ENGINE_PROFILE("imagegraph.skybox.stage-cpu");
		const auto stride = TextureUploadBytesPerPixel(texture.Format);
		if (!State || !owner.IsValid() || !target.Name.IsValid() || !target.Generation ||
			!texture.IsValid() || texture.Width > LiveImagePublisher::MAXIMUM_SIDE ||
			texture.Height > LiveImagePublisher::MAXIMUM_SIDE || texture.LevelCount() != 1 ||
			texture.FlipbookSide || !stride ||
			texture.Pixels.capacity() > imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES ||
			uint64_t(texture.Width) * texture.Height * *stride >
				imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES)
			return false;
		auto &published = State->GraphResources.PublishedTransform3DOutputs;
		const auto prior = std::find_if(published.begin(), published.end(), [&](const auto &item) {
			return item.Owner == owner && item.Name == target.Name;
		});
		if (prior != published.end() && prior->Generation >= target.Generation) return false;
		const size_t index = size_t(prior - published.begin());
		try {
			published.reserve(published.size() + (prior == published.end()));
		} catch (const std::bad_alloc &) {
			return false;
		}
		if (!AddTexture(target.Name, texture, owner)) return false;
		if (index == published.size())
			published.push_back({owner, target.Name, target.Generation});
		else
			published[index].Generation = target.Generation;
		return true;
	}
	bool Renderer::PromoteTextureGroup(
		core::Name sourceOwner, core::Name destinationOwner, std::span<const SourceTexturePublication> targets
	) {
		RequireOwningThread("PromoteTextureGroup");
		ENGINE_PROFILE("imagegraph.skybox.promote");
		if (!State || targets.size() != 6) return false;
		std::array<core::Name, 6> names{};
		for (size_t i = 0; i < 6; ++i) {
			if (!targets[i].Name.IsValid() || !targets[i].Generation ||
				SourceOutputStatus(sourceOwner, targets[i].Name, targets[i].Generation) !=
					SourceTextureStatus::Ready)
				return false;
			names[i] = targets[i].Name;
			for (const auto &published : State->GraphResources.PublishedTransform3DOutputs)
				if (published.Owner == destinationOwner && published.Name == names[i] &&
					published.Generation >= targets[i].Generation)
					return false;
		}
		auto &published = State->GraphResources.PublishedTransform3DOutputs;
		try {
			State->GraphResources.RetiredTextures.reserve(State->GraphResources.RetiredTextures.size() + 6);
			published.reserve(published.size() + 6);
		} catch (const std::bad_alloc &) {
			return false;
		}
		std::array<SDL_GPUTexture *, 6> retired{};
		if (!State->Textures.MoveBatch(sourceOwner, destinationOwner, names, retired)) return false;
		for (size_t i = 0; i < 6; ++i) {
			if (retired[i]) State->GraphResources.RetiredTextures.push_back(retired[i]);
			auto found = std::find_if(published.begin(), published.end(), [&](const auto &p) {
				return p.Owner == destinationOwner && p.Name == targets[i].Name;
			});
			if (found == published.end())
				published.push_back({destinationOwner, targets[i].Name, targets[i].Generation});
			else
				found->Generation = targets[i].Generation;
		}
		++State->ResourceEpoch;
		return true;
	}
}
