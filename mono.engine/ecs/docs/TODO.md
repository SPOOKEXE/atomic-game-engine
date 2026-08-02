# ecs — what is not built yet

What this module still owes, and to whom. Ordered by the version that needs it,
because "later" without a consumer is how a list like this stops being read.

An item here is a **commitment with a trigger**, not a wish. If nothing will be
blocked by its absence, it does not belong here — it belongs in
`docs/DEFERRED.md` or nowhere.

---

## Built

For orientation, since everything below is defined against it.

| Piece | What it is |
|---|---|
| `Entity` | index + generation, no pointer |
| `ComponentId` / `TypeDescriptor` | runtime type info: size, align, lifetime hooks, serialisation |
| `Components` | the process-wide type table, sealed after startup |
| `Column` | one type-erased contiguous array, swap-back removal |
| `ComponentSet` | interned sorted id set — archetype identity, and a class |
| `SparseSet` | paged entity directory: generation, liveness, location |
| `Archetype` (private) | one table: id array + one column per component |
| `Store` | the world: entities, components, resources, clock, queries, deferral |
| `ChangeChannel` | per-row dirty bits as a `DirtyBits` column |
| Change signals | `OnChanged<T>` / `FlushSignals`, fired at a phase boundary |
| Snapshot | `Store::Save` / `Load`, components by name, directory reproduced exactly |

---

## v0.2 — the rest of this version

### Chunked column storage with a shared span pool — **moved to v0.4**

The measurement this was waiting on got taken, and it moved the item twice: it
narrowed the trigger, and it found that most of what the trigger was blamed for
came from somewhere else entirely.

| Shape | Resident | Live rows | Ratio |
|---|---|---|---|
| 1000 worlds, always 100 entities | 72.7 MB | 2.7 MB | 26.6x |
| 1000 worlds, peaked at 10k, settled at 100 | 705.8 MB | 2.7 MB | 258x |

**The first row was not columns.** 64 MB of that 72.7 was `SparseSet` pages:
a page is allocated whole on a world's first entity, and it was 4096 slots of
sixteen bytes, so a world of a hundred entities paid a 64 KB entry fee. Fixed,
by making the first page 512 slots and leaving every page after it at 4096 —
**72.7 MB to 16.7 MB.** Shrinking all of them was tried and rejected: eight
times the allocations scatter a large world's columns and a multi-world tick
over 100k entities each measured 8–21% slower.

**The second row is what chunked storage is actually for**, and roughly two
thirds of it is column capacity that will not be used again. Still worth doing,
now with a number behind it and a narrower trigger: *a world whose population
falls a long way from its peak*, not *a world that is small*.

It goes to v0.4 because the vectorisable-component-layout item there reopens the
same bytes. Chunking makes `Column::At` a divide and a modulo; a vectorisable
layout changes what a row is; a chunk is the natural granularity for a
vectorised block. Sequenced separately, `Column`'s internals get rewritten twice
and the second rewrite invalidates the first's benchmark.

Compatible with the iteration paths as they stand — `EachBatch` already
promises nothing about where a batch ends, so one batch per chunk is a legal
division.

Explicitly **not** an occupancy flag per row. See "Rejected" below.

Take the directory's own leftover in the same pass: `Free` and `Clear` both keep
pages on purpose, so a world that shrank still holds every page it ever touched.
Bounded and small beside the column capacity next to it, which is why it is a
line here rather than an item.

### The class table and the instance model

- A class is a name, a parent class, and a `ComponentSet`.
- `:IsA` as an ancestor test, made O(1) by interval-numbering the class tree.
- **Prototype rows**: each class owns one hidden row of defaults, so
  `Instance.new` is a column copy and `:Clone()` is the same copy from a
  different source row.
- `Hierarchy { Parent, FirstChild, NextSibling, PrevSibling }` plus
  `InstanceName`. Organisational only — **not** a transform hierarchy, because
  `Transform` is world-space and nothing propagates.
- Property descriptors: `{class, name} -> {component, offset, value type}`.

### ~~Archetype edge cache~~ — built

The trigger was a measurement, and the measurement is
`benchmarks/Structure.cpp`. It reported a transition at **50.8 ns** against
**9.2 ns** for overwriting a component already present, with **28 ns** of that
being the intern alone — a vector allocation, a sort, an FNV hash and a
process-wide mutex, to recompute an answer that cannot change.

`ArchetypeEdges` (in `src/`, private like `Archetype` itself) caches add-one and
remove-one per table. **11.99 ms to 6.25 ms on 100k toggles.** A scanned list
rather than a hash map: an archetype has a handful of edges, and a hash of a
composite key would have cost roughly what it replaced.

The invariant to keep: an edge is valid only for the watch epoch it was recorded
under. `Observe` gives a table a `DirtyBits` column and therefore moves where a
transition lands, so an edge from before it sends the row to a table that does
not track changes — a write that goes unreported rather than a crash. `Watched`
is only added to through `WatchComponent`, which bumps the epoch beside it, so
there is one place to get that right rather than three.

---

## v0.3 — replication

### Delta extraction from `ChangeChannel`

The third reader of the dirty bits, after `.Changed` and render invalidation.
Wants `EachChanged` to yield a *column range* rather than a row at a time, so a
delta is a memcpy per run rather than a copy per entity.

### Snapshot into a **running** store

Today `Load` clears first. A client replica applies authoritative state into a
world that is already ticking and already holds entities the server also knows
about. That is a merge, not a replace: same entity, new values, no destroy and
re-create. `v02v03.md` §2.12 names this as the one capability worth reserving
now because it is expensive to retrofit.

