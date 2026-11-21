# World streaming plan

## Status

World streaming is future work. This document defines how authored content,
runtime entities, replication, asset delivery, portals, and travel fit together.
It does not introduce a second world model beside `world::Universe`, a second
asset cache beside `delivery`, or a second replication protocol.

The first useful release should stream presentation content on clients while the
server remains authoritative over a complete simulation. Server-side simulation
streaming follows only after activation rules, persistence, and replay behavior
are proven. That order keeps missing scenery from becoming missing gameplay.

## Product goal

A large world is divided into bounded, named cells. A host or client keeps only
the cells required by its current work, asks the existing delivery layer for
their content, builds them away from live ECS state, and publishes completed
changes at a deterministic world barrier.

The completed system must support:

- authored and generated cell bundles;
- distance, camera, gameplay, audio, and portal interest sources;
- bounded asset, decoded-content, simulation, replication, and GPU residency;
- seamless movement within a world and explicit handoff between worlds;
- deterministic activation of simulation-bearing content;
- reliable recovery from cancellation, loss, malformed content, and failed
  travel;
- Studio authoring, inspection, validation, and profiling;
- stable save and wire identities that do not depend on process-local ids.

## Non-negotiable rules

### Existing owners remain owners

- `world::Universe` continues to own worlds, world states, world clocks, and the
  deterministic bus barrier.
- `ecs::Store` remains the only live storage for entities and components.
- `delivery::AssetClient`, `assets::Manifest`, `assets::ChunkStore`, and
  `delivery::ContentCache` continue to fetch, verify, coalesce, and retain
  content.
- `replication::Authority` and `replication::Connector` continue to own the
  network representation of ECS state.
- the renderer continues to own GPU residency and cascaded presentation caches.
- the current game and universe formats continue to describe projects and
  worlds. Streaming extends them with named references.
- `CrossWorldService` remains the script boundary for world-to-world messages
  and travel coordination.

There must not be a parallel streaming scene graph, replication cache, portal
renderer, or package format.

### Boundaries carry copies

A world boundary never carries a pointer, reference, ECS entity handle, or
process-local `WorldId`. Requests and completions carry owned bytes and stable
names. A world is identified across a save, process, or network by its
`core::Name` text.

The same rule applies to worker completion. A worker returns a checked cell
description or owned artifact. It never mutates a live store and never retains
a view into one.

### Simulation changes only at fixed barriers

Network fetches and presentation prefetch may span frames. They cannot decide a
simulation result. Content that affects collision, navigation, scripts, or
gameplay must be present before the authority schedules its activation tick.

Parallel decode and preparation may run within a bounded job batch. Publication
is sorted and applied by the world owner at the next declared mutation barrier.
Worker completion order therefore cannot change entity order, event order, or a
recorded run.

### Names survive, numbers do not

Cells, authored entities, bundles, portals, destinations, and handoff records
use stable string names in files and messages. Dense indices are allowed after
loading and only inside one process. They are never saved or replicated.

### Hostile content is ordinary input

A server loads a project supplied by its operator. A client loads content from a
server. Both are untrusted boundaries. Parsing and validation finish before any
live entity, script, physics body, render resource, or file is created.

## Current foundation

The repository already contains most of the mechanisms this plan should join:

| Existing system | What streaming should reuse |
|---|---|
| `world::Universe` | named worlds, local and remote worlds, world states, fixed clocks, copied mailboxes, deterministic bus routing |
| `world::World` | one ECS store, one scheduler, one root, fixed-tick ownership |
| `game::SaveGame` and `game::SaveUniverse` | world files, universe manifests, relative path validation, content and DataStore declarations |
| `assets::Manifest` | content-addressed assets and bundles with signed catalogue roots |
| `delivery::AssetClient` | named requests, shared bundle fetches, cancellation, polling, source fallback, and counters |
| `delivery::ContentCache` | bounded on-disk content retention |
| `replication::Authority` | component declarations, changed-row survey, per-client interest predicate, priority, chunking, audits, and bounded per-tick work |
| `scene::VisibleToClients` | service-scope and player-private visibility policy |
| `ReplicatedFirst` | content that must outrank distance-based streaming during join |
| surface cameras and portals | portal view fitting, recursive presentation, seam transforms, body crossing, and cross-world play transitions |
| `parallel::Jobs` | fork-joined preparation with a measured serial crossover |
| profiling and metrics | CPU spans, reported worker spans, bytes, operations, heap residency, and release benchmarks |

The current replication path can stream a whole world over several ticks and
already applies an interest predicate. The spatial system narrows that existing
survey and adds explicit enter and leave lifecycle. It does not serialize ECS a
second way.

## Ownership and module cuts

The exact module split must obey the layer graph at the time it is built. The
responsibility split is fixed even if a type moves to satisfy that graph:

- `world` owns cell identity, activation state, the request schedule, mutation
  barriers, and cross-world handoff coordination;
- `scene` owns authored cell declarations, entity membership, portal references,
  and script-visible service components;
- `game` reads and writes cell declarations and bundle references in current
  project formats;
- `assets` owns canonical cell artifact formats and dependency descriptions;
- `delivery` fetches referenced artifacts and retains verified bytes;
- `replication` filters the existing entity stream by an interest snapshot and
  manages client enter and leave state;
- `physics`, navigation, audio, and render consume published ECS state through
  their existing preparation and residency paths;
- `client` supplies camera, viewport, listener, and local-player interest hints;
- `server` supplies player, bot, script, and host-policy interest hints;
- Studio edits shared declarations and displays diagnostics. It does not define
  runtime semantics.

