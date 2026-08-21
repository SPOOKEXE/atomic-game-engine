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

	// Pulls the camera in front of whatever stands between it and its subject,
	// and fades that one thing so the player can still see past it.
	//
	// **Roblox's poppercam, and the same reason it lives here as
	// `GroundCharacters` does.** `scene::PlaceCamera` cannot ask a query
	// whether the eye it just computed is inside a wall - `scene` may not
	// link this module - so the query and the placement it corrects have to
	// meet somewhere both are visible, which is `physics`.
	//
	// **Writes `scene::CameraController::OccludedDistance`, never `Distance`
	// itself.** The player's own zoom setting must survive being pushed in
	// by a wall and pulled back out the moment it clears - see that field's
	// own header for why a second one exists rather than a temporary
	// overwrite.
	//
	// **Fades the blocker rather than hiding it**, through
	// `scene::SetLocalTransparency` - client-only and never sent, so a wall
	// thinned out for one viewer's camera is not thinned out for anyone
	// standing on the other side of it. Exactly one blocker is faded at a
	// time; the previous frame's is cleared first if a new tick names a
	// different one or none at all, so nothing stays translucent after the
	// camera has moved past it.
	//
	// **A part tagged `IgnorePoppercam` is looked straight through**, up to
	// a handful of times, so an author can mark a canopy or a low ceiling
	// nobody wants the camera fighting. `Raycast` itself refuses a general
	// ignore list - see its own header - so this is a loop over single-hit
	// casts rather than a filtered one.
	//
	// A no-op wherever there is nothing to place a camera *for*: no
	// `CameraController`, no `ActiveCamera`, no subject, `Scriptable`, or
	// disabled. `LockFirstPerson` is also left alone - the eye already sits
	// at the head, and there is nothing between a point and itself to be
	// occluded by.
	//
	// @param store The world.
	// @return `true` when a blocker's fade or the occluded distance changed.
	// @since v0.18
	bool UpdatePoppercam(ecs::Store &store);

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

	// Registers the components this module owns.
	//
	// **One, and it self-installed until v0.19.** `UpdatePoppercam` keeps a
	// resource holding the blocker it last faded, and it created that resource
	// on first use - which meant the component was registered mid-tick, under
	// the compiler's spelling of a type declared in an anonymous namespace in
	// `Characters.cpp`. Registration order fixes component ids and ids fix
	// archetype iteration order, so a type first seen during a tick takes an id
	// decided by whichever world happened to reach that pass first.
	//
	// Nothing caught it because nothing could: `Components::Seal` is what
	// catches a late registration and it had no caller outside a test. The
	// programs seal now, and this is the registration that lets them.
	void RegisterCharacterComponents();

	// Removes the part of a character's commanded velocity that points into
	// something solid.
	//
	// **Composed into `character.control` after `scene::StepCharacters`**, which
	// is where it has to be: that pass writes the walk as an intent rather than
	// a force, so a wall has to be taken out of the intent before the integrator
	// acts on it. The solver alone cannot, because its answer is position
	// correction capped at 3 m/s and a default `WalkSpeed` is 16.
	//
	// Exposed for the suites rather than for a caller to sequence - the
	// registration above already puts it in the right place.
	//
	// @param store The world.
	// @return How many characters had a direction removed.
	size_t ClipCharacterVelocity(ecs::Store &store);
}