### An index range for locally predicted entities

**Trigger: the client predicts anything that spawns.** Entity identity is an
index plus a generation, and two independently built stores both start at index
0 generation 1 — so an entity a replica creates for itself collides exactly with
one the authority created, and `Store::Apply` is right to treat them as the same
entity because it has nothing to tell them apart.

`tests/Replication.cpp` pins the current behaviour in *two stores allocate the
same indices, and apply cannot tell them apart*, so the day this is fixed the
test says so.

**Guarded at v0.3, moved to v0.4.** `Store::SetAdoptOnly` refuses to mint in a
replica and every store a `replication::Connector` writes into has it set, so
the collision cannot be walked into any more. `CreateAt` is untouched: this
refuses minting, not receiving. The pinned test still reaches the collision by
deliberately not setting the flag, so what it pins is the storage's behaviour
rather than a replica's.

The shape of the real fix: reserve a high index range that the authority never
allocates from, and give a replica's `Create` that range. A predicted entity
then has an identity the server can never mint, and promoting one to a server
entity is an explicit step rather than a coincidence.

**Why it is a v0.4 item and not a v0.3 one.** It changes the directory's layout.
A reserved base at 2³¹ under one linear page list would have `Reach` allocate
half a million pages to get there, so `SparseSet` needs a second page region —
and `SaveSnapshot` writes the directory as a run of `Capacity()` entries, so it
needs the same split and a format bump. v0.4 already reopens this storage for
chunked columns and vectorisable components, and one bump beats two. The trigger
is also still unmet: nothing can predict a spawn until there is a projectile to
predict, which wants v0.4's physics and `Part`.

### Interest filtering

A snapshot of *part* of a world — the entities one client can perceive. Needs a
predicate at the table and row level, and a way to say "this entity is not
missing, you simply cannot see it" so a client does not treat it as destroyed.

---

## v0.4 — components and physics

- `QuickHash`, the fallback for change detection over batch-written rows. Label
  it as second-best where it lands, or it will spread.
- Sleeping as an archetype move rather than a per-row branch: adding a
  `Sleeping` tag takes the row out of the dynamic query entirely.
- Column-level `min`/`max` summaries for broad-phase culling, if a measurement
  asks for them.

---

## v0.5 — script bindings

### Per-instance change signals

**Trigger: `.Changed` on an instance, from Luau.** `OnChanged<T>` is per *type*:
one callback for every entity that wrote that component. Roblox's `.Changed` is
per instance, and a binding that filtered a whole-world signal down to one
instance would walk every change for every connected script.

The likely shape is a subscription index — entity to connection list — consulted
during the flush that already exists, rather than a second dispatch mechanism.
The engine-level signal is the right primitive underneath either way, which is
why this is deferred rather than designed now.

- Property get/set **by name at runtime**, through the descriptor table.
- Attributes: a per-entity dynamic key/value map, which is a component holding a
  small map rather than anything the archetype knows about.
- The bindings manifest, generated from `TypeDescriptor::Properties` — which
  means property descriptors have to carry enough to generate from, not just
  enough to read with.
- `FLECS_CPP_NO_AUTO_REGISTRATION`'s equivalent: making `Components::Of<T>` on
  an unregistered type an error in release, once every component is declared.

---

## Unversioned — wanted, no trigger yet

- **Relationships.** `ChildOf`, `OwnedBy` as first-class pairs rather than an
  `Entity` field. Wanted the moment a query needs "every part of this model".
- **Query caching across worlds.** Plans are per-store; the *shape* is not. A
  thousand worlds running the same systems each build the same plan.
- **A component-level allocator hook**, for a type that wants pooling of its own.
- **Bitset queries.** `ContainsAll` is a binary search per term. A 64-bit mask
  per archetype would make matching one AND, at the cost of capping the
  component count — which is a cap this engine may well accept.
- **Structural change batching.** The deferral queue applies one command at a
  time; a thousand entities gaining the same component could move as one block.

---

## Rejected, with the reason

Kept so the same idea is not re-proposed without new information.

### An `Assigned` / `Allocated` flag per component slot

The proposal: preallocate a span of N components, mark each slot assigned or
free, and destroy by clearing the flag rather than moving anything. Entities
hold a slot index; the slot holds a weak back-pointer to its entity.

**Everything it aims at is already true, by a different mechanism:**

| Aim | How it is met today |
|---|---|
| reuse rather than reallocate | `Column` never shrinks; a world allocates once at its high-water mark |
| entity points at an index | `SparseSet`: entity → `{archetype, row}` |
| the component points back | `Archetype::Ids[row]` is exactly that back-pointer |
| destroy is cheap | swap-back is O(1) and touches two rows |
| a script handle survives destruction | generations, checked on every access |

**What it would change is the one thing worth protecting.** A flag means holes,
and holes mean iteration walks slots that hold nothing. Dense packing is why
`EachBatch` hands out an array a compiler can vectorise and why `EachParallel`
measured 3.5x — a span 512 wide holding 10 live rows costs 512 checks to visit
10 entities.

The genuine benefit of the flag is a **stable row index**, and nothing in the
engine currently wants one: every long-lived reference is an `Entity`, resolved
through the directory, which is stable already.

**What would reopen it:** a consumer that must hold a raw row index across a
removal — a GPU buffer indexed by row, or a script that caches a slot rather
than a handle. If that appears, the answer is likely a *pinned* archetype for
those components rather than holes everywhere.

The half of the proposal that *is* right — preallocated fixed spans, reused
rather than grown — is the chunked storage item above, and it wants doing.
