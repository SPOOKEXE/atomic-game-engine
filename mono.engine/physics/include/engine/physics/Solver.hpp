#pragma once

// Resolving the contacts, and telling the world what happened.
//
// Two steps of the `Phase::PostSimulation` chain, and they are here together
// because they are the two halves of one job: `Solve` decides what every
// velocity should become and `Publish` is the pass that makes it so.
//
// **The solver is serial, and that is a determinism requirement rather than a
// performance oversight.** Sequential impulse works by visiting contacts one
// after another and letting each one see the velocities the previous ones left
// behind — that is the whole method. Two threads visiting the same contact set
// in whatever order they got to it produce a different answer every run, and
// the run that differs is the one somebody recorded. `v02v03v04.md` §3.5 and
// decision 8 both say serial in as many words. **Do not "fix" this with a
// `Jobs::For`.** If contact solving ever has to be parallel, the change is
// graph colouring into independent batches with a fixed batch order, which is a
// different algorithm and needs its own measurement.
//
// **Sleeping lives here, and `scene::RigidBody` no longer carries a flag for
// it.** A body that has been still long enough loses its `scene::Motion`, which
// moves its row out of the dynamic archetype so `IntegrateMotion` and the
// dynamic half of the broad phase stop visiting it at all — the archetype move
// `v02v03v04.md`'s allocation table asks for, done with the components that
// already exist rather than with a tag no query could exclude. `AGENTS.md` in
// this directory carries the whole of that decision.
//
// @tier L8 · shared

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// How many times the solver sweeps the contact list.
	//
	// **Measured**, and from two directions, because the two disagree about
	// which way to move it. `v02v03v04.md` §3.6 asks for solver cost per
	// contact by name; this is the number that comes out of it.
	//
	// Cost is `benchmarks/Solver.cpp` in the `bench` preset, 3520 contacts over
	// 200 stacks of four, minimum sample with the spread beside it. Error is a
	// six-box tower dropped a millimetre apart and left for four seconds in the
	// `dev` preset, measured as the furthest any box ends up from the column it
	// started in — the number that says whether a stack stands up.
	//
	// | Iterations | Solve, 3520 contacts | Per contact | Tower drift | Bottom sink |
	// |---|---|---|---|---|
	// | 4 | 1276 us ± 18 | 0.36 us | 143 mm | 0.4 mm |
	// | 8 | 2073 us ± 21 | 0.59 us | 32 mm | 2.1 mm |
	// | 12 | 2859 us ± 56 | 0.81 us | 16 mm | 1.9 mm |
	// | **16** | **2891 us ± 73** | **0.82 us** | **13 mm** | **1.5 mm** |
	// | 24 | 4062 us ± 124 | 1.15 us | 24 mm | 0.7 mm |
	//
	// Sixteen, because the drift curve flattens between twelve and sixteen and
	// does not improve after — twenty-four is worse, which is a toppling tower
	// being chaotic rather than the solver getting worse, and either way it is
	// not an argument for paying forty per cent more. Below twelve the tower
	// visibly slumps.
	//
	// Cost per contact is flat across scene size — 0.82 us at 3520 contacts,
	// 0.82 at 18040, 0.83 at 10688 in taller stacks — so it really is a
	// per-contact figure and a scene's solver budget is a multiplication.
	//
	// **The warm start is what makes sixteen enough, and it costs nothing.**
	// The same benchmark with the impulse cache emptied every tick runs in
	// 2783 us ± 40, which is inside the spread: the lookup is a binary search
	// over a sorted array. What it buys is accuracy — the same tower drifts
	// 105 mm instead of 13 with the cache emptied, because every tick starts
	// its search from zero instead of from the answer the previous tick found.
	inline constexpr size_t SOLVER_ITERATIONS = 16;

	// How much two colliders may overlap before the solver is asked to care.
	//
	// Half a millimetre. Under it the contact is reported and solved but the
	// position correction leaves it alone, which is what stops a resting stack
	// being pushed apart and falling back every tick — the shiver that has no
	// visible cause. Contacts are reported below it because a stack that only
	// notices its neighbours once they have sunk half a millimetre falls half a
	// millimetre first.
	inline constexpr float PENETRATION_SLOP = 0.0005f;

	// How much of a penetration is corrected per tick, as a fraction.
	//
	// **The correction is a separate solve against a second set of velocities
	// that only ever move positions**, which is what lets this be eight tenths
	// rather than the two tenths a correction folded into the real velocity has
	// to settle for. Folded in, the correction is energy: the bodies leave the
	// contact faster than they arrived, so a stack bounces and — worse — a box
	// at rest reports a permanent upward velocity of one tick's gravity, which
	// no sleeping threshold can tell apart from a box that is genuinely
	// creeping.
	//
	// Kept a little under one so that a deep overlap unwinds over two or three
	// ticks rather than in a single jump, which reads as a shove.
	inline constexpr float POSITION_CORRECTION = 0.8f;

	// The fastest a penetration may be unwound, in metres per second.
	//
	// A body that has been teleported into a wall, or spawned inside one, has
	// a penetration measured in metres rather than millimetres, and eight
	// tenths of it in one tick is a shape crossing the screen. The cap turns
	// that into a firm push over several ticks. It bounds only the correction,
	// never the contact itself, so nothing passes through anything while it
	// unwinds.
	inline constexpr float MAXIMUM_CORRECTION_SPEED = 3.0f;

	// Below this closing speed a contact does not bounce.
	//
	// A metre a second. Restitution applied to the small closing speed gravity
	// produces in one tick is what makes a resting box hum in place forever,
	// and the threshold is the standard answer: a real impact is well above it
	// and a resting contact is well below.
	inline constexpr float BOUNCE_THRESHOLD = 1.0f;

	// The speed under which a body counts as still, in metres per second.
	inline constexpr float SLEEP_LINEAR_SPEED = 0.05f;

	// The turn rate under which a body counts as still, in radians per second.
	inline constexpr float SLEEP_ANGULAR_SPEED = 0.15f;

	// How long a body has to stay still before it is put to sleep, in seconds.
	//
	// Half a second, which is thirty ticks at the default rate. Long enough
	// that a box bouncing gently down a slope does not sleep between bounces,
	// short enough that a settled scene is quiet before anybody notices.
	inline constexpr float SLEEP_SETTLE_SECONDS = 0.5f;

	// How fast a neighbour has to be moving to wake a sleeping body.
	//
	// Deliberately above `SLEEP_LINEAR_SPEED`: a body that is itself about to
	// fall asleep must not keep waking the one it is resting on, which is a
	// pair that never settles and never stops costing a solve.
	inline constexpr float WAKE_SPEED = 0.1f;

	// Applies contact impulses to every body a manifold names.
	//
	// `Phase::PostSimulation`, after `NarrowPhase` and before `Publish`.
	// Gathers the bodies, sets up one row per contact point, warm-starts from
	// the previous tick's impulses and sweeps `SOLVER_ITERATIONS` times. Writes
	// `PhysicsWorld::Bodies` and leaves the components alone; `Publish` is what
	// touches the store.
	//
	// Trigger manifolds are gathered and never solved — a trigger reports and
	// applies no impulse.
	//
	// @param store The world to solve.
	// @tick
	void Solve(ecs::Store &store);

	// Writes the solved velocities back and emits the contact events.
	//
	// `Phase::PostSimulation`, last. Three jobs, all of them writes the earlier
	// steps deliberately did not make: `scene::Motion` for every body the
	// solver moved, `PhysicsWorld::Events` for every pair that began, persisted
	// or ended, and the archetype move for a body that fell asleep or woke up.
	//
	// @param store The world to write to.
	// @tick
	void Publish(ecs::Store &store);
}