No lower module may include a product type to learn whether it is running in
Studio, a client, or a server. Products configure the shared system through
plain settings and copied requests.

## Spatial model

### Cell identity

Each cell has a stable `CellName`, a world-space bound, and a content reference.
The initial implementation uses an axis-aligned grid because it gives constant
time lookup, predictable neighboring cells, and straightforward authoring.
Irregular authored regions may be added as an index over the same cell records,
not as a separate streaming mechanism.

A canonical generated name should contain the world name and signed integer
coordinates, for example `Rings/cell/-12/0/7`. An author may assign a readable
alias, but the canonical persistent name must remain unique within the world.

Cell dimensions are world settings. Changing them is a rebake that produces new
membership and new bundle signatures. Runtime code must not silently reinterpret
old coordinates with a new cell size.

### Cell declaration

The proposed authored record contains:

| Field | Meaning |
|---|---|
| `Name` | stable persistent cell name |
| `Bounds` | finite world-space AABB used for lookup and validation |
| `Bundle` | signed manifest name for static authored content |
| `Dependencies` | bounded stable names required before publication |
| `Neighbors` | optional validated acceleration data, derived when absent |
| `Policy` | always loaded, presentation only, or simulation eligible |
| `PriorityBias` | small authored adjustment within its priority class |
| `Version` | cell schema version used by migration and diagnostics |

Bounds, coordinates, dependency counts, name lengths, and bundle sizes have
hard format limits. Bounds containing NaN, infinity, inverted axes, or values
outside `scene::WorldBounds` are refused.

### Membership

An authored entity tree has one home cell. The root of the authored subtree
carries the stable membership, and its descendants follow it. This preserves
hierarchy and prevents half a model from unloading.

The home cell is chosen from the authored streaming pivot, not recomputed from
the current transform every frame. Moving a static model across a boundary in
Studio updates its declaration as an edit. A runtime dynamic entity remains a
live ECS entity and carries dynamic occupancy separately.

Large authored objects use one home cell plus bounded visibility coverage. They
are not cloned into every covered cell. Coverage says which cells may need the
object for rendering, collision, or navigation. The entity and its component
rows still exist once.

Hierarchy crossing between independently unloadable cells is rejected unless
the child is promoted into the parent's cell or both are placed in an
always-loaded group. An unload must never leave a live child naming a removed
parent.

### Authored bundles

A cell bundle is an immutable asset artifact. It contains a validated entity
description, stable local entity names, hierarchy, serializable component rows,
and content dependencies. It does not contain process-local entity ids or
component ids.

The bundle signature covers:

- canonical cell name and schema version;
- canonical entity and hierarchy order;
- component names, versions, and serialized values;
- referenced asset roots;
- declared neighbor, portal, and coverage metadata;
- the engine compatibility version used by the bake.

Editor-only selection, viewport, fold state, and preview data do not affect the
signature.

Repeated component and string data should use the existing package compression
and dictionary path. A new bespoke compression stream would duplicate delivery
and widen the hostile parser surface.

### Always-loaded content

World services, `ReplicatedFirst`, global scripts, shared configuration, active
players, and explicit host roots remain outside ordinary cell eviction. An
always-loaded group is small, named, budgeted, and visible in diagnostics. It
must not become a convenient place to hide an unpartitioned world.

## Runtime state model

The world store owns the authoritative runtime facts. Proposed resources and
components are:

- `StreamingWorld`: immutable cell index, configured limits, and activation
  epoch;
- `StreamingCell`: one ECS-backed row per declared cell containing stable name,
  state, revision, demand count, and measured residency;
- `CellMember`: home cell and stable authored entity name;
- `CellCoverage`: bounded cells or bounds that also need the entity;
- `DynamicOccupancy`: current cell set for a moving runtime entity;
- `StreamingInterestSource`: stable source name, kind, position or volume,
  radii, priority class, and expiry tick;
- `StreamingFailure`: bounded diagnostic code, retry count, and next eligible
  attempt;
- `CellLease`: a scoped request represented in ECS or a world resource rather
  than an owning pointer.

Only one row owns each fact. The scheduler may build temporary sorted arrays and
indexes for one barrier, but it must not retain a second mutable copy of cell
state. Derived indexes are invalidated by the specific cell row revision that
feeds them.

### Cell states

The state machine is explicit:

1. `Absent`: no request and no decoded payload.
2. `Requested`: delivery has a named request or a local bundle read pending.
3. `Preparing`: verified bytes are being parsed and artifacts are being built.
4. `Staged`: a complete checked description waits for a publication barrier.
5. `Active`: ECS state is published for the cell's declared role.
6. `Cooling`: demand has ended, but the grace window prevents churn.
7. `Evicting`: consumers are releasing resources at a barrier.
8. `Failed`: the latest request failed and carries bounded retry policy.

Presentation, replication, and authoritative simulation residency are separate
flags under this state machine. A cell may be simulated on the host, represented
on a client, and rendered only after its GPU dependencies settle. These are not
three cell objects.

Every transition carries a monotonically increasing revision. A completion for
an older revision is discarded. This makes cancellation and rapid movement safe
without asking a worker to stop at exactly the right instruction.

## Interest sets

### Interest sources

Interest is the union of bounded, named sources:

