#pragma once

// The contact half of a hole: what a body standing in one is standing on.
//
// **A picture and a contact have nothing to share but the seam**, which is why
// this is not `scene::CutAndCloneSeams` with a collider on it. That pass reads a
// draw list and answers "what does this look like"; this one walks bodies and
// answers "what pushes back". The two agree about which rectangle a body is in
// because both ask `scene::GatherPortalSeams`, and about nothing else.
//
// ## Which way round the map goes, and why it is not the obvious one
//
// The arrangement everybody reaches for first is a **kinematic twin**: place a
// copy of the body at `M(body)` on the far side, let the solver resolve it
// against the far room, and map every contact back through `M⁻¹` as an impulse
// on the original. It works, and it needs a second solver path - an impulse
// mapped by a rotation and a scale, applied to a body the contact was not
// generated for - in a module whose whole design is that there is one.
//
// **Mapping the other way needs none of it.** Copy the *far room's colliders*
// into the near room through the inverse seam. There they are ordinary static
// geometry standing exactly where the body's far half is, and the solver pushes
// the body with them in its own space, with its own mass, through the path every
// other contact takes. Nothing is mapped back because nothing crossed.
//
// It is the same trick a shadow through a hole wants: move the receiver into the
// caster's space rather than the caster into the receiver's.
//
// ## What it costs
//
// One overlap query per straddling body per tick, and a handful of entities
// created and destroyed inside the same tick. Both are zero on every tick where
// nothing is standing in a hole, which is nearly all of them - the pass returns
// at the first line in a world with no portal in it.
//
// **Created and destroyed rather than pooled**, which is the one thing here that
// would be worth measuring if a scene ever put a crowd in a doorway. An
// archetype move per proxy per tick is real; a pool that has to be invalidated
// whenever the far room moves is a cache, and a cache is what this module has
// spent four versions removing.
//
// @tier L6 · shared

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// Puts the far room's geometry into the near room, under anything standing
	// in a seam.
	//
	// **`PreSimulation`, before the broadphase syncs**, so a proxy is indexed on
	// the tick it exists for. `RegisterCharacterSystems` installs it beside
	// `scene::OpenPortals`, which is the pass that lets a body be in a pane at
	// all - without that one there is never anything to hold up.
	//
	// @param store The world.
	// @return How many proxies were placed. Zero on nearly every tick.
	// @since v0.15
	size_t GhostPortalBodies(ecs::Store &store);

	// Takes them away again.
	//
	// **`PostSimulation`, and unconditionally.** A proxy that outlived its tick
	// would be a piece of another room standing invisibly in this one, and the
	// body it was made for may have walked out of the seam - or through it - in
	// the tick that just ran. Rebuilding costs an overlap query; leaving one
	// costs a wall nobody can see.
	//
	// @param store The world.
	// @return How many were removed.
	// @since v0.15
	size_t RetirePortalProxies(ecs::Store &store);
}
