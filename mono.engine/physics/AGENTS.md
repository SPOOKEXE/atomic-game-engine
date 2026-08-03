# physics — module invariants

L8, `shared` tier. Shapes, integration, the two indexes that answer "which
colliders might be touching", the six exact pairs that decide whether they
really are, the solver that pushes them apart, and the queries game code asks.
`v02v03v04.md` §3.5 and its v0.4 allocation table are the design; this file is
what a reviewer should refuse.

**The whole pipeline is here.** Integrate, sync, broad phase, narrow phase,
solve, publish — the six rows of the §3.5 table — plus `Raycast`, `OverlapBox`,
`OverlapSphere` and `ShapeCast` against real colliders. The last section of this
file lists what is still absent and why each absence is deliberate rather than
unfinished.

## L8 is shared with `assets`, and that was confirmed

`v02v03v04.md` §3.1 puts this module at L8 and says in the same paragraph that
the height was free when the table was written and `assets` has since taken it.
It asks for the sharing to be confirmed rather than inherited. It is confirmed,
on these grounds:

- Sharing a height is already the norm rather than an exception —
  `replication`, `input` and `render` all sit at L12.
- The two are **disjoint**. Nothing here depends on `assets` and nothing in
  `assets` depends on this, so there is no edge between them to be ambiguous
  about.
- Every dependency this module declares is strictly below L8: `scene` at L7,
  `spatial` at L6, `ecs` at L3, `parallel` at L2, `core` at L0/L1. The stack is
  still a stack.

A reviewer should refuse a change that gives this module an edge to `assets`, in
either direction, without moving one of them first. A same-height edge is the
one the layer rule cannot adjudicate, which is exactly why two modules at one
height have to stay disjoint.

## The direction of every edge, and the one the build cannot check

`core`, `parallel`, `ecs`, `spatial` and `scene`. All five are `shared`, so
`mono_check_all_tiers` catches none of them — by rule 6 of the root `AGENTS.md`
that makes the heights a convention, and this is where it is written down.

Refuse:

- **An edge from `scene` to this module**, in any form. `scene/AGENTS.md`
  already refuses it from the other side and names the exact shape it would
  take: a `Collider` that acquired a contact list "just for now".
- **An edge from `spatial` to this module or to `scene`.** `spatial` does not
  know what an entity is and must not learn.
- **An edge to `world`.** L4 is below this and the edge would therefore be
  legal, and it would still be wrong for the reason `scene/AGENTS.md` gives:
  physics takes an `ecs::Store &` and nothing larger, so a physics test does not
  have to stand a universe up. A world's tick calls these systems; they do not
  call it.
- **A vendor library.** Decision 3 is an exact analytic narrow phase written
  here. There is no physics engine behind this module and there is not going to
  be one.

## Nothing here reads a clock, a thread id or a pointer address

`v02v03v04.md` §2.4 and §3.5: same binary, same platform, same result. Four
concrete refusals, and each of them is a change that would compile and pass
every existing test:

- **No unordered container iterated anywhere in a system.** A lookup is fine; a
  walk is not, because the bucket order is a function of the allocator.
- **No wall clock.** The delta is `Store::Time().Delta`, the fixed tick. A
  system takes no float argument precisely so nobody can hand it a frame time.
- **No pointer address in a sort key, an id or a hash.** Addresses differ
  between runs under ASLR.
- **No thread id anywhere.** `EachParallel` partitions rows and each range
  writes only its own; a body that behaved differently depending on which
  worker got it would make the answer depend on the schedule.

**The pair sort is load-bearing and is not a tidiness measure.** Sequential
impulse is order-dependent, so a solver visiting contacts in grid-walk order
gives a different trajectory for the same scene built two ways. `just
determinism` and `just replay-check` are the checks; they fail a long way from
here, which is why the sort is a documented property of `PhysicsWorld::Pairs`
rather than an implementation detail of `BroadPhase`.

## `Collider::Extent` means one thing, and `Shapes.hpp` is where it is written

Box half-extent per axis; sphere radius in X; cylinder radius in X and
half-height in Y. **Half, never full.** Reading it as a full extent produces a
world exactly twice the size and reads as a physics tuning problem rather than
as a units mistake.

