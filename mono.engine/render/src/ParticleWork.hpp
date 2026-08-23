#pragma once

// Flat device work for particle simulation.
//
// One entry names one resident state row and its emitter block. The compute
// shader dispatches these entries as ordinary 64-wide lanes, so thousands of
// six-particle emitters do not each consume a mostly empty workgroup.

#include <cstdint>

namespace engine::render {

	struct ParticleWorkItem {
		uint32_t Block = 0;
		uint32_t StateRow = 0;
	};

	constexpr uint32_t ParticleWorkgroups(uint32_t workItems) {
		return (workItems + 63u) / 64u;
	}
}