- authoritative player characters and their expected movement corridor;
- server bots and gameplay systems that require active simulation;
- client camera frusta and a near safety radius;
- audio listeners and audible source ranges;
- script leases with explicit expiry and permission;
- portal destination views and their limited recursive closure;
- pending travel destinations;
- always-loaded policy;
- Studio preview cameras and selected cells while editing.

Each source states what it needs: simulation, replication, collision,
navigation, audio, or presentation. A camera must not accidentally wake server
AI, and an off-screen gameplay source must not lose collision because it is not
visible.

Sources use stable names and deterministic ordering. A product updates source
rows before the world streaming barrier. The streaming system snapshots them
once per fixed tick and never calls back into product code while walking cells.

### Distances and hysteresis

Every source has an enter range and a larger leave range. The gap prevents a
cell boundary from loading and unloading every tick. Lookahead uses bounded
velocity and a configured horizon. Client-provided velocity is never trusted by
the server.

Distance is measured to cell bounds, not cell centers. Large cells therefore do
not disappear while the viewer still stands inside them.

The initial spatial query uses integer grid spans and visits only intersecting
cells. It must not scan every declared cell per player per tick. Irregular cells
later require a shared spatial index with explicit rebuild invalidation.

### Priority order

Priority classes are fixed and ordered before numeric distance:

1. always-loaded and join-critical content;
2. current authoritative collision and gameplay cells;
3. an accepted travel destination;
4. imminent movement and near safety cells;
5. visible portal destinations;
6. current camera-visible presentation;
7. audible cells;
8. speculative lookahead and Studio preview.

Within a class, order by deadline tick, distance to bounds, authored bias, then
stable cell name. Completion order never breaks a tie.

`ReplicatedFirst` keeps its existing meaning and outranks this spatial order.
Player-private service visibility remains an independent security predicate.

### Portal closure

A visible portal adds the cells intersecting its destination view. Recursion is
bounded by depth, projected screen area, total destination cells, and byte
budget. A visited pair of portal name and destination cell breaks cycles.

Portal interest is directional. Seeing through A into B does not automatically
load every cell that can see back through B. The next recursion step must pass
the same visibility and budget tests.

A body close enough to cross a portal requests destination collision and the
arrival safety region at gameplay priority. Rendering readiness alone is never
used as proof that traversal is safe.

## Request and publication pipeline

### Request discovery

At the streaming barrier, the world:

1. snapshots valid interest sources;
2. resolves cells and portal closure into a sorted demand table;
3. compares demand with current cell rows;
4. issues, upgrades, downgrades, or cancels named delivery requests;
5. records counters and the reason for each decision.

Repeated demand for one bundle becomes one `AssetClient` request with multiple
cell subscribers. Cancelling one cell does not cancel a shared request that
another cell still needs.

### Fetch and verify

The existing delivery client resolves names through the signed manifest,
coalesces requests by carrier bundle, walks configured sources, verifies
content roots, and places verified bytes in the existing bounded content cache.

Streaming adds priority hints and subscribers to that path. It does not bypass
manifest trust for local files or create a streaming-only HTTP client.

Unknown content names, untrusted catalogues, root mismatches, and exhausted
sources produce typed failure codes. Logs name the public content name and
source label, never a secret grant or private local path.

### Parse and prepare

Verified bytes are parsed into a bounded plain description. Validation includes
all counts, offsets, parent indices, component names, serialized lengths,
finite transforms, dependency depth, and aggregate memory estimates.

Only after validation may preparation build:

- ECS creation commands with stable local ordering;
- collision and navigation artifacts through their existing consumers;
- render upload descriptions through the current residency path;
- script load descriptions for the existing script host;
- replication metadata for the current authority.

Preparation may use `parallel::Jobs` only above a measured release crossover.
The owner joins any simulation-relevant batch before scheduling activation.
Presentation-only decode may finish later, but its result is published as visual
state and cannot affect the fixed-tick result.

### Staging

A staged cell is complete or absent. Partial hierarchy, partial collision, and
half-decoded component columns never enter the live store.

Staging records the request revision, bundle root, schema version, measured byte
costs, and dependencies. If any dependency changes before publication, the
staged result is stale and is discarded or prepared again.

### Deterministic publication

The owner publishes at one documented point before systems consume the new
tick. It sorts staged cells by stable name and applies each cell as one ECS
deferral scope. Creation order inside a cell follows canonical authored entity
name and hierarchy order.

Activation events are queued in the same order and delivered after all selected
cells are coherent. A script cannot observe a cell whose parent rows exist but
whose collision or required dependencies do not.

If publication of one staged description fails, the deferral is discarded and
that cell enters `Failed`. Other independent cells may publish. A required
dependency failure prevents its dependants from publishing.

### Deactivation

When demand ends, a cell enters `Cooling`. It may be reactivated without work
while its lease and decoded artifacts remain valid. Once the grace period and
budgets allow eviction, the owner:

1. emits a deterministic deactivation notice;
2. prevents new script work owned by the cell;
3. detaches or migrates allowed dynamic entities;
4. removes the authored subtree in hierarchy-safe order;
5. releases physics, navigation, audio, and render residency through current
   owners;
6. records persistent mutations before discarding their live rows;
7. marks the cell absent only after every required release succeeds.

An entity referenced by an active lease, handoff, persistent transaction, or
cross-cell hierarchy cannot be evicted. The diagnostic names the pinning owner.

## Dynamic entities and simulation

### First release behavior

The first release streams client presentation and replication visibility. The
server keeps authoritative simulation entities live. Static cell artifacts may
still be prepared on demand for clients, but server gameplay does not depend on
an I/O completion time.

