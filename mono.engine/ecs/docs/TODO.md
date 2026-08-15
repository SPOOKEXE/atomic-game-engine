# ecs - what is not built yet

What this module still owes, and to whom. Ordered by the version that needs it,
because "later" without a consumer is how a list like this stops being read.

An item here is a **commitment with a trigger**, not a wish. If nothing will be
blocked by its absence, it does not belong here - it belongs in
`docs/DEFERRED.md` or nowhere.

---

## Built

For orientation, since everything below is defined against it.

| Piece | What it is |
|---|---|
| `Entity` | index + generation, no pointer |
| `ComponentId` / `TypeDescriptor` | runtime type info: size, align, lifetime hooks, serialisation |
| `Components` | the process-wide type table, sealed after startup |
| `Column` | one type-erased component array as a directory of doubling chunks, swap-back removal, capacity released as the rows fall |
| `ChunkPool` (private) | the process-wide freelist chunks are taken from and given back to, capped so it cannot become the leak it removed |
| `ComponentSet` | interned sorted id set - archetype identity, and a class |
| `SparseSet` | paged entity directory: generation, liveness, location, in two index regions |
| `Archetype` (private) | one table: id array + one column per component |
| `Store` | the world: entities, components, resources, clock, queries, deferral |
| `ChangeChannel` | per-row dirty bits as a `DirtyBits` column |
| Change signals | `OnChanged<T>` / `FlushSignals`, fired at a phase boundary |
| Snapshot | `Store::Save` / `Load`, components by name, directory reproduced exactly |

---

## v0.2 - the rest of this version

### ~~Chunked column storage with a shared span pool~~ - built at v0.4

The measurement this was waiting on got taken, and it moved the item twice: it
narrowed the trigger, and it found that most of what the trigger was blamed for
came from somewhere else entirely.

| Shape | Resident | Live rows | Ratio |
|---|---|---|---|
| 1000 worlds, always 100 entities | 72.7 MB | 2.7 MB | 26.6x |
| 1000 worlds, peaked at 10k, settled at 100 | 705.8 MB | 2.7 MB | 258x |

**The first row was not columns.** 64 MB of that 72.7 was `SparseSet` pages:
a page is allocated whole on a world's first entity, and it was 4096 slots of
sixteen bytes, so a world of a hundred entities paid a 64 KB entry fee. Fixed at
v0.2, by making the first page 512 slots and leaving every page after it at 4096
- **72.7 MB to 16.7 MB.** Shrinking all of them was tried and rejected: eight
times the allocations scatter a large world's columns and a multi-world tick over
100k entities each measured 8-21% slower.

**The second row is what chunked storage was for**, and it is now closed. A
`Column` is a directory of chunks taken from a process-wide `ChunkPool`, and a
chunk goes back the moment the rows stop reaching into it.

**Chunks double rather than being a fixed size, and that is the decision worth
keeping.** A fixed size has to choose between giving a settled world its peak
back and not scattering a big world's rows, and measured, it cannot have both:

| Rows per chunk | `Each · 100k` | `Each over two archetypes · 100k` |
|---|---|---|
| 1024, fixed | +8.0% | +16.6% |
| 2048, fixed | +2.6% | +12.3% |
| 4096, fixed | +2.1% | +6.9% |
| 16384, fixed | +0.9% | +2.9% |
| **8, doubling** | **+0.4%** | **+1.4%** |

Doubling refuses the choice: chunk zero holds eight rows - exactly the capacity
the column used to jump to on its first growth, so **nothing got worse for a
world that never grows** - and chunk `k` holds `8 << (k-1)`, so capacity is never
more than twice the rows and almost every row of a big column is in its largest
two chunks. Eight rows also matters for a reason a row count hides: a *resource*
is a column of one row and some of them are 720 bytes wide, so a fixed thousand-
row chunk would have charged a world a megabyte and a half to hold one.

Measured on the shape the item names, a thousand worlds of three components
peaking at 10k and settling at 100:

| | Storage's own accounting | Process resident |
|---|---|---|
| Held at peak, which is what the old layout kept forever | 820.3 MB | 620.7 MB |
| After settling, chunked | **13.7 MB** | **85.2 MB** |

The remaining 85 MB is not the storage: `Store::ResidentStorageBytes` accounts for
13.7 MB and `ChunkPool` for at most 8 MB, and `malloc_trim` moves the figure by
0.1 MB - the rest is glibc holding freed blocks in its arenas. Getting that back
wants an arena-aware allocator and is not this item.

