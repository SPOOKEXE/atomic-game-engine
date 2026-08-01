# ecs — module invariants

L3. Storage, and nothing above it.

## The ECS owns the storage

This is the rule the whole layer exists to enforce, and it is the one most
often broken by accident:

> A module does not keep private vectors or dirty flags for data another module
> also reads.

If `render` needs to know which transforms moved, that is a component or a
change channel in here — not a `std::vector<bool> dirty` in `render`. Two copies
of the same fact drift apart the first time one of them is updated in a branch,
and the resulting bug reproduces once a week.

The rule was broken for the whole of v0.1 and it did not look like a violation
at the time. The client kept a `DemoScene` object holding the draw list, the
simulated clock and the scene extent, and registered systems that captured it.
It read as ordinary C++. What it actually meant was that half the world lived
outside the world:

- the affinity check did not cover it, so the one guarantee this layer makes
  about concurrent access simply did not apply to that half
- the profiler did not see it
- a second world could not have its own, because the systems closed over one
  particular object
- none of it would survive the world being serialised, replayed or migrated —
  which is the whole job of `world` at L4

Which is why there is now somewhere for it to go.

## Components are for what you iterate. Everything else is a resource

Two kinds of storage, and the choice between them is not a style question:

| | Component | Resource |
|---|---|---|
| How many | one per entity | one per world |
| Reached by | a query | a name |
| Costs | a column in every matching archetype | one slot |

> Componentise what you **iterate**. One-of-a-kind state is a resource.
> `GARG_ECS_Layout.md` §5

A camera as a component on a single entity buys an archetype, a query and a
loop that runs once, and turns "where is the camera" from a lookup into a
search. A `HalfExtent` that is the same four bytes on all 4096 entities is a
shared fact stored per-entity — it belongs in a resource, and the loop reads it
once instead of loading it per row.

**Resources live on a disabled entity**, so no query reaches them. A type used
as a component *and* as a resource therefore does not silently gain a row in
`Each<T>` — `tests/Resources.cpp` asserts it, and the assertion fails when the
`disable()` is removed.

## A system takes the world and nothing else

`System` is `void(Store &)`. Not `void(Store &, float)`, and not anything that
closes over a scene object.

**No delta parameter.** A delta handed in from outside is a delta that can be
the wrong one, and "this system got the frame time instead of the tick time" is
invisible at the call site and shows up as a simulation that behaves
differently at 30 fps and at 300. A system reads `store.Time()`, where `Delta`
and `FrameDelta` are separate fields with separate names. The bug is now a
typo rather than an argument order.

**Nothing to capture.** Everything a system needs is in the world, so a system
can be a plain function — which is what lets the L13 bindings register one, a
recording replay one, and a second world reuse one. A lambda capturing a `this`
can do none of those.

Only the store writes the clock: `AdvanceTick` and `SetFrame`. `Time()` returns
a copy, so a system cannot write to it by accident and cannot be left holding a
reference into a table that moved.

## Everything public here is a migration cost

Userland gets an ECS library at L13, bound from this layer. Whatever leaves
`include/` becomes something a game developer writes against, so the day it
changes it is a user migration rather than a refactor.

Keep the public surface small. If a thing is only needed by this module, it goes
in `src/` where nothing outside can reach it.

## `Store::Native()` is a debt, not a pattern

Wrapping the whole of flecs was not v0.1's job, so `Native()` exists and flecs
appears in the templates in `Store.hpp`. Two consequences:

- **Do not call `Native()` from another module.** Add the operation you need to
  `Store` instead. Every direct flecs call outside this module is one more
  thing the binding generator cannot see.
- **Search for it before adding one.** The name is deliberately awkward so that
  a grep finds every shortcut that was taken.

## There is no Count() of everything

Removed rather than fixed. The backing store's entity space also holds every
component registration, tag and builtin module, so a total is a number nobody
can act on — and the one that used to be here returned **zero** for the whole
of v0.1, because nothing ever called it and no test covered it.

Count what you can name: `CountMatching<Ts...>()`. Its query is built once per
type list per store and kept, so a system may call it every tick. That is a
recent change and it removed a real workaround — the server used to capture its
entity count at build time, because asking the store cost a fresh query per
call and became the most expensive thing in the tick.

## Affinity aborts, and it does so in release too

A store belongs to the thread that bound it. Every mutation checks. The check
stays on in every build because a data race that only appears under load on a
player's machine costs orders of magnitude more than a predictable branch.

It aborts rather than throwing. By the time the check runs, the race has already
happened; unwinding would hand the corrupted state to whoever catches, and the
stack at the moment of the violation is the only useful thing left.

## Phases are the ordering mechanism

Two systems in the same phase have no guaranteed order relative to each other.
That is deliberate. If the order between two systems matters, they belong in
different phases — adding "and register this one second" as a rule makes the
order invisible at the point where somebody breaks it.

## Not here yet

`Column`, `ComponentSet`, `SparseSet` and `ChangeChannel` are named in
`repo_layout.md` §5.2 and are not in this module yet. They arrive with v0.2's
multi-world work, where the storage layout has to be something the engine
controls rather than something flecs decides. Do not add half of one now.