This provides the largest memory and bandwidth win with the smallest
determinism risk.

### Simulation streaming

Server simulation streaming is an explicit later phase. A cell policy chooses
one of:

- `AlwaysSimulate` for global or time-sensitive gameplay;
- `SuspendWhenEmpty` for state that may pause exactly while no source needs it;
- `PersistAndUnload` for state serialized at a known tick and restored later;
- `OfflineAdvance` only for a system that provides a deterministic closed-form
  catch-up operation.

There is no implicit wall-clock catch-up. A sleeping farm, cooldown, or NPC does
not advance because real time passed unless its owning gameplay system defines
and tests that rule.

Content needed by simulation is preloaded before the authority announces an
activation tick. If it cannot be ready, activation is postponed as an explicit
host decision and replicated as state. Two replays given the same activation
record therefore apply the same cell on the same tick.

### Dynamic occupancy

Moving entities update occupancy from their world-space bounds at a fixed-tick
barrier. The broadphase or another existing spatial producer should provide
candidate bounds where practical. Streaming must not maintain a second transform
walk merely to rediscover what physics already knows.

An entity straddling cells remains one entity. Occupancy contains a bounded set
of overlapping cells. Fast motion expands the query by swept bounds so an
entity cannot skip the destination safety cell.

Persistent runtime entities have stable gameplay identities independent from
authored bundle-local names. Their rows are saved through the chosen DataStore
backend before a `PersistAndUnload` cell is released.

## Replication integration

### One interest predicate

The host combines these existing facts in one predicate:

1. the entity belongs to a client-visible service;
2. player-private ownership permits this client;
3. `ReplicatedFirst` or always-loaded policy requires it, or its cell is in the
   client's replication interest set;
4. any component-specific suppression still permits it.

Spatial interest narrows the current `replication::Authority` survey. It does
not replace service scope, privacy, component declarations, change detection,
audits, or ownership validation.

The interest set is frozen once per publish tick. Parallel per-client lanes read
that immutable snapshot and never mutate cell state.

### Enter, delta, and leave

A client-cell pair has explicit lifecycle:

- enter sends reliable structure and the current replicated rows for that cell;
- steady state uses the existing changed-row delta path;
- leave sends a reliable interest eviction for entities owned by that cell;
- authoritative destruction remains a different message and cannot be confused
  with temporary interest loss.

The current authoritative snapshot behavior treats unmentioned entities as
stale. Interest-scoped replication must therefore track which remote cell owns
each replica and evict only that ownership set. It must not feed a partial world
snapshot into whole-world authoritative apply.

Cross-cell references are sent only when both ends are visible, or are encoded
as a stable unresolved reference that becomes live when the target enters.
Process-local entity ids never stand in for that reference.

### Priority and bandwidth

Join-critical rows remain first. Structure enters before component deltas that
refer to it. Current gameplay and near collision outrank scenery, portal detail,
audio-only cells, and lookahead.

Every connection retains the existing bytes-per-tick and message limits.
Streaming adds per-class accounting, not another unlimited queue. A low-priority
cell that cannot fit remains pending. It is not truncated into an incoherent
replica.

When bandwidth collapses, the server shrinks speculative ranges before current
gameplay range. Range changes use hysteresis and a bounded adjustment rate so
one bad tick does not make the world breathe in and out.

### Client readiness

Replication readiness and visual readiness are distinct:

- replicated ECS state may exist while an asset is still downloading;
- a placeholder may render, but collision and authority remain server facts;
- a cell becomes visually ready only when required render dependencies publish;
- the client reports readiness for travel presentation and diagnostics, not as
  permission to change server simulation.

## Asset and entity lifecycle

### Dependency graph

Each cell bundle names immutable asset roots for meshes, textures, materials,
audio, animation, scripts, terrain chunks, and nested prefab packages. The
dependency graph is bounded, acyclic after validation, and deduplicated by
content root.

Dependencies are classified:

- required for simulation;
- required for first visible frame;
- optional quality data;
- speculative next-LOD data.

The classification controls readiness and priority. Missing optional quality
data selects an explicit fallback. Missing simulation data refuses activation.

### Shared residency

One content root used by ten cells is fetched, decoded, and resident once where
the existing owner supports sharing. Cells hold leases against that residency.
Eviction releases a resource only after its final lease ends.

The delivery disk cache remains the byte cache. Renderer, audio, physics, and
navigation caches remain owned by their outputs. Streaming records leases and
pressure but does not mirror their payloads.

### Hot updates

Studio and live development may publish a new bundle root for a stable cell
name. The old active cell remains coherent while the new revision prepares.
Swap occurs at one barrier. If validation or publication fails, the old revision
stays active and the error remains visible.

Runtime production content is immutable for a session unless the host signs and
announces a catalogue revision. A client never mixes dependencies from two
catalogue roots in one staged cell.

## Portals and cross-cell visibility

### Same-world portals

The current seam transform remains the geometric truth. Streaming adds only the
residency decisions around it:

- destination presentation cells are requested from the mapped camera frustum;
- destination collision cells are requested from mapped traversal safety
  bounds;
- portal lights and effects request the same destination dependency set as the
  current portal render pass;
- visible recursion is bounded and shares the renderer's portal capacity;
- cell eviction cannot remove a destination while a crossing transaction or
  visible portal lease still names it.

Streaming does not introduce a second portal camera or material path.

### Cross-world windows

A portal into another world is a window until a travel handoff commits. The
source world receives a copied presentation snapshot or remote-world replica
through existing world and replication boundaries. It never reads the target
store directly.