What it cost, against the same tree with the change reverted, on a quiet machine
with fifteen samples and the minimum reported:

| | Before | After | |
|---|---|---|---|
| `Each · 10k` | 3.91 us | 3.97 us | +1.5% |
| `Each · 100k` | 39.01 us | 39.17 us | +0.4% |
| `Each · 500k` | 204.18 us | 194.89 us | -4.6% |
| `EachBatch · 100k` | 39.44 us | 39.58 us | +0.4% |
| `EachBatch · 500k` | 203.11 us | 195.57 us | -3.7% |
| `EachBatchParallel · 500k` | 73.08 us | 65.61 us | -10.2% |
| `toggle · add and remove, 10k` | 703.49 us | 759.23 us | **+7.9%** |
| `control · Each over 10k rows` | 4.00 us | 4.54 us | **+13.5%** |

Two costs are real and reproducible. **A structural change is 6-9% slower**,
because `At` is a directory lookup rather than a multiply and a removal has to
notice when it emptied a chunk. And `Structure.cpp`'s control - an `Each` over
10k rows in a world the toggle benchmarks have just churned - is **13.5% slower**,
which the same `Each` over an unchurned 10k world is not: after heavy structural
churn a column's chunks come back from the pool in release order rather than in
address order, so iteration walks memory out of order. A pool that handed back
the lowest-addressed free chunk of a class would fix it and would cost a sorted
insert on every release; that trade has not been measured and is the obvious
thing to try if this ever matters.

**Two static-teardown bugs came out of this and neither was new.** A `Column`
destroyed during static teardown reaches the process-wide chunk pool and the
process-wide component registry, and both were function-local statics built
*later* than whatever static owned the store - so reverse destruction order tore
them down first and the column's destructor read freed memory. It surfaced as a
benchmark binary that printed its whole report and then segfaulted, which then
made every suite measured after it look slow. Both singletons now outlive the
process. The failure depends on which static was touched first, which is why it
had not shown up before.

**The multi-world *parallel* barrier benchmarks could not be attributed and are
not claimed either way.** `Tick · 50 quiet worlds, no entities` measured +3.5%,
+61%, +3.3% and +61% across runs of the same binary, and a world with no entities
holds no columns to chunk; bypassing the pool entirely changed nothing. Every
*serial* variant - which is what `ROADMAP.md` added them for, because thread
wake-up dominates fifty empty worlds - is inside its own spread.

**The directory's own leftover went in the same pass**, and the roadmap
over-specified it. `SparseSet::Free` still releases nothing on its own - dropping
a page there needs the page's indices purged from the LIFO free list, which is an
O(FreeList) pass inside the operation whose whole value is being O(1). What
happens instead is that a page emptying releases every *trailing* page holding
nothing live, the high-water mark comes back down with them, and a free-list entry
naming a released index is discarded once when it surfaces. `Clear` releases
everything.

The hazard that comes with it is generations: a recreated page starting again at
`FIRST_GENERATION` hands a reissued index back at exactly the generation the
oldest handles were issued with, and every one of them comes alive. Each page
index keeps an **epoch** - one past the highest generation it ever issued - for as
long as the directory exists, and a recreated page starts every slot there. Four
bytes per 64 KB page, and it makes the revival impossible by construction.

`Archetype::Ids` stayed one contiguous array rather than being chunked, because
`VisitChangedRuns` hands a callback `entities + start` beside a value pointer and
one addition is worth more than the bytes a second directory would save. Its
capacity is rebuilt when it is four times the rows and comes back to twice them,
so a population has to double before it can shrink again.

Explicitly **not** an occupancy flag per row. See "Rejected" below.

### The class table and the instance model

- A class is a name, a parent class, and a `ComponentSet`.
- `:IsA` as an ancestor test, made O(1) by interval-numbering the class tree.
- **Prototype rows**: each class owns one hidden row of defaults, so
  `Instance.new` is a column copy and `:Clone()` is the same copy from a
  different source row.
- `Hierarchy { Parent, FirstChild, NextSibling, PrevSibling }` plus
  `InstanceName`. Organisational only - **not** a transform hierarchy, because
  `Transform` is world-space and nothing propagates.
- Property descriptors: `{class, name} -> {component, offset, value type}`.

### ~~Archetype edge cache~~ - built

