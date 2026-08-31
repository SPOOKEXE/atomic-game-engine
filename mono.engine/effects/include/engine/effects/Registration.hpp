#pragma once

// Where this module's types get their names.
//
// The same two reasons `scene/Registration.hpp` gives, unchanged: registration
// order decides iteration order and therefore floating-point summation order,
// and a name crosses where an id does not.
//
// Both entry points are idempotent, so a program that registers, tears a
// universe down and builds another does not accumulate anything.
//
// @tier L8 · shared

namespace engine::effects {

	// Registers this module's components and the `ParticleSystem` resource.
	//
	// **`EmitterSlot` is registered with a writer that writes nothing and a
	// reader that clears.** A block index is a position in one process's pool -
	// rule 4's hazard exactly - so restoring it would point an emitter at whatever
	// block happened to take that number in the new process. `RefreshEmitters`
	// hands out a fresh block on the first frame after a load, which is the same
	// arrangement `engine::render::DrawList` uses and for the same reason: derived state
	// is recomputed rather than carried.
	void RegisterEffectComponents();

	// Registers `ParticleEmitter`, `Beam`, `Trail`, `Decal` and `Texture`.
	//
	// Calls `RegisterEffectComponents` first, because a class is a set of
	// component ids and cannot be declared before they exist. Also registers
	// `scene`'s tree, because all three derive from `Instance` and a second root
	// would be a tree scripts cannot walk.
	void RegisterEffectClasses();
}