Two rules follow, and both have a case in `tests/Shapes.cpp`:

- **A second reading of `Extent` anywhere is the change to refuse.** The AABB
  derivation and the shape's own definition must agree, and the test that pins
  it compares the two against each other rather than against a literal — so a
  literal that was updated on one side and not the other cannot pass.
- **The components a shape does not use are not read.** A sphere's `Extent.Y`
  is whatever the author left there. Deriving anything from it makes an
  ellipsoid out of a typo, and adding an ellipsoid is a change to
  `scene::ShapeKind` — which costs a narrow-phase pair against every other
  shape, which is what `Enums.hpp` means by calling the set closed.

**The world bound is exact per shape and not one oriented box for all three.**
A sphere does not grow when it turns and a tilted cylinder is narrower than the
box around it. The loose version is *correct* — a bound too large costs
candidates and never drops a contact — so it will pass every test that is not
looking for it, and it hands the broad phase a sphere 73 per cent too wide. The
direction that matters is the other one: a bound smaller than the shape drops
contacts and reports nothing at all.

## The index is built from `Collider`, and the design note says `Bounds`

`v02v03v04.md` §3.5 says the world AABB comes from `Transform` + `Bounds`. It
does not, here, and this is the departure recorded rather than left for somebody
to find.

`Bounds` is the extent a thing is **drawn** at and `Collider::Extent` is the
shape it **collides** as. They are the same number for a `MakePart` box and
nothing keeps them so. An index built from the first can be smaller than the
shape in the second, and a broad phase whose bound is too small drops contacts
silently — the exact failure `core::AABB::FromOrientedBox` was written to avoid.

Querying `<Transform, Collider>` also excludes anything that does not collide,
where every part has a `Bounds`.

Refuse a change back to `Bounds` that does not also explain what happens when
the two disagree.

## Two indexes, because the grid is rebuild-only

`spatial::HashGrid` has no `Insert`, no `Move` and no `Remove`, and
`spatial/AGENTS.md` is explicit that adding one is a different data structure
rather than an extension. So the allocation table's "only re-insert what moved"
is **not a call this module can make** — it is a decision about what to hand to
`Rebuild`, and the answer `spatial/AGENTS.md` names is a second grid holding the
static proxies.

- **The dynamic index is everything with a `scene::Motion`**, rebuilt every
  tick. `MakePart` gives an anchored part neither `RigidBody` nor `Motion`, so
  static geometry is already a separate archetype and the split costs no branch.
- **The static index is everything else**, rebuilt when the static set changes.
  Measured at four thousand colliders, not rebuilding it saves 135 microseconds
  a tick, which is five times what the rest of the sync costs.
- **A sleeping body is in the static set**, because `Publish` takes its
  `scene::Motion` away. That is the archetype move sleeping is built on, and it
  means a scene settling costs one static rebuild per body that drops out —
  paid once each, while the count of dynamic rows falls.

Staleness is decided by two gates, and a reviewer should refuse a change that
removes either:

- `Store::ChangeVersion()`, which only moves for a write through `Set` to an
  observed component. An unchanged counter means nothing authored has happened
  and the changed-row walk is skipped. **This is what makes the property hold in
  a store whose dirty bits nobody clears** — a bare `Scheduler` never calls
  `ClearChanges`, so `EachChanged` alone would report the same rows forever and
  rebuild every tick.
- the changed-row walk itself, over `Transform` and `Collider`, skipping rows
  that have a `Motion`.

**A consumer calling `Store::MarkAllChanged<Transform>()` every tick defeats the
first gate**, because that claim covers the anchored rows too. The result is
correct and rebuilds every tick. That is why `IntegrateMotion` does not make the
claim, and it is the thing to check first when the static rebuild count starts
climbing with the tick count.

## `IntegrateMotion` loads no mass, and marks nothing changed

Two refusals that look like omissions:

- **No `RigidBody` in the query.** A platform, a projectile and a demo cube all
  move and none of them has a mass. Adding the term narrows the query to bodies
  that have one and loads three floats the arithmetic never reads — and it
  silently stops moving all three. The split between `scene::Motion` and
  `scene::RigidBody` exists for this one query.
