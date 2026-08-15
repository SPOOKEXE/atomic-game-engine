# ecs - module invariants

L3. Storage, and nothing above it.

## The ECS owns the storage

This is the rule the whole layer exists to enforce, and it is the one most
often broken by accident:

> A module does not keep private vectors or dirty flags for data another module
> also reads.

If `render` needs to know which transforms moved, that is a component or a
change channel in here - not a `std::vector<bool> dirty` in `render`. Two copies
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
- none of it would survive the world being serialised, replayed or migrated -
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
shared fact stored per-entity - it belongs in a resource, and the loop reads it
once instead of loading it per row.

**Resources live on a disabled entity**, so no query reaches them. A type used
as a component *and* as a resource therefore does not silently gain a row in
`Each<T>` - `tests/Resources.cpp` asserts it, and the assertion fails when the
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
can be a plain function - which is what lets the L13 bindings register one, a
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

## A type has two serialisations, and `Save` may only ever see one of them

`TypeDescriptor::Write` and `Read` are the file format: a snapshot, a save, a
recording. `TypeDescriptor::Wire` is a second, **lossy** pair - a quantised
position, a rotation as smallest-three - that exists so a replication delta can
be a third of its size.

**Nothing in this module calls `Wire`, and `Store::Save` and `Store::Load` must
never start.** The build cannot check that, so by rule 6 it is written here.
What it costs to get wrong is specific rather than vague: a recording made
through a lossy codec still replays into an identical recording, so
`just replay-check` goes on passing while comparing one lossy file against
another - a check that is green and means nothing. `replication` is the only
caller, and `engine.replication.quantisation` asserts that a component with a
wire form saves and restores exactly.

The two are separate slots rather than one replaceable pair for the same reason,
and installing a codec *over* `Write` is the change to refuse however tidy it
looks.

**A wire form is installed by the same registration that names the type**, which
is what makes a server and a client agree about it without either being told.
The alternative - a table `replication` keeps by component name, filled in by
each program - makes agreement a discipline repeated in every program and every
test, and the failure mode of forgetting one is a receiver decoding ten bytes as
twenty-eight.

**A component holding an index into any table this module keeps needs a written
pair, and there are two of them.** `Components::Register<T>`'s generated
serialisation is the object representation, so a component holding a `core::Name`
writes an interning counter and one holding a `ClassId` writes a registration
index - both first-seen order within one process, both meaningless in a file or
on a wire. `InstanceName` has had a written pair since v0.8 for the first reason;
`InstanceClass` got one at v0.15 for the second, and writes the class's
registered name instead. Nothing makes two processes number their classes the
same way: `Classes::Register` runs wherever the code needing a tree runs, and
`gui`'s tree is registered lazily on first use.

`Hierarchy` is the one that genuinely may use the generated form, and the reason
is not "it is only handles" - it is that `Store::Load` restores the directory
*exactly*, index and generation alike, so an `Entity` inside a component still
names the same row. Nothing restores the class table that way.

## There is no escape hatch out of `Store`, and there used to be

`Store::Native()` was v0.1's debt: flecs was the backing store, wrapping all of
it was not that version's job, so an accessor existed that handed the underlying
world out and let a caller reach around the API. **It is gone - v0.2's storage
rewrite made the storage first-party, and flecs stopped being a dependency at
all.** There is no `Native()`, no vendored ECS, and nothing outside this module
can reach a row except through `Store`.

Recorded rather than deleted because the rule it existed to bound is the one
that still applies: **add the operation you need to `Store`.** The reason has
outlived the hatch - an access path the module does not own is one the binding
generator cannot see, which now means `v05.md`'s manifest rather than a
hypothetical one.

## There is no Count() of everything

Removed rather than fixed. The backing store's entity space also holds every
component registration, tag and builtin module, so a total is a number nobody
can act on - and the one that used to be here returned **zero** for the whole
of v0.1, because nothing ever called it and no test covered it.

Count what you can name: `CountMatching<Ts...>()`. Its query is built once per
type list per store and kept, so a system may call it every tick. That is a
recent change and it removed a real workaround - the server used to capture its
entity count at build time, because asking the store cost a fresh query per
call and became the most expensive thing in the tick.

## Affinity aborts, and it does so in release too

A store belongs to the thread that bound it. Every mutation checks. The check
stays on in every build because a data race that only appears under load on a
player's machine costs orders of magnitude more than a predictable branch.