The trigger was a measurement, and the measurement is
`benchmarks/Structure.cpp`. It reported a transition at **50.8 ns** against
**9.2 ns** for overwriting a component already present, with **28 ns** of that
being the intern alone - a vector allocation, a sort, an FNV hash and a
process-wide mutex, to recompute an answer that cannot change.

`ArchetypeEdges` (in `src/`, private like `Archetype` itself) caches add-one and
remove-one per table. **11.99 ms to 6.25 ms on 100k toggles.** A scanned list
rather than a hash map: an archetype has a handful of edges, and a hash of a
composite key would have cost roughly what it replaced.

The invariant to keep: an edge is valid only for the watch epoch it was recorded
under. `Observe` gives a table a `DirtyBits` column and therefore moves where a
transition lands, so an edge from before it sends the row to a table that does
not track changes - a write that goes unreported rather than a crash. `Watched`
is only added to through `WatchComponent`, which bumps the epoch beside it, so
there is one place to get that right rather than three.

---

## v0.3 - replication

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

### ~~An index range for locally predicted entities~~ - built at v0.4

`SparseSet` splits the index space at 2³¹. An authority allocates from
`[0, 2³¹)`; `Store::CreatePredicted` allocates from `[2³¹, 2³²−1)`, with
`0xFFFF'FFFF` reserved as the value a refused allocation returns. Neither side
wraps into the other - an exhausted range refuses and says which one it was,
because wrapping would put the collision back at the one moment nobody is
watching.

The directory is **two page regions**, each paged exactly as the single one was,
so the small-first-page rule holds for both and a store that never predicts
anything allocates nothing for the second. `SaveSnapshot` writes the directory as
one run per region and `SNAPSHOT_VERSION` is **2**; the two layouts are
indistinguishable from the bytes, so an older snapshot is refused rather than
sniffed.

`SetAdoptOnly` stays and its meaning narrows to what it always meant: a replica
may not mint an *authoritative* entity. It now covers `CreateInstance` and
`CloneInstance` as well as `Create` - it did not, and `scene::MakePart` carried a
copy of the check to work around that, which is now deleted.

`tests/Replication.cpp` still pins the collision by minting authoritatively in
both stores, and the case beside it takes the same scenario through
`CreatePredicted` and keeps both entities. `engine.ecs.prediction` covers the
range, promotion and the adopt-only split.

**`Store::Promote` is the primitive and the policy is deliberately absent.** It
rewrites a predicted entity's identity to an authoritative one without moving the
row, and fixes up the directory, the row's own handle, the name maps and the
instance tree. It does **not** rewrite an `ecs::Entity` held inside an arbitrary
component - nothing in `TypeDescriptor` says which bytes are handles - and such a
handle reads as dead afterwards rather than as a different entity. *When* to
promote belongs to whatever predicts, and the trigger for that is still unmet:
nothing predicts a spawn until there is a projectile.

### Interest filtering

A snapshot of *part* of a world - the entities one client can perceive. Needs a
predicate at the table and row level, and a way to say "this entity is not
missing, you simply cannot see it" so a client does not treat it as destroyed.

---

## v0.4 - components and physics

- `QuickHash`, the fallback for change detection over batch-written rows. Label
  it as second-best where it lands, or it will spread.
- Sleeping as an archetype move rather than a per-row branch: adding a
  `Sleeping` tag takes the row out of the dynamic query entirely.
- Column-level `min`/`max` summaries for broad-phase culling, if a measurement
  asks for them.

---

## v0.5 - script bindings

### Per-instance change signals

**Trigger: `.Changed` on an instance, from Luau.** `OnChanged<T>` is per *type*:
one callback for every entity that wrote that component. Roblox's `.Changed` is
per instance, and a binding that filtered a whole-world signal down to one
instance would walk every change for every connected script.

The likely shape is a subscription index - entity to connection list - consulted
during the flush that already exists, rather than a second dispatch mechanism.
The engine-level signal is the right primitive underneath either way, which is
why this is deferred rather than designed now.

- ~~Property get/set **by name at runtime**, through the descriptor table.~~
  **Built at v0.5** as `Store::GetProperty`/`SetProperty`. Bytes and a size,
  size-checked; a conversion reaches its component through `GetMutable`, so the
  change mark comes for free rather than being something the setter has to
  remember.
- ~~The bindings manifest, generated from `TypeDescriptor::Properties` - which
  means property descriptors have to carry enough to generate from, not just
  enough to read with.~~ **Built at v0.5, and that last clause was the whole
  problem.** A descriptor carried a component and an offset, which is enough to
  read `Visible` with and cannot describe `Size` at all. It is a getter, a
  setter, a kind and the component sets each side touches now - so the manifest
  needs **no offsets**, and rule 4 is satisfied by construction rather than by a
  disclaimer about which fields survive a recompile.
