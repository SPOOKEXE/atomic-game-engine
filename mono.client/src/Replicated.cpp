#include <engine/ecs/Components.hpp>

#include <client/Replicated.hpp>

namespace client {

	// The wire contract, checked at compile time rather than at join time.
	//
	// A snapshot writes a component's bytes through its `TypeDescriptor`, so a
	// receiver whose type is a different size reads a different number of bytes
	// and every entity after the first is misaligned. Catching that here is
	// cheap; catching it from a corrupted world three layers up is not.
	//
	// This is a check on *this* file against a number, not against the server's
	// header — the two programs do not include each other, which is the whole
	// problem `scene` at v0.4 solves. Three floats is the shape both sides
	// declare today.
	static_assert(sizeof(ReplicatedPosition) == sizeof(float) * 3);
	static_assert(sizeof(ReplicatedVelocity) == sizeof(float) * 3);

	void RegisterReplicatedComponents() {
		// Registered under the *server's* names. A component crosses by name and
		// the name is the server's to choose, so this is a mapping rather than a
		// declaration.
		engine::ecs::Components::Register<ReplicatedPosition>("server.Position");
		engine::ecs::Components::Register<ReplicatedVelocity>("server.Velocity");
	}
}