It aborts rather than throwing. By the time the check runs, the race has already
happened; unwinding would hand the corrupted state to whoever catches, and the
stack at the moment of the violation is the only useful thing left.

## The index space is two ranges, and only one of them is the authority's

An entity is an index plus a generation, and two independently built stores both
start at index 0 generation 1. So an entity a replica minted for itself used to
collide *exactly* with one the authority minted, and `Store::Apply` was right to
merge them - it had nothing to tell them apart.

`SparseSet` now splits the index space in half:

| Range | Indices | Minted by |
|---|---|---|
| Authoritative | `[0, 2³¹)` - 2 147 483 648 | `Create`, `CreateInstance`, `CloneInstance` |
| Predicted | `[2³¹, 2³²−1)` - 2 147 483 647 | `CreatePredicted` |

`0xFFFF'FFFF` is `SparseSet::NO_INDEX`, the value a refused allocation hands
back, which is why the predicted side is one index shorter.

**Neither side ever wraps into the other.** A range that has issued everything it
owns refuses, `Create` returns `NULL_ENTITY` and says which range ran out.
Wrapping would reintroduce the collision at the one moment nobody is watching.

Three consequences worth knowing before touching this:

- **The directory is two page regions**, not one. A reserved base at 2³¹ under a
  single linear page list would allocate half a million pages to reach its first
  index. Each region pages exactly as the single one did, so the small-first-page
  rule holds for both - and a store that never predicts anything allocates
  **nothing** for the second region. `tests/SparseSet.cpp` pins that on
  `ResidentSlots`, in slots rather than pages, because the fee is bytes.
- **A snapshot writes the directory as one run per region**, which is what
  `SNAPSHOT_VERSION` 2 is. Anything walking `Capacity()` has to walk
  `PredictedCapacity()` too; `Store::EachEntity` is the one that would otherwise
  miss every prediction silently.
- **`Apply` never destroys a predicted entity.** "The sender did not mention it"
  is the definition of a prediction, so `ApplyMode::Authoritative`'s sweep skips
  the predicted range. Retiring a prediction is a promotion or a deliberate
  destroy.

`SetAdoptOnly` did not go away and still does its job: a replica may not mint an
*authoritative* entity, because that index is the authority's to hand out. What
changed is that minting a *predicted* one is legal, and the two are different
method names rather than a mode somewhere else.

## `Store::Promote` is a primitive, and the policy is deliberately absent

Promotion rewrites a predicted entity's identity to an authoritative one without
moving the row. **Nothing in this module decides when it is called**, which
authoritative handle it is called with, or what happens to a prediction the
server never confirms. That is the layer that predicts, and there is none yet -
the trigger is a projectile, which wants v0.4's physics and `Part`. `ROADMAP.md`
says the design should not be guessed at before its consumer exists, so this is
a convention rather than something the build checks: **do not add a promotion
policy here.** It belongs in `replication` when there is something to promote.

What promotion covers is the directory, the row's own copy of its handle, the
store's name maps, and the instance hierarchy around it. What it does **not**
cover is an `ecs::Entity` stored inside some other component: nothing in
`TypeDescriptor` says which of a component's bytes are entity handles, and a
byte-pattern search would rewrite an unrelated integer that happened to match.
Such a handle keeps the predicted value and reads as **dead** - the predicted
index's generation is bumped as it is freed - rather than naming a different
entity. A caller holding one rewrites it itself, because it is the layer that
knows the field is a handle.

## Phases are the ordering mechanism

Two systems in the same phase have no guaranteed order relative to each other.
That is deliberate. If the order between two systems matters, they belong in
different phases - adding "and register this one second" as a rule makes the
order invisible at the point where somebody breaks it.

## A column is chunks, and there is no whole-column base pointer

`Column::Data()` is gone. It stated the invariant this layout gives up - *one
base pointer and a stride reach every row* - and an API still offering it is how
a caller ends up walking off the end of chunk zero. It was deleted rather than
redefined so the compiler had to enumerate the callers; there were three, all in
`Store.cpp`, and two of them were `memset` and a per-row `Mark` over
`table.Rows()` from that base. Both are heap overruns the moment chunk zero stops
being the whole column, and neither is a compile error.

So: **anything reading a column as one buffer is wrong.** Walk the chunks -
`Column::ChunkData()` indexed by `Column::ChunkOf(row)`, or `Column::At` for one
row. The four `Visit*` templates in `Store.hpp` do it once per chunk and hoist
the base out of the row loop, which is worth 92% on `Each` over 10k rows and is
not decoration.