- Attributes: a per-entity dynamic key/value map, which is a component holding a
  small map rather than anything the archetype knows about. **Still open, and
  the manifest deliberately does not describe it** - it is dynamic, so there is
  nothing static to describe, and the absence should not read as an oversight.
- `FLECS_CPP_NO_AUTO_REGISTRATION`'s equivalent: making `Components::Of<T>` on
  an unregistered type an error in release, once every component is declared.
  **Still open, and v0.5 found the sharpest case for it yet**: constructing a
  `world::Postbox` on a store that never registered its mailbox types minted
  them under the compiler's spelling, and nothing failed until the next
  `Universe` registered them properly and aborted - in whichever test order
  reached it first.

---

## Unversioned - wanted, no trigger yet

- **Relationships.** `ChildOf`, `OwnedBy` as first-class pairs rather than an
  `Entity` field. Wanted the moment a query needs "every part of this model".
- **Query caching across worlds.** Plans are per-store; the *shape* is not. A
  thousand worlds running the same systems each build the same plan.
- **A component-level allocator hook**, for a type that wants pooling of its own.
- **Bitset queries.** `ContainsAll` is a binary search per term. A 64-bit mask
  per archetype would make matching one AND, at the cost of capping the
  component count - which is a cap this engine may well accept.
- **Structural change batching.** The deferral queue applies one command at a
  time; a thousand entities gaining the same component could move as one block.

---

## Rejected, with the reason

Kept so the same idea is not re-proposed without new information.

### Padding a twelve-byte component to sixteen, for vectorisation

The proposal, and `ROADMAP.md`'s v0.4 item: `Each` is at the single core's
streaming limit and "a compiler cannot vectorise a twelve-byte stride cleanly
whatever the iteration does", so widen the row - a declared fourth member rather
than `alignas(16)`, because implicit padding is never initialised and would make
two runs of one scene serialise differently.

**Measured, and the premise is backwards.** The same loop over two arrays, three
adds per row, minimum of twenty-five samples:

| | 100k rows | 500k rows |
|---|---|---|
| 12 bytes, `-O2` | 38.74 us | 196.38 us |
| 16 bytes, `-O2` | 39.59 us | 189.77 us |
| 12 bytes, `-O3` | **16.61 us** | **85.63 us** |
| 16 bytes, `-O3` | 41.93 us | 201.54 us |

At the flags this project actually builds with there is no difference at all. At
`-O3`, where GCC vectorises, **the packed twelve-byte layout is 2.4x faster than
the padded one** - which is the opposite of the claim. A twelve-byte AoS of
floats has no gaps, so the vectoriser treats it as one flat float stream and
fills every lane; padding puts a lane nobody writes in every vector and throws a
quarter of the width away.

Through the ECS, at `-O2`, padding both benchmark components to sixteen bytes
moved `Each` and `EachBatch` at 10k, 100k and 500k by less than 3% in either
direction - inside the run-to-run spread - and made `Each over two archetypes ·
100k` **14-19% slower**, which is the most bandwidth-bound case in the suite and
the one 33% more bytes should hurt. The acceptance criterion was *10k and 100k
improve by more than 500k regresses*; nothing improved.

So the padding is reverted and `scene`'s components keep their widths.

**What would reopen it:** not a compiler flag argument - a *measurement* showing a
specific loop the vectoriser refuses for a stride reason. The number worth
chasing instead is in the table above and has nothing to do with layout: the
control loop is **2.4x faster at `-O3` than at `-O2`**, and the `bench` and
`release` presets build first-party code at `-O2`. If `Each` at 500k is the
number that matters, that is where the factor is.

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
measured 3.5x - a span 512 wide holding 10 live rows costs 512 checks to visit
10 entities.

The genuine benefit of the flag is a **stable row index**, and nothing in the
engine currently wants one: every long-lived reference is an `Entity`, resolved
through the directory, which is stable already.

**What would reopen it:** a consumer that must hold a raw row index across a
removal - a GPU buffer indexed by row, or a script that caches a slot rather
than a handle. If that appears, the answer is likely a *pinned* archetype for
those components rather than holes everywhere.

The half of the proposal that *is* right - preallocated fixed spans, reused
rather than grown - is the chunked storage item above, and it wants doing.
