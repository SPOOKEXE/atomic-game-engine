# spatial — module invariants

L6, `shared` tier. A uniform hash grid over `{id, AABB, layers}` and the queries
that read it. `v02v03v04.md` §3.1 and §3.5 are the design; this file is what a
reviewer should refuse.

Its own module rather than part of `physics` because render culling wants the
same structure and must not have to link a solver to get it.

## This module does not know what an entity is, and only this file catches it

`Proxy::Id` is a `uint64_t`. It is whatever number the caller put in, and
nothing here dereferences it, resolves it, or assumes it means anything.

**An edge to `ecs` would pass every check the build has.** `ecs` is L3 and this
is L6, so the layer rule allows it. Both are `shared`, so `mono_check_all_tiers`
allows it. By rule 6 of the root `AGENTS.md` that makes it a convention, and this
paragraph is where the convention lives.

Refuse:

- `Engine::ecs` appearing in `CMakeLists.txt`, in any form and for any reason.
- `ecs::Entity` in a signature, even converted at the boundary. The conversion is
  the caller's, on both sides.
- A `Proxy` growing anything that only an ECS could fill in — an archetype, a
  row index, a component id.
- `scene::Collider` or any other component reaching a signature here. `scene` is
  L7 and above this; the direction of that edge is the whole reason `spatial`
  lands before `physics`.

`Engine::core` is the entire dependency list and it should stay that way.

## The grid is rebuilt, never edited

There is no `Insert`, no `Move` and no `Remove`, and adding one is not an
extension — it is a different data structure. Count-then-fill produces one flat
array with no per-cell vector and no per-tick allocation, which is what the
allocation table in `v02v03v04.md` states as a standing rule. An editable grid
needs a per-cell list with holes in it: it allocates, it fragments, and it
iterates in an order that depends on the history of the edits.

§3.5 of the same document says "O(1) move" and §3.1 says "insert, move, remove".
Those paragraphs describe the other structure. **They are descriptions and the
allocation table is a rule**, so the rule won. Do not resolve the contradiction
the other way without a measurement and a `release` number.

The cost is real and is admitted in the header: static geometry is re-measured
every rebuild. **The answer to that is a second grid**, holding the static
proxies and rebuilt only when the scene changes. It is not a mutable grid, and
it is not a "just this one" `Insert`.

## De-duplication is first-shared-cell, and a visited stamp is refused

A proxy spanning several cells appears in several buckets. It is reported from
the first cell of the walk that lies in both its own cell range and the query's
— and because every axis is walked ascending, that cell is the corner formed by
taking the larger minimum on each axis. One comparison, no scratch memory, and
the same answer whatever order anything runs in.

**A per-proxy visited stamp is the change to refuse.** It turns a query into a
write, which ends thread-safe querying, and it makes the result depend on which
thread reached a proxy first. A `mutable` member or an "it is only a debug
counter" is the same change wearing a hat.

The rule has three spellings and two of them are wrong. Report from the query's
first cell and a proxy in any later cell is never found. Report from the proxy's
first cell and a proxy that starts outside the query is never found. Both
failures are silent and both are covered by cases in `tests/HashGrid.cpp`.

## A bucket collision is a false positive and never a miss

A cell hashes to the same bucket in the build and in the query, so a proxy is
always looked for where it was put. What a collision costs is a candidate from
somebody else's cell, and there are two lines that throw it out: the entry
carries its cell coordinate, and every surviving candidate is re-tested against
its own box.

**Do not remove the box re-test on the grounds that the cell check already
narrowed it down.** They answer different questions. The cell check says "not
this cell"; the box test says "not this volume", and a cell is coarse — two
proxies in one cell need not touch each other or the query.

The entry has to carry its cell. Without it, a proxy whose *own* two cells
collide into one bucket is reported twice, which is the case that looks like a
de-duplication bug and is not one.

## `std::floor`, not a cast

A cast truncates toward zero, so -0.5 and +0.5 land in the same cell and the
cell at the origin is twice the width of every other. It loses nothing — the
build and the query agree — which is exactly why it survives review: it silently
doubles the busiest cell in every scene, because scenes are built around the
origin. `CellCoordinateOf` in `src/GridInternals.hpp` is the only place a world
coordinate becomes a cell, and it is the only place that may be.

## Two rebuilds of the same input iterate identically