The portal view request names:

- target world name;
- target portal or anchor name;
- source view description;
- recursion depth and request revision;
- bounded quality and byte budget.

The response contains owned presentation data or a named remote replica update.
It carries no `WorldId`, entity pointer, renderer pointer, or store reference.

Lighting, physics queries, and gameplay do not cross a world window implicitly.
They need explicit copied protocols. A pretty destination image is not proof
that its simulation is locally accessible.

## Seamless travel and cross-world handoff

### Travel ownership

`CrossWorldService` coordinates travel by stable world and destination names.
The Players service owns player-specific possession, roster, camera, and client
binding. The character system remains unaware of players. Generic entities may
use the same world handoff protocol when their owning gameplay system permits
it.

### Prepare and commit

Travel is an idempotent prepare and commit exchange:

1. the source allocates a stable transfer name and copies a whitelisted entity
   description, source tick, destination name, and session claim;
2. the target validates version, permissions, capacity, destination, and
   content readiness, then reserves a target identity;
3. the target returns an acceptance carrying the transfer name and reservation
   revision;
4. the source freezes transferable input at a barrier and sends commit bytes;
5. the target creates the entity at its next barrier and acknowledges the
   committed target identity;
6. only after acknowledgement does the source remove or release the old
   authority;
7. the Players service swaps possession and camera binding to the accepted
   target character.

Duplicate prepare, commit, or acknowledgement messages return the recorded
outcome. They never create a second entity. A transfer tombstone remains for a
bounded retry window.

### Transfer description

The whitelist contains only components whose owners declare them transferable.
Pointers, process-local ids, open script continuations, physics contacts, GPU
state, and transient caches never cross. References inside transferable rows are
stable names and are resolved against the target world.

Gameplay systems decide whether inventory, status effects, companions, and
persistent identities move. Streaming provides the checked envelope and
transaction, not one universal character snapshot with accidental authority
over every game.

### Failure behavior

Until commit acknowledgement, the source remains authoritative. On timeout or
target refusal it unfreezes the entity and reports a typed reason. After target
commit, a lost acknowledgement is resolved by querying the transfer name rather
than recreating or restoring both copies.

A client may prewarm the destination view and assets, but cannot claim travel,
choose a target spawn, or provide transferable state. The server validates the
active possession and portal crossing.

Session reconnect finds the committed transfer record from the host or
persistent session owner. It does not guess from the last world the client drew.

## Scripts

### Service surface

Expose one `WorldStreamingService` backed by world-owned state. Proposed script
operations are:

- query a cell name and current public state;
- map a finite position or region to stable cell names;
- request a bounded lease with purpose, priority class, and expiry;
- release a lease;
- observe activation, deactivation, failure, and budget-pressure events;
- inspect public per-cell diagnostics where permissions allow;
- ask `CrossWorldService` for travel using a stable destination name.

The simple case is one call that requests a region for a bounded duration. More
advanced controls remain server-only. A client request is a hint subject to
rate, range, ownership, and permission checks.

### Event timing

Script callbacks run on the world thread after a completed publication barrier.
Events are sorted by cell name, transition kind, and revision. They never run on
delivery or worker threads.

An activation callback sees the complete published cell. A deactivation
callback cannot retain an entity pointer or yield while assuming the cell stays
live. Scripts retain stable names and request a new lease when needed.

### Script lifecycle

Cell-owned scripts start only after all required rows exist. They receive one
activation for a revision and one deactivation before removal. Failed starts
roll the cell back or mark that script failed according to explicit bundle
policy. They do not leave half the cell running.

Global scripts live in the always-loaded group and may observe cells. They may
not keep stale ECS handles across deactivation.

## Studio authoring

Studio should provide:

- a world streaming panel showing the cell grid, stable names, bounds, policy,
  bundle root, revision, state, demand reasons, and memory costs;
- viewport overlays for home cells, coverage, interest radii, portal closure,
  current residency, and eviction pins;
- assign, split, merge, rebake, validate, and move-to-always-loaded commands;
- a cell browser that focuses existing Explorer entities rather than maintaining
  a second hierarchy;
- portal destination validation and recursion-cost preview;
- a budget simulator driven by recorded camera or character paths;
- live counters for requested, preparing, active, cooling, failed, and pinned
  cells;
- hot-swap diagnostics that preserve the last good cell when a rebake fails;
- import and export progress through the existing progress framework.

Editing a cell pins its current revision. Unsaved edits are never evicted. A
save first flushes script buffers and pending edits, then bakes cell artifacts,
then atomically updates manifest references. Failure leaves the prior project
document and content roots intact.

Undo and team editing operate on authored declarations and instance edits. They
do not record cache residency or worker completion order.

## Budgets, cancellation, and backpressure

### Declared budgets

Budgets are separate because pressure in one resource must not masquerade as
another:

- requested bundles and network bytes in flight;
- verified disk-cache bytes;
- compressed and decoded CPU bytes;
- staged cell bytes awaiting publication;
- live ECS entities and component bytes;
- physics and navigation artifact bytes;
- audio stream count and decoded bytes;
- GPU buffer and texture bytes;
- cells activated, deactivated, or structurally replicated per tick;
- portal recursion cells and pixels;
- cross-world transfers in flight.

Defaults come from platform and product settings. Every limit has a hard safety
ceiling. A game may lower limits freely and may raise them only within the
engine ceiling.

### Backpressure

