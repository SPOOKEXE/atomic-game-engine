#pragma once

// What makes things fall, which is a scene's business rather than physics'.
//
// **`Engine::physics` deliberately has no gravity and should not gain one.**
// Its own suites say why in as many words: a scene that wants weight applies
// it, and a top-down game should not have to switch one off. `RigidBody` has no
// gravity scale and the pipeline has no gravity step, so weight is a rule the
// world holds rather than a constant the solver assumes.
//
// **What was missing is that nothing applied it.** The physics module was
// complete, tested and connected to nothing for four versions — `D00039` — and
// wiring `RegisterPhysicsSystems` alone would not have made a single thing fall,
// because every body would have been integrated at zero acceleration for ever.
// The two halves are separate features and this is the second one.
//
// **A resource rather than a constant**, because it is authored: a world under
// water, on the moon, or seen from above wants a different vector, and one that
// wants none deletes the system rather than fighting a number.
//
// @tier L7 · shared

#include <engine/core/types/Vector3.hpp>

namespace engine::ecs {
	class Scheduler;
	class Store;
}

namespace engine::scene {

	// How fast a body gains speed, and in which direction.
	//
	// @since v0.13
	struct Gravity {
		// **Metres per second squared, down.** Earth's, because this engine
		// measures parts in metres — a part sized `2` is two metres — so the
		// number that makes a two-metre crate fall the way a two-metre crate
		// falls is the real one. Roblox's 196.2 is the same acceleration in
		// studs, and copying it here would make everything fall twenty times
		// too fast.
		core::Vector3 Acceleration{0.0f, -9.81f, 0.0f};
	};

	// Adds the gravity step to a world's scheduler.
	//
	// **`PreSimulation`, and the phase is the whole of the ordering argument.**
	// `physics.simulation` runs in `Simulation`, and a system sharing that phase
	// has no ordering against it — so gravity applied there is gravity applied
	// after the integrate on some ticks, which is one tick of fall lost each
	// time and reads as a body that is slightly too light.
	//
	// Applies to dynamic bodies only. A static or kinematic one is moved by
	// whatever owns it, and accelerating it here would fight that owner.
	//
	// **Does nothing in a world with no `Gravity` resource**, so installing the
	// system and authoring the weight are two decisions. `PrepareGravity` sets
	// the default.
	//
	// @param scheduler The scheduler to add to.
	// @since v0.13
	void RegisterGravitySystem(ecs::Scheduler &scheduler);

	// Gives a world Earth's gravity, if it has none.
	//
	// **Idempotent and non-overwriting**, so a host may call it on every world
	// it opens without undoing a value a game file authored.
	//
	// @param store The world.
	// @since v0.13
	void PrepareGravity(ecs::Store &store);
}