Three rules that are load-bearing rather than stylistic:

- **`VisitTables` yields one slice per table, never one per chunk.**
  `VisitBatchParallel` weighs the pool handover against `slice.Rows`, and
  `Jobs::For` refuses anything below `MINIMUM_GRAINS` grains - 32 768 indices at
  the default. A slice that was one chunk would put every dispatch under the
  floor and a large parallel iteration would silently run serially, a measured
  3.5x with nothing failing. The chunk split belongs inside the visitor and
  inside the worker body. `engine.ecs.parallel` has a case that goes red if it
  moves.
- **A run never crosses a chunk boundary.** `VisitChangedRuns` clips, because a
  run's contract is that `data + row * size` is the value for `entities[row]`
  across the whole run - and `Archetype::Ids` is one contiguous array while a
  column is not. An unclipped run sends one entity's id with another's bytes:
  every count still adds up and a client sees objects teleport.
- **Chunk boundaries are a function of the row index alone**, so every column in
  an archetype divides identically whatever its stride. That is what lets
  `EachBatch<Transform, Motion>` hand out two pointers over the same rows.

Chunks come from `ChunkPool` and go back to it as soon as the rows stop reaching
into them. The pool is process-wide because the shape it exists for is a thousand
worlds in one host, and it is **capped** because a pool that never trims holds the
same bytes under a different name. `ecs/docs/TODO.md` carries the measurements.

## The directory releases pages, and the epoch is why that is safe

`SparseSet` gives back every trailing page holding nothing live, and `Clear`
gives back all of them. `Free` itself still releases nothing on its own: dropping
a page there needs that page's indices purged from the LIFO free list, which is
an O(FreeList) pass inside the operation whose whole value is being O(1). Instead
the high-water mark comes back down with the pages and a free-list entry naming a
released index is discarded once, when it surfaces.

**A released page takes its generations with it, and that is the hazard.** A
recreated page starting again at `FIRST_GENERATION` would hand a reissued index
back at exactly the generation the oldest handles were issued with, and every one
of them would come alive. Each page index keeps an **epoch** - one past the
highest generation it ever issued - for as long as the directory exists, and a
recreated page starts every slot there. Do not remove it to save four bytes a
page: it makes the revival impossible by construction rather than by care.

## A described component is a component, and the layout is not the caller's

`Schemas::Register` builds a `TypeDescriptor` from a field list instead of from
a `T`, so a game can declare `Health` in a script and have it be a real
component - the same dense id, the same column, iterated by a C++ system that
never heard of the script. Three things about it are conventions the build
cannot check:

- **The layout is derived, and it has to stay derived.** Fields are sorted by
  alignment descending and then by name, so two processes handed the same field
  set lay it out identically. The caller is usually a script and a Luau table
  iterates in hash order - a layout that followed declaration order would differ
  between two runs of one file, and a snapshot written by one would not load in
  the other. Do not add a "keep my order" option.
- **Serialisation is field by field and must never become raw.** A `Name` field
  holds a process-local id, which `core/Name.hpp` says never to serialize, so
  the object representation `DescribeType` would have installed is exactly the
  bug that header warns about. Padding is zeroed at construction instead, which
  is the promise a derived layout can make in place of "there is no padding".
- **`Schemas::Clear` must not run while a world holds rows.** A column reaches
  its schema to destroy its own values, and clearing the table unpublishes the
  pointer the generated hooks read. It exists for tests and for a host tearing
  one universe down before building another.

The number of described components is **capped**, because the six lifetime
hooks are bare function pointers with nowhere to put a schema - one hook set is
generated per slot at compile time. `Schema.cpp` carries the number, the
measurements behind it and the argument for it; the refusal is
`Status::Exhausted` rather than silence.

**The hook bodies must stay out of line, and the macro that holds them there is
load-bearing rather than tidy.** `Thunks<N>` is meant to be a trampoline - load
the index, jump - and with the bodies inlinable the compiler put a copy of each
`PropertyType` switch into every one of them: the same file compiled to 113 MB
of object at 4096 slots. With `SCHEMA_OUT_OF_LINE` the table costs about 192
bytes of `.text` a slot, which is what makes the cap a number somebody picked
rather than a constraint. Removing the attribute would not fail a test; it
would make the build slow and the binary large, so it is written down here.

## Not here yet

`ComponentSet` and `ChangeChannel` were named in `repo_layout.md` §5.2 as things
this module would grow; both are here. Nothing from that list is outstanding.