Nothing in the build may depend on history. Bucket contents are placed in proxy
order by a cursor, oversized proxies are examined in proxy order after the
cells, and the cell walk is ascending on every axis.

A broad phase that visits pairs in a different order produces a different solver
result and a recorded run stops replaying — a long way from here, in
`just determinism`. Refuse anything that makes the order a function of the
previous contents: a hash seeded from a clock or an address, a bucket count that
grows rather than being chosen from the entry count, an early exit that leaves a
cursor where it stopped.

## A layer bit index is session-local

`LayerMask` is 32 bits and an index is a number. **Nothing here serialises one**
and nothing should: an index is derived from whatever order layers were
declared in, which is rule 4 of the root `AGENTS.md`. A save file, a wire format
or a manifest names a layer with a string and resolves it to an index once, at
load, in whatever module owns the naming — not in this one.

`LayerMask` is a struct rather than a bare `uint32_t` because a collider carries
two of them, the layer it is on and the set it collides against, and swapping
them at a call site compiles and returns a plausible wrong answer.
`LayerMask::Overlaps` is a **shared bit and not equality**; a default-constructed
mask is **empty and matches nothing**.

## Queries answer against boxes

A `Proxy` holds an `AABB` and that is the whole of what any query here knows
about a shape. A hit is a *candidate* and not a contact; a normal is the face of
an axis-aligned box and not the surface of anything.

Two consequences a reviewer should hold to:

- **`spatial::Raycast` and `physics::Raycast` both exist.** Each header comment
  says which it is. The second one calls this one for its candidates and then
  tests them exactly; picking the wrong one gives an answer that is a box away
  from right, which is the hardest kind to notice.
- **`ShapeCast` sweeps an axis-aligned box and only that.** `ShapeKind` is L7.
  Nothing at this layer holds a rotation, so there is no oriented sweep to be
  had here even in principle.

`v02v03v04.md` §3.7 asks for **"a raycast against a rotated box"**. That case
lives in `physics/tests/Query.cpp` and adding it here would test nothing: the bug it exists to
catch is a wrong inverse transform, and this module has no transform to invert.
A rotated case written against `spatial` would rotate a box, take its
axis-aligned bound, and then check an axis-aligned raycast against an
axis-aligned box.

## The two bounds, and why they are not tuning knobs

Both keep an allocation or a loop from being unbounded, and both are load-bearing
rather than preferences.

- **`HashGrid::MAXIMUM_CELLS_PER_PROXY`.** Past it a proxy stops producing
  entries and joins a short list every query examines directly. A baseplate two
  kilometres across would otherwise cost a quarter of a million entries per
  rebuild for one object, and an infinite or NaN box would ask for an unbounded
  allocation. Every query must consult the oversized list; one that forgets it
  misses the floor of every world, and there is a case per query for exactly
  that.
- **`WALK_CELL_ALLOWANCE`.** Past it a query scans every proxy instead of
  walking cells, because at that size the scan is both cheaper and bounded. The
  answer must not change between the two routes — only the route.

Raising either is a measurement, not an opinion.

## Not here yet, so do not add half of one

- **No `physics`.** `IntegrateMotion`, the narrow phase, the solver and the
  contact cache are L8 and are all in that module now. This one holds the index
  and the queries over it, and a contact list or a manifold appearing here means
  the layers have been inverted.
- **No frustum culling.** It is the second consumer this structure was made a
  module for, and it wants a `render` consumer that arrives at v0.6. A plane set
  and a half-space test with nothing calling them is surface with a maintenance
  cost and no benefit — §3.4's rule applied to this module.
- **No BVH.** Decision 4 chose the uniform grid on the grounds that a world is a
  bounded subarea. Replacing it is an *algorithmic* change, which by the
  allocation and algorithms rule needs a measurement taken in `release` and a
  number written into a comment. `benchmarks/HashGrid.cpp` is where that number
  would come from.
- **No walk along the ray.** A digital differential walk would visit fewer cells
  for a long raycast and would visit them in an order that is not the ascending
  one the de-duplication rule is built on. It is an optimisation with a rule
  change attached; take both together or neither.
- **No parallel query.** Every query here is a read, and two threads may run two
  of them against one grid — which is what the visited-stamp rule protects.
  Nothing here starts a job, because the grain for a raycast is a `physics`
  decision about how many rays it has, not a `spatial` one.
- **No `Vector2` and no 2D grid.** The same cells serve a flat world with one
  cell on Y.