Each stage stops accepting lower-priority work before its queue becomes
unbounded. Pressure propagates upstream as capacity, not as a blocking wait:

- renderer pressure stops optional visual preparation;
- staged-byte pressure stops new decode jobs;
- decode pressure stops speculative delivery requests;
- network pressure shrinks lookahead and delays low-priority enters;
- ECS activation limits spread structural publication across ticks without
  splitting one cell transaction.

Current gameplay safety work may displace speculative work. It may not exceed
hard memory bounds. If a required safety cell cannot fit, the authority applies
an explicit game policy such as blocking traversal, slowing movement, or
refusing travel. It does not continue into absent collision.

### Cancellation

Each request has a cell revision and cancellation source. Fetch, decode, and
artifact builders check cancellation at bounded work intervals. Cancellation is
cooperative and need not interrupt a system call.

Correctness comes from revision checks at publication. Even a worker that
finishes after cancellation cannot publish stale data.

Cancellation releases queue slots, temporary bytes, and leases exactly once.
Shared content stays live while any subscriber remains.

## Cache and eviction policy

### One policy, several owners

Streaming computes demand and eviction priority. Each output owner releases its
own resource. It does not hand payload ownership to a central mega-cache.

Candidate order is:

1. failed or superseded staged work;
2. speculative cells with no current demand;
3. cooled presentation-only cells;
4. distant audio and optional quality data;
5. decoded artifacts whose source bytes remain cached;
6. inactive simulation cells that have completed persistence;
7. never an active safety, travel, edit, or transaction pin.

Within a class use last demanded tick, rebuild cost, bytes freed, then stable
cell name. The decision is reproducible from the same recorded state.

### Grace and churn

Separate grace windows apply to cell ECS state, decoded artifacts, GPU
resources, and disk content. A camera turn should not immediately destroy work
needed on the next turn.

Diagnostics report loads, reactivations, evictions, bytes rebuilt, and demand
oscillation by reason. A high reload rate is a policy failure even when memory
stays within budget.

### Cascaded presentation caches

Cell activation invalidates only the presentation layers whose source set
changed. A transform change inside one active cell must not rescan bundle
references for the entire world. A portal destination update invalidates its
portal history and composition, not unrelated game or Studio interface layers.

A cache hit performs no upload, command-buffer rebuild, or allocation for that
layer. Baselines commit only after successful writes.

## Persistence and save format

### Authored state

The universe manifest continues to list worlds and content sources. A world file
gains a versioned streaming section containing:

- cell dimensions and coordinate convention;
- stable cell declarations and policies;
- bundle names and content roots;
- always-loaded group references;
- portal and destination references;
- schema compatibility requirements.

Large cell payloads remain content artifacts. They are not embedded repeatedly
inside the universe XML. Relative paths continue through the existing checked
resolution rules.

### Runtime state

Runtime mutations are keyed by universe, world, cell, and stable gameplay
identity in the configured DataStore. The record contains a schema version,
saved simulation tick, source bundle revision, and game-owned component data.

Loading a cell applies runtime mutations to the authored base in a scratch
description before publication. Deleted authored entities use explicit
tombstones. Silence never means deletion.

Writes are transactional per cell revision. A host crash during eviction leaves
either the previous durable record or the new complete one. It cannot leave
half the entities persisted and half discarded.

### Versioning and migration

Every cell artifact begins with magic, format version, declared byte length,
and content root. Component rows keep stable component names and their own
serializer versions.

Loaders either migrate a known older version into the checked current
description or refuse it with a precise error. They never infer a version from
payload shape.

A migration tool rewrites project references only after every selected cell has
converted and verified. It retains the old bundles until the project save
commits. Streaming format migration is therefore recoverable.

## Failure recovery

Failures are classified as permanent for the current catalogue revision or
transient for one source attempt.

Permanent failures include malformed cell bytes, unknown required component,
invalid hierarchy, root mismatch, unsupported version, impossible bounds,
dependency cycle, and missing required simulation data.

Transient failures include source timeout, connection loss, temporary origin
failure, cancelled work, and budget pressure. Retry uses bounded exponential
backoff with deterministic jitter derived from the stable cell name. Gameplay
safety work may retry sooner within a fixed cap.

Source fallback remains delivery's job. Streaming observes the final request
state and does not retry each origin itself.

On failure:

- an active old revision remains active during a hot update;
- an absent presentation cell uses an explicit placeholder or nothing;
- an absent collision cell blocks traversal according to host policy;
- a travel destination refuses prepare before source authority is released;
- failed persistence pins the live simulation cell and raises operator-visible
  pressure;
- a device-loss rebuild requests current active visual cells from existing
  decoded or content residency, not from a shadow scene copy.

## Security limits

All external counts and lengths are bounded before allocation or multiplication.
The format declares and validates limits for:

- cells per world and dependencies per cell;
- entities, hierarchy depth, components, and bytes per cell;
- strings, stable names, and referenced roots;
- portals, recursion depth, and cross-cell coverage;
- compressed and decompressed sizes and expansion ratio;
- meshes, textures, scripts, audio, and nested packages;
- outstanding requests, leases, retries, and transfers per peer;
- cells and bytes a client may request outside server-selected interest.

Parsing uses checked arithmetic and scratch descriptions. Paths cannot escape
the opened project or content store. Content roots and catalogue signatures are
verified before parsing executable or GPU-facing payloads.

Client hints are clamped to server-owned positions, permissions, and budgets. A
client cannot request another player's private cell state, force an authority to
load arbitrary server content, select a transfer payload, or acknowledge its
own travel.

