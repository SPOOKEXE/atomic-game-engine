#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Volume.hpp>

#include <algorithm>

namespace engine::scene {

	size_t ResolveVolumes(const ecs::Store &store, std::span<VolumeState> out) {
		size_t count = 0;
		auto &mutableStore = const_cast<ecs::Store &>(store);
		mutableStore.Each<const Volume, const Transform>(
			[&](ecs::Entity, const Volume &volume, const Transform &transform) {
				if (!volume.Enabled || count == out.size() || !(volume.HalfExtent.X > 0.0f) ||
					!(volume.HalfExtent.Y > 0.0f) || !(volume.HalfExtent.Z > 0.0f)) {
					return;
				}
				out[count++] = VolumeState{
					.Frame = transform.Frame,
					.Colour = volume.Colour,
					.HalfExtent = volume.HalfExtent,
					.Density = std::max(volume.Density, 0.0f),
					.Extinction = std::max(volume.Extinction, 0.0f),
					.Falloff = std::clamp(volume.Falloff, 0.0f, 1.0f),
					.NoiseScale = std::max(volume.NoiseScale, 0.001f),
					.NoiseStrength = std::clamp(volume.NoiseStrength, 0.0f, 1.0f),
					.Steps = std::clamp(volume.Steps, 1u, 64u),
					.ShadowSteps = std::clamp(volume.ShadowSteps, 1u, 32u),
					.Seed = volume.Seed,
					.Shape = volume.Shape,
					.Enabled = true,
				};
			}
		);
		return count;
	}
}
