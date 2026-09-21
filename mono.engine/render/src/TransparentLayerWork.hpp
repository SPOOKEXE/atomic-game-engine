#pragma once

#include <engine/render/PortalExchange.hpp>

#include <cstdint>

namespace engine::render {
	constexpr bool CaptureIncludesWorldEffects(PortalImageScope scope, bool orderedLayers) {
		return scope == PortalImageScope::CompleteWorld || orderedLayers;
	}

	// Host-visible draw plan for one peeled layer. Every visual kind shares the
	// nearest pass and colour replay so their depth order is resolved together.
	struct TransparentLayerWork {
		uint32_t Meshes = 0;
		uint32_t Particles = 0;
		uint32_t Ribbons = 0;
		uint32_t InterfaceBatches = 0;

		constexpr bool HasSceneVisuals() const {
			return Meshes != 0 || Particles != 0 || Ribbons != 0;
		}

		constexpr uint32_t PhaseCount() const {
			return HasSceneVisuals() || InterfaceBatches != 0 ? 2 : 1;
		}
	};
}
