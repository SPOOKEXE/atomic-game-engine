#pragma once

// Beam sampling is independent of the viewer; facing and texture scroll are per-view work.
#include <engine/effects/Ribbon.hpp>

namespace engine::effects::beam_geometry {
	uint32_t PackRibbonRgba(const core::Color3 &colour, float alpha);
	core::Vector3 RibbonSideVector(const core::Vector3 &along, const core::Vector3 &toEye);
	BeamCadenceCache *PrepareBeamCache(ecs::Store &store);
	bool AppendBeamGeometry(
		ecs::Store &store,
		BeamCadenceCache *cache,
		ecs::Entity entity,
		const Beam &beam,
		const core::Vector3 &eye,
		float elapsed,
		std::vector<RibbonVertex> &output
	);
}
