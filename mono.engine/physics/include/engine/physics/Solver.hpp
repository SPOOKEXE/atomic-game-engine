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
// behind - that is the whole method. Two threads visiting the same contact set
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
// dynamic half of the broad phase stop visiting it at all - the archetype move
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
	// started in - the number that says whether a stack stands up.
	//
	// The cost column was re-measured after the row layout changed in v0.14; the
	// error column is from the original run, because the change was arithmetic
	// - the same sweeps in the same order - and not an algorithm the drift
	// would answer differently.
	//
	// | Iterations | Solve, 3520 contacts | Per contact | Tower drift | Bottom sink |
	// |---|---|---|---|---|
	// | 4 | 819 us ± 87 | 0.23 us | 143 mm | 0.4 mm |
	// | 8 | 1118 us ± 129 | 0.32 us | 32 mm | 2.1 mm |
	// | 12 | 1473 us ± 128 | 0.42 us | 16 mm | 1.9 mm |
	// | **16** | **1785 us ± 226** | **0.51 us** | **13 mm** | **1.5 mm** |
	// | 24 | 2385 us ± 129 | 0.68 us | 24 mm | 0.7 mm |
	//
	// Sixteen, because the drift curve flattens between twelve and sixteen and
	// does not improve after - twenty-four is worse, which is a toppling tower
	// being chaotic rather than the solver getting worse, and either way it is
	// not an argument for paying a third more. Below twelve the tower visibly
	// slumps.
	//
	// Cost per contact is flat across scene size - 0.51 us at 3520 contacts,
	// 0.56 at 18040, 0.55 at 10688 in taller stacks - so it really is a
	// per-contact figure and a scene's solver budget is a multiplication.
	//
	// **The warm start is what makes sixteen enough, and it costs nothing.**
	// The same benchmark with the impulse cache emptied every tick runs in
	// 1619 us ± 114, which is inside the spread: the lookup is a binary search
	// over a sorted array. What it buys is accuracy - the same tower drifts
	// 105 mm instead of 13 with the cache emptied, because every tick starts
	// its search from zero instead of from the answer the previous tick found.
	inline constexpr size_t SOLVER_ITERATIONS = 16;

	// How much two colliders may overlap before the solver is asked to care.
	//
	// Half a millimetre. Under it the contact is reported and solved but the
	// position correction leaves it alone, which is what stops a resting stack
	// being pushed apart and falling back every tick - the shiver that has no
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
	// contact faster than they arrived, so a stack bounces and - worse - a box
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

	// The fewest contact rows a solve will cut into groups.
	//
	// **Below this the solve is one run in manifold order, which is what it was
	// before v0.17 and is bit-for-bit the same answer.** Partitioning costs a
	// pass over the bodies, a sort and a counting pass, and it buys nothing at
	// all when there is one group to be had; a scene of a dozen crates is every
	// scene in every suite in this module, and none of them should pay for a
	// machine they cannot use.
	//
	// **It is a row count and not a worker count, and that is the load-bearing
	// half.** Choosing the path from `Jobs::WorkerCount()` would make the
	// trajectory of a scene a function of the machine it ran on: a recording
	// taken with a pool and replayed without one would take the other path and
	// diverge, and nothing would say so. A row count is a property of the scene,
	// so the two runs agree.
	//
	// Two thousand is where `engine.physics.bench.solver` puts the crossover:
	// the partition costs about as much as one sweep of the rows it partitions,
	// and there are sixteen sweeps to win it back from.
	//
	// @since v0.17
	inline constexpr size_t PARALLEL_SOLVE_ROWS = 2048;

	// How many groups a partitioned solve aims to produce.
	//
	// **A constant rather than a multiple of the worker count**, for
	// `PARALLEL_SOLVE_ROWS`' reason: the chunk size is derived from this, the
	// groups are derived from the chunk size, and the row order is derived from
	// the groups - so a number that moved with the machine would move the
	// answer with it.
	//
	// Comfortably more than the widest pool this engine expects, because groups
	// are not the same size as each other: a chunk holding a pile has many times
	// the rows of one holding a wall, and a dispatch with one group per worker
	// finishes when the largest finishes. Several per worker is what lets the
	// short ones fill in around the long ones.
	//
	// @since v0.17
	inline constexpr size_t SOLVE_GROUP_TARGET = 64;

	// How many sweeps a worker does over its own group before rejoining.
	//
	// **A handover costs about as much as a sweep, so sixteen of them would
	// spend half the solve starting.** Measured on ten thousand boxes in
	// `engine.physics.bench.solver`, one dispatch of the groups is 2.9 ms of
	// worker time finished in 376 us of wall - of which roughly 190 us is
	// waking twenty-three threads and joining them again. Sixteen dispatches is
	// three milliseconds of that, against a solve that is eight.
	//
	// **It is sound because the groups are independent, not because it is
	// approximately sound.** No two groups name a body the solver may write, so
	// a worker sweeping its own group has nothing to hear from any other worker
	// between one sweep and the next. The only rows that couple two groups are
	// the border ones, and the barrier before those is the one that is kept.
	//
	// What it costs is how fast news crosses a chunk face: with four sweeps per
	// round there are four rounds rather than sixteen, so a stack straddling a
	// chunk boundary hears about the load above it four times instead of
	// sixteen. Every row still gets `SOLVER_ITERATIONS` sweeps.
	//
	// **Four, and both quality columns are what chose it.** Cost is
	// `engine.physics.bench.solver`'s tallest row in the `bench` preset - ten
	// thousand boxes in five hundred stacks of twenty, about fifty thousand
	// contact rows. Accuracy is `tests/SolverGroups.cpp`'s "a partitioned stack
	// still stands up" in the `ci` preset: a hundred and twenty-eight towers of
	// eight, two simulated seconds, drift being the furthest any box ends up
	// from the column it started in and sink being the furthest any box ends up
	// below where it started.
	//
	// | Sweeps | Rounds | Solve, 10k boxes | Drift | Sink |
	// |---|---|---|---|---|
	// | 1 | 16 | 8.45 ms | 142 mm | 0 mm |
	// | 2 | 8 | 7.72 ms | 110 mm | 0 mm |
	// | **4** | **4** | **7.57 ms** | **206 mm** | **0 mm** |
	// | 8 | 2 | 7.10 ms | 438 mm | 28 mm |
	//
	// **Sink is the column that decides it and drift is the one that looks like
	// it should.** A toppling tower is chaotic, so drift moves in both
	// directions for reasons that are not the solver - 110 mm at two sweeps is
	// below the sixteen-round figure and means nothing. Sink is convergence: it
	// is zero for as long as the contacts are being solved enough, and at eight
	// sweeps per round it is not. Eight is also where drift leaves the band the
	// other three sit in.
	//
	// Above four the curve is flat anyway - 7.10 ms against 7.57 is six per cent
	// for a stack that has started to sag.
	//
	// **Must divide `SOLVER_ITERATIONS`**, which a `static_assert` in `Solve`
	// enforces: it would otherwise silently give every row fewer sweeps than the
	// constant beside it promises.
	//
	// @since v0.17
	inline constexpr size_t SOLVE_SWEEPS_PER_BATCH = 4;

	// How many manifolds one worker takes at a time while their bodies are
	// located.
	//
	// Two binary searches over an array that has outgrown the cache, which is
	// two chains of dependent loads and almost no arithmetic. Wider than
	// `GATHER_GRAIN` because each index is cheaper.
	//
	// @since v0.17
	inline constexpr size_t LOCATE_GRAIN = 256;

	// How many manifolds one worker takes at a time while setting rows up.
	//
	// **Chosen against the shape of the body rather than measured to the row**,
	// which `parallel/AGENTS.md` allows for a body this size: setting up one
	// manifold is up to four contacts, each three prepared axes - six angular
	// responses and two cross products apiece - and a binary search over the
	// impulse cache. That is hundreds of nanoseconds, so the handover pays at a
	// few dozen and the default 4096 would refuse to dispatch a scene of ten
	// thousand contacts at all.
	//
	// Sixty-four rather than one because a range claim is an atomic and a
	// manifold is not expensive enough to be worth one each.
	//
	// @since v0.17
	inline constexpr size_t SETUP_GRAIN = 64;

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
	// Trigger manifolds are gathered and never solved - a trigger reports and
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