- **No `MarkAllChanged<Transform>`.** See the section above: it would rebuild
  the static index every tick, forever. A consumer needing a replication delta
  out of an integrated world makes the claim in its own publish step.
  `mono.server`'s `Integrate` does it today and would have to carry it across if
  it ever defers to this system.

**Angular integration is real and the normalise is not optional.** A first-order
quaternion step leaves the rotation off the unit sphere and the error compounds;
a `CFrame` whose quaternion is not unit length *scales* what it transforms, so
the symptom is parts that slowly grow and nobody looks at the integrator.
Refuse a change that renormalises only past a threshold to save a reciprocal
square root — that is a data-dependent branch in the hottest loop of the tick.

## The steps are composed, not registered separately

`ecs::Scheduler` states that two systems in one phase have no ordering
guarantee. `SyncBroadphase` reading what `IntegrateMotion` just wrote is a hard
dependency, so `RegisterPhysicsSystems` adds **one system per phase** and calls
the steps in order inside it.

`v02v03v04.md` §3.5 lists them as separate rows in the same phase. That table
describes the steps, not their registration; relying on registration order
inside a phase is precisely what the scheduler's contract refuses. Each step
opens its own profiler span, so the overlay still separates them.

Refuse a change that splits them back into two registered systems in one phase
"because the table says so".

## The grain is measured and the default is wrong for this body

`INTEGRATE_GRAIN` is 1024, from `benchmarks/Integrate.cpp` in the `bench`
preset. `Jobs::DEFAULT_GRAIN` is 4096 and, through `Jobs::MINIMUM_GRAINS`, would
refuse to dispatch anything below 32768 rows — measured at twenty thousand
entities that is 73.5 microseconds against 27.3 for the same body.

**It was 512 until the build moved to `-O3`, and the reason it moved is the
reason this section exists.** A grain is a ratio between the cost of a row and
the cost of a handover, and only the first of those changed: the serial column
halved and the handover stayed at about 31 microseconds, so the crossover went
from ~4096 rows to ~8000 and the grain that puts the floor there doubled.
Nothing in the build noticed. `docs/DEFERRED.md` D00012 is the entry about that
failure mode.

Changing it is a measurement, not an opinion, and the numbers in
`Integrate.hpp` are re-taken from that suite rather than adjusted to match a
new value.

## Every buffer is cleared and never freed

The pair, manifold and event lists and the candidate scratch all live on the
`PhysicsWorld` resource and keep their capacity across ticks — the allocation
table in `v02v03v04.md` names exactly these. A steady scene stops allocating
after its first tick.

Every one of them now has a producer. `BroadPhase` clears the pair list, **the
narrow phase clears the manifold and event lists in its own step** — the event
list there rather than in `Publish`, so a world whose narrow phase ran and whose
solver did not cannot hand a reader last tick's events as this tick's — and
`Solve` clears the body and row lists. A reviewer should refuse one that
reallocates per tick; `tests/Broadphase.cpp`, `tests/NarrowPhase.cpp` and
`tests/Solver.cpp` each pin their own.

**Two of them are not rebuilt from scratch, and both are deliberate.** The
impulse cache is double-buffered and swapped, because a tick both reads last
tick's answer and records this one. The resting list is *merged* rather than
rebuilt: a body that fell asleep on static geometry produces no candidate pair
at all, so it is not gathered, and an entry rebuilt from this tick's bodies
would lose it and wake it for nothing.

**They live in the store and not in a module-scope vector.** Rule 2 of the root
`AGENTS.md`: a module does not keep private state another module reads, and
every one of these is read outside the function that filled it.

## `Proxy::Id` in these grids is an index, not an entity

Both indexes carry the index into `PhysicsWorld`'s own proxy and record arrays.
That is what makes resolving a candidate's masks an array subscript instead of a
store lookup — the cost an index exists to remove, and the reason
`ColliderRecord` exists at all.

The grids are therefore **not reachable from outside this module**, and a change
that publishes one is a change that hands a caller a `RayHit::Id` that is
plausible and wrong. If a caller needs the entity, the answer is a function here
that resolves it, not a widened header.

