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

### Chunked column storage with a shared span pool

**Trigger: hundreds of small worlds in one host.** Today a column owns one
growing allocation and never gives it back, so a thousand worlds each hold
their own high-water mark forever. Chunks of a fixed row count, drawn from and
returned to a process-wide pool, bound that: a world that shrinks releases spans
another world reuses.

Compatible with the iteration paths as they stand — `EachBatch` already
promises nothing about where a batch ends, so one batch per chunk is a legal
division. `Column::At` becomes a divide and a modulo, which is why this wants a
measurement before and after rather than an assumption.

Explicitly **not** an occupancy flag per row. See "Rejected" below.

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

### Archetype edge cache

**Trigger: a measurement.** `Set` on a component an entity lacks currently
interns `set.With(id)` — a sort, a hash and a map lookup. Caching add-one and
remove-one edges per archetype turns it into one lookup. Not urgent: nothing has
shown it in a profile, and the number to have first is what archetype
transitions cost as a fraction of a tick.

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

The likely shape: reserve a high index range that the authority never allocates
from, and give a replica's `Create` that range. A predicted entity then has an
identity the server can never mint, and promoting one to a server entity is an
explicit step rather than a coincidence.

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