Portal graphs and dependencies are treated as hostile graphs. Cycle detection,
node limits, and total visited limits apply even when each individual record is
valid.

Every new binary parser receives fuzz coverage. Corpus cases include truncated
headers, huge counts, offset overlap, invalid UTF-8 where required, NaNs,
dependency cycles, hierarchy cycles, duplicate names, unknown components,
decompression bombs, and valid maximum-size inputs.

## Observability

### Per-frame and per-tick counters

Report:

- demand sources and cells by reason;
- state transitions and failures;
- request, cancellation, retry, and cache-hit counts;
- transferred, verified, decompressed, decoded, staged, published, replicated,
  uploaded, and evicted bytes;
- active, cooling, pinned, and failed residency by owner;
- replication enter, delta, leave, and deferred counts;
- portal closure depth, visited cells, and clipped requests;
- travel prepares, commits, retries, refusals, and recovery queries;
- queue depths, oldest age, and time spent under each pressure limit.

Use metrics counters for drained per-frame totals and gauges for current
residency. Do not read metrics to steer behavior.

### Profiles

Profile request discovery, spatial query, delivery pump, validation, decode,
artifact preparation, publication, deactivation, replication interest, enter
assembly, eviction, and travel transitions.

Workers measure their own scopes and the owner reports completed durations after
join. Network wait is idle time. GPU upload and device work use their existing
categories and counters rather than being folded into CPU streaming time.

All performance claims name release preset, platform, world shape, cell size,
entity count, client count, movement path, portal count, cache state, bandwidth,
and configured budgets.

## Migration from whole-world residency

Migration keeps the project runnable after each phase.

### Phase 0: baseline and invariants

- record current whole-world load, save, join, replication, render residency,
  and portal costs in release;
- add counters that distinguish structural join, steady deltas, content
  delivery, and GPU residency;
- add architecture checks that prevent product dependencies in shared streaming
  code;
- state hard format and runtime ceilings before accepting new fields.

Gate: existing projects produce byte-equivalent simulation and current
replication tests remain green.

### Phase 1: declarations and bake

- add stable cell declarations and world settings;
- add Studio grid assignment and validation;
- bake immutable cell bundles through the current asset publisher;
- keep every cell always loaded at runtime;
- round-trip declarations through game, universe, export, import, and team-edit
  paths.

Gate: partitioning and rebaking a project changes no runtime behavior.

### Phase 2: delivery and preparation

- request cell bundles through `AssetClient`;
- validate into scratch descriptions;
- add shared dependency leases and bounded decoded artifacts;
- add cancellation, revision checks, counters, and failure diagnostics;
- publish all cells before play begins.

Gate: the staged path loads the same ECS result as the existing direct world
load, including hierarchy and stable references.

### Phase 3: client presentation streaming

- add camera, audio, portal, and lookahead interest;
- add visual activation, grace, and owner-driven eviction;
- retain full server simulation and full authoritative ECS state;
- add placeholders and device-loss rebuild behavior.

Gate: visual memory follows configured bounds, gameplay results remain
byte-identical, and a recorded camera path has no missing near-safety content.

### Phase 4: interest-scoped replication

- freeze per-client spatial interest per publish tick;
- combine it with current service, privacy, suppression, and `ReplicatedFirst`
  rules;
- add reliable enter and leave lifecycle without partial authoritative apply;
- add unresolved stable cross-cell references;
- adapt range under measured connection pressure.

Gate: clients never receive private or out-of-interest rows, re-entry restores
the exact current state, and steady bandwidth scales with local interest rather
than total world size.

### Phase 5: portals

- drive destination cell demand from portal views and traversal safety;
- share current portal render and seam transforms;
- add bounded recursive closure and cycle diagnostics;
- add cross-world presentation windows using copied remote state.

Gate: portals neither bypass budgets nor permit traversal into absent collision.
Current non-Euclidean portal tests remain valid.

### Phase 6: simulation streaming

- add explicit cell simulation policies;
- preload simulation dependencies before recorded activation ticks;
- persist and unload eligible runtime entities transactionally;
- add deterministic restore and optional game-owned offline advance;
- integrate physics, navigation, scripts, and bots at one activation barrier.

Gate: replay reproduces activation ticks and gameplay state. Failure to persist
never destroys live authority.

### Phase 7: seamless travel

- add prepare, commit, acknowledgement, tombstones, and recovery query;
- integrate destination prewarm, Players possession swap, input, and camera;
- support generic transferable entities through explicit gameplay whitelists;
- test process-to-process handoff under loss and duplicate messages.

Gate: no test can produce two authorities or lose the only authority for one
transfer name.

### Phase 8: remove the replaced path

- make an unpartitioned world one always-loaded cell through the same runtime;
- delete direct load or replication branches that the cell path replaces;
- keep compatibility readers only where version policy requires them;
- publish final budgets, profiles, and migration guidance.

Gate: there is one load path, one replication path, and one portal path.

## Test plan

### Data and format tests

- canonical cell naming handles negative coordinates and rejects collisions;
- position-to-cell mapping is exact on positive and negative boundaries;
- AABB spans include every intersected cell and no unrelated cell;
- authored bundles round-trip with stable canonical bytes;
- hierarchy and component name resolution survive reordered registration;
- old known versions migrate and unknown versions fail whole;
- malformed input never mutates a live store;
- aggregate limits reject individually valid records whose total is unsafe.

### State machine tests

