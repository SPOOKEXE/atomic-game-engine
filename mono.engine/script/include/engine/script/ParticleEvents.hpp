#pragma once

// What `OnParticleCollision` hands a script, shared by both languages.
//
// @tier L9 · shared

#include <engine/script/Codec.hpp>

#include <cstdint>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	struct ParticleCollision;
}

namespace engine::script {
	// Collisions logged since `cursor`, oldest first, and moves `cursor` past
	// them. A zero cursor starts at the log's end and returns nothing.
	// Records whose field is gone are skipped: nothing can be connected to it.
	[[nodiscard]] std::vector<scene::ParticleCollision>
	TakeParticleCollisions(const ecs::Store &store, uint64_t &cursor);

	// The handler's table, without the two instance fields (`Instance`, the
	// part hit, and `Field`), which each language adds itself:
	//
	//   Position, Direction, Velocity, Normal: Vector3
	//   Speed, Age: number (metres per second, seconds)
	//   ParticleId: number
	//   ParticleComponents: { Radius, Color, Response }
	[[nodiscard]] ScriptValue ParticleCollisionPayload(const scene::ParticleCollision &collision);
}