## The contact normal has one convention and one place that flips it

**The normal points from `A` toward `B`, `A` is the smaller entity id, and a
contact point lies on `B`'s surface.** `ContactManifold` says so, and every one
of the six pair functions obeys the same rule expressed in shape order rather
than entity order: from the first shape toward the second, points on the second.

`ContactBetween` is the only function in the module that reorders a pair and the
only one that reverses a normal. That is not tidiness. The pipeline names its
two bodies by entity id and the pair functions name theirs by `ShapeKind`, and
the two orders disagree for half of all pairs — so a flip written into each pair
function is six chances to get it wrong, and getting it wrong in two of six
reads as objects occasionally flying apart rather than as a sign error.

Refuse a second flip site. Refuse a pair function that reports its points on the
first shape "because it was easier there": the relation
`pointOnA = pointOnB + normal * penetration` is what the one flip relies on, and
a pair that breaks it makes the flip wrong for that pair only.
`tests/NarrowPhase.cpp` pins both halves.

## The cylinder axis sets are finite, and a cylinder is not a polytope

A box is a polytope, so its fifteen face and edge-cross axes are provably the
whole set and box-box is exact. A sphere is analytic against everything, and so
is sphere-cylinder — a closest point on a cylinder is a clamp along the barrel
and a clamp across it. **The two remaining pairs are the ones with a stated
limit.**

A cylinder is smooth, so its minimum-penetration direction can point anywhere
and no finite list of axes is complete for it. The lists in `ContactPairs.cpp`
cover every contact the design names — cap on face, barrel on face, cap on cap,
barrel on barrel crossed and parallel, box edge on barrel, box corner on barrel,
box corner on rim, rim on rim, rim on wall — and stop short of a box *edge*
meeting a cap's rim obliquely.

The direction that failure runs in is worth knowing: `ProjectionRadius` is exact
for whatever axis it is handed, so a missing axis never invents depth. It can
only miss a separating axis, which reports a shallow contact between two shapes
that are in fact a fraction of a millimetre apart. **Widening the set is a
change with a case attached**, and narrowing it needs an argument about which
resting configuration stops working.

## The solver is serial, and a reviewer should refuse `Jobs::For` in it

Sequential impulse works by letting each contact see the velocities the previous
ones left behind — that is the method, not an implementation detail. Two threads
visiting one contact list in whatever order they reached it give a different
answer every run, and the run that differs is the one somebody recorded.
`v02v03v04.md` §3.5 and decision 8 both say serial in as many words.

This was mutation-tested rather than assumed: replacing the sweep loop with two
threads over halves of the row list turns "two runs of one scene agree byte for
byte" red. **The same mutation written with `parallel::Jobs::For` does not**,
because a test binary has no worker pool running and `For` degrades to inline —
so a reviewer must not read a green suite as evidence that a `Jobs::For` in
there is safe.

If contact solving ever has to be parallel, the change is graph colouring into
independent batches with a fixed batch order. That is a different algorithm and
it needs its own measurement.

## The position correction is a second velocity that only moves positions

An overlap is unwound by a separate solve against `SolverBody::CorrectionLinear`
rather than by adding a Baumgarte term to the real velocity. Two reasons, and
the second is the one that bites:

- Folded into the real velocity, the correction is energy — the bodies leave the
  contact faster than they arrived, so a stack bounces.
- A box at rest then carries a permanent upward velocity of exactly one tick of
  gravity, because that is what the correction has to cancel. No sleeping
  threshold can tell that apart from a box that is genuinely creeping, and the
  threshold that could would be a function of the tick rate.

**It is translation only, and that is also deliberate.** A correction that turns
bodies has nothing damping it: the real solve never sees the rotation, so
nothing resists it, and a stack acquires a lean that grows every tick until it
slides apart. Measured on a six-box tower over four seconds, the angular half
costs 326 millimetres of drift against 40 without it. The price is that a
rotational overlap is pushed straight out rather than tipped out.

## Nothing here writes a `Transform` through `Store::Set`

