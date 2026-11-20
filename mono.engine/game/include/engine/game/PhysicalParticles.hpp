#pragma once

// Steps every `scene::PhysicalParticleField` in fixed point and collides its
// particles with anchored parts.
//
// Server and client run this same code on their own copy of the world and
// nothing crosses the wire. A sourced field steps on its weather's clock, and
// a particle's spawn is a pure function of the field's seed, that clock's tick
// and the weather sampled at that tick, so every machine holds the same
// particles. A machine that starts late replays the last `Lifetime` seconds of
// ticks to catch up, and reports no collisions from that replay.
//
// Collisions are logged to `scene::ParticleCollisionLog`, which scripts read
// as `OnParticleCollision`.
//
// @tier L12 · shared

#include <cstdint>

namespace engine::ecs {
	class Scheduler;
	class Store;
}

namespace engine::game {
	// Brings every record up to its clock's current tick. Creates records for
	// new fields and marks records of destroyed fields dying.
	void StepPhysicalParticles(ecs::Store &store);
	// Registers "particles.physical" in Animation, after the weather runtime
	// and the ground index when they are present.
	void RegisterPhysicalParticleSystem(ecs::Scheduler &scheduler);
}
