#pragma once

// Flat device work for particle simulation.
//
// One entry names one resident state row and its emitter block. The compute
// shader dispatches these entries as ordinary 64-wide lanes, so thousands of
// six-particle emitters do not each consume a mostly empty workgroup.

#include <engine/core/types/AABB.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/graph/Frustum.hpp>

#include <cstdint>

namespace engine::render {

	struct ParticleWorkItem {
		uint32_t Block = 0;
		uint32_t StateRow = 0;
	};

	// A conservative world-space box for every particle one emitter can draw.
	// `Cullable` is false when authored non-finite or amplifying values make a
	// finite upper bound impossible; callers must draw in that case.
	struct ParticleDrawBounds {
		core::AABB Bounds;
		bool Cullable = false;
	};

	ParticleDrawBounds BoundsForParticleDraw(
		const effects::EmitterBlock &block, const effects::EmitterSpawnState &spawn, float zOffset = 0.0f
	);

	bool ParticleDrawVisible(
		const core::AABB &bounds, bool cullable, bool cullingSafe, const graph::Frustum &frustum
	);

	constexpr uint32_t ParticleWorkgroups(uint32_t workItems) {
		return (workItems + 63u) / 64u;
	}

	// A presented revision advances the resident pool once. Rendering the same
	// revision again only reuses its output, while a failed submission carries
	// the unsubmitted time without charging the same revision twice.
	constexpr float ParticleStepDelta(
		uint64_t preparedRevision, uint64_t presentedRevision, float presentedDelta, float carriedDelta
	) {
		return carriedDelta + (preparedRevision == presentedRevision ? 0.0f : presentedDelta);
	}
}