- every allowed transition succeeds and every illegal transition is refused;
- stale completion revisions cannot publish;
- cancellation releases resources once;
- shared bundle demand survives one subscriber leaving;
- cooling prevents boundary churn and still yields under hard pressure;
- hot update failure preserves the prior active revision;
- dependency failure prevents dependant publication.

### Determinism tests

- randomized worker completion order produces identical activation and event
  order;
- cell input order and hash-map insertion order do not affect output;
- recorded activation ticks replay byte-identically;
- the same interest snapshot yields the same priority list;
- simulation never branches on client asset or visual readiness;
- parallel and serial preparation publish identical canonical descriptions.

### Replication tests

- `ReplicatedFirst` arrives before spatial content;
- service scope and player-private visibility still hold inside active cells;
- enter sends structure before rows;
- leave removes only replicas owned by that interest set;
- temporary leave is not authoritative destruction;
- re-entry receives mutations made while absent;
- cross-cell references resolve when targets enter and clear safely when they
  leave;
- loss, reordering, duplication, and bandwidth pressure converge;
- one hundred clients with distinct interest sets stay within per-tick bounds.

### Portal and travel tests

- visible portal closure loads only bounded destination cells;
- portal cycles terminate by visited identity and budget;
- traversal cannot commit before destination collision is active;
- cross-world windows carry copies and no process-local ids;
- duplicate prepare and commit are idempotent;
- lost acknowledgement recovers the committed target;
- refusal and timeout leave source authority live;
- reconnect finds exactly one committed authority;
- player possession and camera swap only after target acknowledgement;
- bots and generic entities can use the handoff without a Player dependency.

### Persistence tests

- cell runtime state overlays the matching authored revision;
- explicit tombstones survive unload and reload;
- interrupted persistence leaves the prior durable record valid;
- failed persistence pins the live cell;
- save, export, import, package, and universe discovery preserve cell references;
- migration failure leaves old bundle roots and project files intact.

### Security and fuzz tests

- fuzz every new cell, dependency, lifecycle, and handoff decoder;
- reject huge counts before allocation;
- reject overflowed offsets and decompressed sizes;
- reject invalid floats, bounds, hierarchy, names, and dependency graphs;
- reject paths escaping the project or content root;
- reject unsigned or root-mismatched content;
- rate-limit malicious client hints and travel retries;
- verify error logs expose no grant, token, or private path.

### Studio tests

- cell assignment, split, merge, undo, redo, and save are lossless;
- unsaved edited cells stay pinned;
- failed rebake preserves the last good revision;
- overlays reflect runtime rows rather than separate editor state;
- team edits address worlds and entities by stable names;
- budget simulation reproduces a recorded path deterministically.

### Release benchmarks

Measure at minimum:

- one small world where streaming should add almost no steady cost;
- a sparse world with one million authored entities and a small active radius;
- fast linear travel through new cells;
- repeated boundary turns that stress hysteresis;
- many clients sharing one region;
- many clients spread across distinct regions;
- deep but bounded portal visibility;
- cold cache, warm disk cache, warm decoded cache, and warm GPU residency;
- constrained bandwidth with loss;
- eviction under CPU, GPU, and entity pressure;
- cross-world travel between processes.

Record frame and tick time, busy and idle time, network bytes, disk reads, decode
bytes, allocations, live and peak memory, uploads, command buffers, replication
work, cache hits, reload churn, and missed readiness deadlines.

## Release gates

World streaming is ready only when:

- small projects use the same cell path without a behavior fork;
- no world, worker, network, or save boundary carries a pointer or process-local
  number as identity;
- simulation activation is fixed-tick and replayable;
- malformed or partial content never mutates a live store;
- all queues, graphs, counts, retries, caches, and residency have hard bounds;
- client hints cannot widen authority, privacy, or server content access;
- portal and travel failure never create duplicate or missing authority;
- existing delivery, replication, portal, save, and cache owners remain the only
  owners of their data;
- release profiles show memory and bandwidth scale with active interest;
- steady still scenes stop requesting, decoding, uploading, and rebuilding;
- migration leaves one production load and replication path.

## Explicit non-goals

The first implementation does not include:

- an infinite procedural universe with no authored or bounded cell catalogue;
- peer-to-peer authority migration;
- transparent shared-memory access between worlds or processes;
- client-authoritative cell activation, collision, travel, or persistence;
- automatic simulation of unloaded time for arbitrary scripts;
- a second renderer for portals or streamed cells;
- a second asset cache, replication protocol, ECS, or package format;
- virtualized geometry or terrain algorithms, which have their own plans;
- arbitrary hierarchy links across independently unloadable cells;
- transferring live script stacks, physics contacts, GPU state, or native
  pointers between worlds;
- hiding required-content failure behind silent fallback geometry;
- unbounded portal recursion, dependency graphs, retries, prefetch, or cache.

## Decisions to keep visible

- Grid cells are the first spatial index. Add irregular regions only after a
  measured authored case needs them.
- Client presentation streaming ships before server simulation streaming.
- Static bundle content and dynamic persistent state remain separate inputs to
  one staged ECS publication.
- Interest loss and authoritative destruction are distinct replication events.
- Cross-world portals are presentation windows until an explicit handoff
  commits.
- A source remains authoritative until the target acknowledges commit.
- Async I/O may change presentation readiness. It may not decide a simulation
  result.
- Budget pressure degrades optional range and quality before gameplay safety.
- One stable name identifies each persistent cell, entity, portal, and transfer
  across every boundary.
