#include <engine/scene/GpuParticleField.hpp>

#include <array>

namespace engine::scene {
	uint32_t NormalizeGpuParticleCount(uint32_t requested) {
		constexpr std::array<uint32_t, 5> PRESETS{
			262'144u, 1'048'576u, 4'194'304u, 16'777'216u, 50'000'000u,
		};
		if (requested == 0) return PRESETS[1];
		for (uint32_t preset : PRESETS) {
			if (requested <= preset) return preset;
		}
		return PRESETS.back();
	}

	bool HasGpuParticleLayer(const GpuParticleField &field, GpuParticleLayer layer) {
		return field.Enabled && (field.Layers & static_cast<uint8_t>(layer)) != 0;
	}
}
