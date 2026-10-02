#pragma once
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	struct FluidSpawnParticle {
		Vector2 Position{}, Velocity{};
	};
	// Reset zeroes the source solver arrays before publishing a caller-owned domain.
	Status ResetFlipReplay(
		const FluidDomainSettings &settings,
		uint64_t tick,
		uint64_t revision,
		uint64_t maximumBytes,
		FluidDomainValue &result,
		Diagnostic &diagnostic
	);
	// Source spawning appends up to remaining capacity and preserves the existing simulation tick.
	Status SpawnFlipReplay(
		const FluidDomainValue &previous,
		std::span<const FluidSpawnParticle> particles,
		uint64_t maximumBytes,
		FluidDomainValue &result,
		Diagnostic &diagnostic
	);
	// Fixed contiguous source steps commit together; undefined source array accesses refuse atomically.
	Status StepFlipReplay(
		const FluidDomainValue &previous,
		uint64_t tick,
		uint64_t revision,
		uint64_t maximumBytes,
		uint64_t maximumWork,
		FluidDomainValue &result,
		Diagnostic &diagnostic
	);
}