`Publish` applies the position correction through the reference an `Each` hands
out, which `Store::Each` documents as a direct memory write. A write through
`Set` stamps the row, and `SyncBroadphase` reads those stamps to decide whether
*static* geometry moved — so a stamp left on a body that later falls asleep, and
therefore no longer has a `Motion` to exclude it from the static walk, rebuilds
the static index every tick forever.

That failure is silent, it only appears once something sleeps, and the counter
that reveals it is `PhysicsWorld::StaticRebuilds`. Refuse a `Set<Transform>`
anywhere in this module.

## Nothing here reaches `PhysicsWorld` through `Store::Resource` directly

`Store::Resource<T>()` **registers `T` as a side effect of looking for it**,
under the compiler's spelling, because the resource table is keyed by
`ComponentId` and getting one means calling `Components::Of<T>()`. So asking an
unprepared world whether it has a `PhysicsWorld` is what gives the type the
wrong name, and the next `RegisterPhysicsComponents` aborts the process because
a type may not have two names.

The abort is the lucky outcome. `mono.server/include/server/Simulation.hpp`
names the other one: a component registered under the compiler's spelling
produces a snapshot a differently-spelling process cannot read, and nothing says
so at the time.

Every read goes through `PreparedWorld` or `PreparedWorldMutable` in
`src/WorldResource.hpp`, which answers "is this world prepared" with
`Components::Find` — a name lookup that registers nothing — before it makes the
typed call. **`PHYSICS_WORLD_COMPONENT` is the one spelling of the name**, used
by the registration and by that lookup.

It is worth knowing how this hid. It needs a read to happen *before* any
registration, so in a shuffled suite it fires on some seeds and not others: it
was first reported as an unreproducible flake, at three failures in sixty runs,
and each failure aborted a different case. The deterministic reproduction
against a build with the bug is

    test_physics --order decl "a query against a world with no physics
    resource finds nothing,an overlap tests the shape and not the bound"

and `tests/Pipeline.cpp` and `tests/Query.cpp` both assert the invariant
directly rather than leaving it implied by a suite that happens to pass.

**A query is the entry point that makes this reachable outside a test**, because
it is the only one a program can call before `PreparePhysicsWorld`. The systems
are guarded the same way anyway, and a reviewer should refuse a
`store.Resource<PhysicsWorld>()` anywhere in this module.

**One typed access in this module is still not behind that guard, and it is
`scene`'s to own rather than this module's.** `IntegrateMotion` names
`scene::Transform` and `scene::Motion` in its query, so calling it on a store
where nobody registered the `scene` types registers them under the compiler's
spelling. The audit is short enough to state in full: every other typed access
here — every `Get`, `Each`, `Has`, `CountMatching` and the `SurfaceTable`
lookup — sits after a `PreparedWorld` guard, and the two `Observe` calls sit
inside `PreparePhysicsWorld`, whose first line is the registration.

Three things make `IntegrateMotion` a different case from the one above.
`Store::Set<scene::Transform>` registers the type too, so any store that has a
transform to integrate has already resolved the name one way or the other;
`scene/Registration.hpp` already owns the rule and says to register before
anything uses it; and `RegisterPhysicsComponents` registers the `scene` types
before its own precisely so a caller preparing physics cannot mint them wrongly.

Closing it properly needs a primitive the ECS does not have: a
`Components::Registered<T>()` that reads the per-type slot and adopts nothing,
so a module can ask about a type it does not own the name of.
`Components::Find` cannot serve — it takes a name, and physics has no business
spelling `scene`'s. That is a change to `ecs`'s public surface and is not one
this module should make on its own.

## `scene::RigidBody::Sleeping` is gone, and sleeping is an archetype move

The table in `v02v03v04.md` says a `Sleeping` **tag moves the row to another
archetype**, so the query never visits it. `scene::RigidBody` used to carry a
`bool Sleeping` beside it, described in its own comment as what the tag was
"derived from" — two answers to one question, the same state twice, and
readable only by making the visit the tag existed to avoid.

**The field is removed and the solver owns sleeping.** Three parts to the
decision:

- **A tag would not have worked.** `ecs::Store` has no "without this component"
  query term — `SyncBroadphase` already records that fact — so a query for
  `<Transform, Motion>` matches the archetype that also holds a `Sleeping` tag.
  The row would still be visited, and the tag would deliver none of the benefit
  that made it better than a branch.
