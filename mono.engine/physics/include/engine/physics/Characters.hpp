#pragma once

// The half of a character controller that needs a query.
//
// **`scene` may not link this module** - `scene/AGENTS.md` refuses the edge in
// that direction - so `scene::Humanoid::Grounded` is a flag `scene::
// StepCharacters` reads and never computes. Somebody with a broad phase has to
// write it, and until now that somebody was a static function in
// `mono.client/src/Scene.cpp`: a server hosting the same world had no grounding
// at all, so a character on a dedicated server could never jump.
//
// This is the same split `replication::DistancePriority` already makes - the
// arithmetic in the shared module, the query in whatever can run one - put in
// the one module that can see both `scene` and a grid. One implementation, and
// a client, a server and the studio all install it with one call.
//
// @tier L8 · shared

#include <cstddef>

namespace engine::ecs {
	class Scheduler;
	class Store;
}

namespace engine::physics {

	// Writes `scene::Humanoid::Grounded` for every enabled humanoid.
	//
	// A downward ray from just inside the feet to just below them. **Starting
	// inside rather than at the surface**, because a ray that begins exactly on
	// a face is a coin flip about whether it hits it - and the coin lands
	// differently on two machines, which is a desync arriving through a
	// character controller.
	//
	// The humanoid's own body is rejected by comparing against the nearest hit
	// rather than by excluding it from the query, because `Raycast` filters by
	// layer and not by entity. That is right for the case that matters - a
	// humanoid standing on itself - and a character rig's limbs are on no layer
	// at all, so they are never candidates.
	//
	// @param store The world.
	// @return How many humanoids were tested.
	size_t GroundCharacters(ecs::Store &store);

	// Gives a body back to the simulation when its humanoid wants to move.
	//
	// **The half of a character controller that a sleeping body breaks.**
	// `physics::Publish` takes `scene::Motion` off a body the solver has put to
	// rest - the archetype move is what sleeping *is* here - and until now the
	// only thing that ever gave it back was a contact with an awake neighbour.
	// A character standing still therefore settled, lost its velocity column,
	// and could never be walked again: `scene::StepCharacters` had a perfectly
	// good move direction and nowhere to write it.
	//
	// Only characters with something to do are woken, because a still character
	// is exactly the body sleeping exists for.
	//
	// @param store The world.
	// @return How many were asleep and are not any more.
	size_t WakeMovingCharacters(ecs::Store &store);

	// Installs waking, grounding, stepping, portal crossing and posing, in that
	// order across phases.
	//
	// **Five passes and one call, because none of them is useful alone.** A
	// host that installed the step without the wake would have characters that
	// walk until they stand still and then never again; without the ground
	// query, ones that walk and cannot jump; without the pose, ones whose limbs
	// stayed where they spawned; without the crossing, ones that walk into a
	// portal and stop against it. The grouping is what stops a host getting four
	// of five right.
	//
	// **What is deliberately not here is the input.** `scene::
	// UpdateCharacterControl` turns a keyboard into a move direction, and a
	// dedicated server has no keyboard - so it is installed by whoever has one.
	//
	// @param scheduler The world's scheduler.
	void RegisterCharacterSystems(ecs::Scheduler &scheduler);
}