- **Losing `scene::Motion` is the archetype move that does work.** A sleeping
  body drops out of `IntegrateMotion`'s query and out of the dynamic half of the
  broad phase, with the components that already exist. It is genuinely static
  for as long as it sleeps, which is also true physically.
- **How long a body has been still lives in `PhysicsWorld::RestingList`**, keyed
  by entity and sorted by it. That is per-world state in the store, not a
  module-scope vector, so rule 2 holds; `PhysicsWorld::Sleeping` is the one
  reader outside the solver and exists for a debug view.

Two consequences a reviewer should hold to. A body that fell asleep on static
geometry produces no candidate pair at all, so `Publish` reports that contact as
`Persisted` rather than `Ended` — the box did not leave the floor. And waking is
one pass over the manifolds in pair order, so a stack wakes one layer per tick:
bounded, deterministic, and visibly a settling stack rather than a scene
jumping at once.

Refuse a change that puts a sleeping flag back on a component, and refuse one
that wakes bodies by walking a set rather than the sorted manifold list.

## The queries are reads, and that is what lets a system raycast per entity

`physics::Raycast`, `OverlapBox`, `OverlapSphere` and `ShapeCast` take a
`const ecs::Store &`, write into a span the caller owns, and keep the grid
walk's scratch on the stack. All three of those are the same decision: the
design expects a system that casts a ray per entity, which means several workers
holding one world's index at once.

Refuse a change that gives one of them a mutable store or moves the candidate
scratch onto `PhysicsWorld`. It would compile, it would be marginally faster,
and it would end parallel querying without anything failing to build. The
scratch overflowing is reported through `QueryResult::Overflowed`, which is why
it can be a fixed stack array at all.

`RAYCAST_GRAIN` is 32 and is chosen rather than measured — `parallel/AGENTS.md`
says a query body wants a grain in the tens, and the default 4096 is a guess
about a body that does almost nothing. What a benchmark would refine is which
side of a hundred it sits.

**`ShapeCast` is conservative and says so.** It sweeps the moving shape's own
world bound and intersects candidates against that box exactly, so it never
misses and it can over-report for a shape whose bound is much larger than
itself. A first-time-of-impact sweep needs a distance function between two
convex shapes, which is in the list below.

## Not here yet, so do not add half of one

**An empty manifold list now means nothing touched.** It did not always: while
there was no narrow phase it meant nobody had looked, and both this file and
`PhysicsWorld` carried a warning saying so. The warning is gone because the gap
is, and `tests/PhysicsWorld.cpp` no longer holds the case that pinned it.

What is still absent:

- **Gravity.** There is no gravity row in §3.5, `scene::RigidBody` has no
  gravity scale, and a world with no down — an orbital simulation, a top-down
  game — should not have to switch one off. A host applies weight in its own
  `PreSimulation` system, which is what `tests/Behaviour.cpp` does. Adding a
  gravity term here is a change to the design note first.
- **A distance function between two convex shapes**, and therefore a
  time-of-impact sweep and continuous collision. `ShapeCast` is conservative
  instead and says so in its own comment. Adding one is where speculative
  contacts would come from too, and both want the same measurement.
- **Joints and constraints of any other kind.** The solver's row is a contact:
  a normal, two friction directions and a correction. A distance joint or a
  motor is a different row type and a different setup pass, and half of one is
  worse than none.
- **Continuous collision for fast bodies.** A body moving further than its own
  extent in a tick passes through a thin wall. `MAXIMUM_CORRECTION_SPEED`
  bounds the correction and not the motion, so it does not help here.
- **Island-based sleeping.** Waking propagates one contact layer per tick
  rather than flooding an island at once. That is bounded and deterministic;
  what it is not is instant, and a tall stack takes as many ticks to wake as it
  is high.
- **Lag-compensation rewind.** It needs a history of transforms per world, which
  is a storage decision and not a physics one.
- **Cross-platform determinism.** Not promised, and §3.5 says to document it as
  not promised. Same binary and same platform is the guarantee.
