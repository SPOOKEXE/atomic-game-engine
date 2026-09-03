# Navigation and artificial intelligence system plan

## Status

This document describes future work. It does not claim that the current engine
has a navigation mesh, crowd solver, perception system, or behaviour runtime.

The first useful delivery is deliberately narrow: bake walkable surfaces from
the existing collider model, answer bounded path queries, follow a corridor,
and let a server bot drive a character through neutral `CharacterIntent`. Each
later feature extends that path instead of creating another movement stack.

## Product goal

Build one deterministic, inspectable system that lets authored agents and
scripts find routes, perceive relevant world facts, choose goals, and drive any
character without pretending to be a player.

The system must support:

- tile-based navigation over floors, ramps, stairs, caves, and stacked rooms;
- explicit traversal links for jumps, drops, doors, ladders, portals, and
  game-defined actions;
- separate walk, climb, swim, and fly navigation domains;
- bounded synchronous path results within a fixed simulation tick;
- stable local avoidance for crowds without changing the global path;
- sight, hearing, proximity, and script-defined perception facts;
- high-level goals and a small domain-specific behaviour plan;
- server bot drivers that write the same neutral intent as players and replays;
- offline baking, Studio preview baking, and explicit runtime publication;
- streamed navigation data and paths that may cross cells, portals, and worlds;
- Luau and JavaScript surfaces generated from the same registered classes;
- enough diagnostics to explain why a bake, path, sensor, or behaviour failed.

Navigation does not own character locomotion. It asks for a route and produces
steering. The character system remains the only system that chooses grounded,
falling, climbing, swimming, or other character states and the only system that
solves the character capsule.

## Existing foundation

The engine already has useful pieces. This work must extend them rather than
build parallel copies.

| Existing piece | Current role | Required use |
|---|---|---|
| `spatial::HashGrid` | Stable broad spatial candidate lookup | Index navigation tiles, agents, dynamic obstacles, and perception candidates with purpose-specific grids |
| `spatial::RaycastAll`, `OverlapBox`, and `ShapeCast` | Bounded broad spatial queries | Reuse their overflow contract and stable candidate handling for navigation-side indexes |
| `physics::Raycast` and `ShapeCast` | Exact queries against prepared static and dynamic collider indexes | Resolve generic ECS world-query batches for sight and traversal checks |
| `scene::Collider`, transforms, and collision layers | Authoritative world geometry | Supply bake sources, exclusions, costs, and dynamic obstacle bounds |
| `collision` shapes and triangle data | Geometry below scene and physics | Rasterise and validate baked walkable space without depending on the physics solver |
| `parallel::Jobs::For` | Fork-joined parallel work | Parallelise tile baking and independent path queries while joining before publication |
| `ecs::Store::EachParallel` | Parallel component traversal with structural writes forbidden | Evaluate independent agents only after all inputs are committed |
| `core::Name` | Stable names with dense process-local comparisons | Identify agent profiles, areas, traversal modes, goals, facts, actions, and graph node types |
| world fixed ticks and mutation barriers | Deterministic simulation schedule | Commit requests, results, route changes, steering, and intent at named barriers |
| scene services and registered properties | Inspectable object model | Hold `NavigationService`, path objects, modifiers, links, agents, and sensors |
| generated Luau and JavaScript bindings | One reflected script surface | Expose navigation through registered scene classes and methods |
| content-addressed assets and manifests | Stable delivery by content | Publish baked tile sets and compiled behaviour plans as immutable artifacts |
| `nodegraph` | Studio-only generic graph editing library | Edit behaviour documents in Studio without linking editor types into engine runtime |
| `scene::Portal` and seam transforms | Traversal between remote regions | Form explicit navigation links rather than teaching the mesh about portal rendering |
| character isolation plan | Neutral character intent and player-independent characters | Let player clients, bots, scripts, and replays share one character input path |

The existing physics broadphase has separate static and dynamic indexes. The
navigation system must not borrow their private storage or keep a pointer into
it. A generic world-query batch is the data-shaped cut between navigation and
physics. Navigation writes bounded requests to the ECS, physics resolves them
using its existing indexes, and navigation consumes the results later in the
same fixed tick.

## Non-negotiable design rules

1. Navigation and AI never own a `Player` reference. A bot is an ordinary
   character driver and does not need a fake player.
2. Navigation never writes transforms, velocities, humanoid state, or bones.
   Its final output is bounded neutral `CharacterIntent` through a bot driver.
3. Character state remains in the character system. A behaviour action named
   `MoveTo` cannot directly set `Running`, `Jumping`, or `Climbing`.
4. Persistent identities are stable strings. Dense polygon, tile, node, and
   query numbers are compiled handles valid only inside one published artifact
   or process.
5. Authoring work may be asynchronous. A fixed tick never waits for a result
   whose completion time depends on an operating-system scheduler.
6. Fixed-tick work may use jobs only when all jobs join before the phase ends.
   A route cannot appear one tick later on a slower machine.
7. Runtime reads one immutable navigation snapshot per tick. A replacement is
   published atomically at an owner-thread mutation barrier.
8. Every queue, query, graph, route, sensor, and neighbourhood is bounded.
   Overflow is a visible status, never silent truncation presented as success.
9. Stable iteration and explicit tie-breaks decide equal costs. Hash-table or
   worker completion order never decides gameplay.
10. The ECS owns saved and replicated facts. Derived indexes and compiled
    artifacts are caches with explicit source signatures, not second
    authorities.
11. Studio owns editing, previews, and overlays. Engine modules own formats,
    validation, baking, pathfinding, perception, and runtime semantics.
12. Navigation does not become a universal graph virtual machine. Custom game
    rules remain scripts or native systems that write typed goals and facts.

## Engine ownership and layer placement

The first implementation adds one shared `navigation` engine module at L8. It
may see `scene` at L7, `spatial` at L6, `collision` at L5, `ecs` at L3,
`parallel` at L2, and `core` at L1. It does not link `physics`, another L8
module, and therefore does not add a lateral dependency.

Responsibilities are split as follows:

| Owner | Responsibility |
|---|---|
| `core` | Stable names, vectors, frames, bounds, clocks, byte readers, and byte writers |
| `parallel` | Fork-joined tile and query work plus cancellable authoring jobs |
| `collision` | Source triangle and convex geometry used by the baker |
| `spatial` | General spatial indexes and bounded candidate queries |
| `scene` | Serializable navigation components, service classes, requests, results, links, modifiers, agents, sensors, and character intent |
| `physics` | Resolve generic world-query batches against the authoritative collider indexes |
| `navigation` | Bake artifacts, tile residency, pathfinding, route following, avoidance, perception reduction, goals, behaviour plans, and bot driver output |
| `assets` | Content addressing, manifests, bundles, and verified byte delivery |
| `script` | Methods and properties that create scene requests and inspect scene results |
| `render` | Debug drawing from immutable navigation debug snapshots |
| Studio | Bake commands, progress, validation messages, graph editing, and view overlays |
| client | Local visualisation and optional non-authoritative prediction |
| server | Authoritative navigation publication, AI evaluation, and intent permission |

`scene` defines the plain request and result rows used between `navigation` and
`physics`. Neither module includes the other. The scheduler order is the
contract:

1. navigation gathers generic world-query requests;
2. physics resolves the whole bounded batch;
3. navigation reduces results into perception and traversal facts;
4. AI chooses goals and steering;
5. bot drivers publish character intent;
6. the character system consumes intent.

The architecture graph must gain the new module and its exact links in the same
change that adds its CMake target. New scene components must also be added to
the component purpose catalogue. There is no `ALLOW_TIER_ESCAPE` path.

## Public object model

The public object model is intentionally small. Most compiled polygon and
search state is not represented as thousands of Instances.

### `NavigationService`

One `NavigationService` exists per world. It owns settings and request entry
points, not a second copy of navigation data.

Initial properties:

| Property | Type | Meaning |
|---|---|---|
| `Enabled` | boolean | Whether the world processes navigation requests and bot drivers |
| `PublishedRevision` | integer, read-only | Revision of the immutable navigation snapshot |
| `PublishedContent` | string, read-only | Content hash or canonical artifact name currently in use |
| `PendingBakeState` | named enum, read-only | `Idle`, `Collecting`, `Building`, `Validating`, `Ready`, `Failed`, or `Cancelled` |
| `PendingBakeProgress` | number, read-only | Completed authoring work divided by total declared work |
| `MaximumQueriesPerTick` | integer | Server-authoritative bounded request budget |
| `MaximumAgents` | integer | Bounded active navigation agent count |

Initial methods:

- `CreatePath(parameters)` creates an inspectable `NavigationPath` with a
  copied parameter set.
- `RequestBake(options)` starts authoring work and returns a `NavigationBake`.
- `PublishBake(bake)` atomically publishes a validated ready artifact.
- `CancelBake(bake)` requests cooperative cancellation.
- `GetAgentProfile(name)` returns an immutable profile view or nil.
- `GetArea(name)` returns an immutable area definition or nil.
- `GetDebugStatistics()` returns bounded copied counters, not internal spans.

Publishing is authority-only. A client may preview a local bake in Studio but
cannot replace live server navigation.

### `NavigationPath`

A path object is a request handle and result view. It does not retain pointers
to navigation tiles.

Properties include:

- `Status`: `NotComputed`, `Queued`, `Success`, `Partial`, `NoPath`,
  `OutsideNavigation`, `Unloaded`, `OverBudget`, `Stale`, or `Invalid`;
- `Origin` and `Destination`;
- `AgentProfile` by stable name;
- `AllowedModes` and area-cost overrides;
- `NavigationRevision` used by the result;
- `Waypoints`, copied and bounded;
- `TotalCost`, `ExpandedNodes`, and `VisitedTiles`;
- `FailureReason`, a bounded diagnostic code and short message.

`ComputeAsync(origin, destination)` yields the calling script while the request
joins the current world's bounded path batch. The name describes script
scheduling, not nondeterministic simulation completion. On the authority the
request is resolved and committed at a defined fixed-tick phase. A non-yielding
`TryCompute` returns `Queued` when called after that phase has closed.

Waypoints carry position, desired traversal mode, area name, and an optional
traversal link name. Scripts do not receive internal polygon ids.

### `NavigationAgent`

A `NavigationAgent` attaches navigation preferences and runtime route state to
an entity. It may be used by a character, vehicle, animal, or script-owned
object.

Authoritative properties include:

- profile name and allowed traversal modes;
- requested destination or explicit path;
- stopping distance and preferred speed;
- avoidance group, mask, priority, and radius override;
- repath policy and maximum repath frequency;
- current route status and route revision;
- current world-space steering direction and desired speed, read-only;
- current traversal link and progress, read-only.

The agent does not contain health, character state, animation state, player
ownership, or a camera subject.

### `NavigationModifier`

A modifier marks authored space as excluded, expensive, preferred, or assigned
to a named area. It is a saved Instance with a transform and bounded shape.

Modifiers affect baking when static. A runtime modifier uses a dynamic obstacle
or cost overlay and does not silently trigger an asynchronous mesh rebuild.

### `NavigationLink`

A link joins two named endpoints that normal polygon adjacency cannot express.
It stores:

- a stable name;
- source and destination frames;
- one-way or two-way direction;
- supported agent profiles and traversal modes;
- entry and exit radii;
- base cost and optional named area;
- enabled state and capacity;
- an optional action name such as `OpenDoor`, `JumpGap`, or `UsePortal`;
- an optional destination world name and portal name;
- a revision used to invalidate routes.

The link does not run arbitrary script code from the pathfinder. Route following
emits a bounded named action request. A script or native system accepts or
refuses it and reports completion through the link traversal state.

### `NavigationObstacle`

A dynamic obstacle projects a moving bounded shape into one or more navigation
domains. It may block a path, increase local cost, or participate only in local
avoidance.

Doors should usually be links whose enabled state changes. Crates and crowds
should usually be dynamic obstacles. Rebaking a static tile because a door
moved is the expensive answer to a small state change.

### Perception instances

`PerceptionSensor` is a base class for inspectable sensors. Initial concrete
classes are:

- `SightSensor` for view cone, range, target mask, and occlusion;
- `HearingSensor` for bounded sound events and falloff;
- `ProximitySensor` for nearby tagged entities;
- `DamageSensor` for authoritative damage facts already emitted by gameplay;
- `NavigationSensor` for route, obstacle, and traversal facts.

Sensors write a bounded `PerceptionMemory` on their owning AI agent. They do not
call behaviour callbacks while a spatial query is half complete.

### `BotController`

`BotController` is the adapter from AI output to neutral character intent. It
references a character model, a navigation agent, and an optional behaviour
plan. It holds no `Player` and is valid on a server with no client modules.

Its output is one resolved intent snapshot per fixed tick. Disabling or
destroying the controller publishes neutral intent in the same mutation, so a
character does not keep walking on stale input.

## Navigation representation

### Why a polygon mesh

The walking representation is a tile-based polygon navigation mesh, not a
height map and not a grid of world-sized voxels.

A polygon mesh handles ramps, stairs, caves, bridges, stacked floors, and large
open rooms with fewer search nodes than a uniform grid. Tile boundaries make
baking, invalidation, delivery, and streaming bounded. The source geometry may
come from parts, convex hulls, triangle soups, or future terrain, but the query
representation is one format.

### Published tile set

One published navigation artifact contains:

- format and compatibility versions;
- canonical bake settings;
- agent profile declarations;
- area names and default costs;
- a stable world-space tile coordinate convention;
- independently addressed tile payloads;
- a coarse cluster graph for long paths;
- traversal link declarations;
- source content signatures and build diagnostics.

Each surface tile contains compact vertices, convex polygons, adjacency,
boundary edges, clearance, area names, height range, and links. Vertices are
quantised relative to the tile origin using declared precision. The decoded
form validates every count and index before allocation or publication.

Internal polygon handles combine a published artifact revision, tile slot, and
polygon slot. They are never saved, replicated, or shown to scripts. Persistent
references use a tile coordinate plus a stable authored link or area name.

### Agent profiles

An `AgentProfile` is a stable named record containing:

- radius and standing height;
- maximum step height and walkable slope;
- minimum ledge clearance;
- preferred speed and acceleration hints;
- allowed traversal modes;
- per-area cost defaults;
- simplification and waypoint tolerances.

The profile used to bake and the profile used to query must be compatible. A
larger agent cannot query a tile baked only for a smaller clearance and pretend
the route is valid. The first implementation may bake one profile per tile set.
Multi-profile artifacts are added only after measurements show shared source
rasterisation is worth their format complexity.

### Surface, volume, and link domains

Walkable surfaces are not forced to represent every kind of travel.

| Domain | Representation | Uses |
|---|---|---|
| Surface | Polygon navigation tiles | Walking, wheeled travel, grounded animals |
| Climb | Authored or generated traversal links and climb strips | Ladders, vines, climbable walls |
| Water | Sparse three-dimensional cells or convex navigation volumes | Swimming above floors and through caves |
| Air | Sparse three-dimensional cells, authored lanes, or waypoint graphs | Flying agents |
| Transition | Named links between domains | Jump, dive, surface, take off, land, portal |

The first release builds the surface domain and authored links. Swim and fly
formats arrive later behind the same route segment interface. They do not turn
the surface mesh into a fake three-dimensional structure.

### Canonical ordering

Canonical output is required for content addressing and replay diagnostics.
The baker sorts:

- tiles by signed world coordinate;
- source geometry by stable entity path and source signature;
- regions by their lowest raster coordinate;
- contours by stable winding and lowest vertex;
- polygons by tile-local centroid and canonical vertex sequence;
- adjacency and links by stable endpoint key.

Equal floating-point comparisons use documented quantisation before sorting.
Worker completion order never reaches the artifact.

## Baking pipeline

### Source snapshot

A bake starts by collecting an immutable description on the world owner thread.
It includes only data that affects navigation:

- collider shapes and world transforms;
- collision layer and navigation inclusion flags;
- terrain collision artifacts when terrain exists;
- static modifiers and authored links;
- agent profile and area definitions;
- tile bounds, cell size, cell height, and region settings;
- stable source names and per-source content signatures.

The worker never holds an ECS pointer. Source collection copies a bounded
description, then releases the store. A source revision change makes the result
stale rather than letting a worker read half of two worlds.

### Surface tile stages

Each tile runs these named stages:

1. collect source geometry intersecting the tile plus its declared halo;
2. rasterise triangles and convex shapes into compact spans;
3. mark slope, head clearance, step, and ledge constraints per profile;
4. apply exclusions, named areas, and cost modifiers;
5. build connected walkable regions;
6. trace and simplify region contours within a declared error;
7. partition contours into bounded convex polygons;
8. build adjacency, boundary portals, clearance, and local clusters;
9. attach and validate traversal links;
10. canonicalise, encode, hash, and validate the tile payload.

Tile halos prevent an operation at one edge from seeing less geometry than the
same operation in a neighbouring tile. The published polygon data is clipped
back to the tile boundary, and matching edge keys verify seams.

### Incremental baking

Every source has a navigation signature derived from shape, transform,
relevant flags, modifier data, and profile settings. A changed source dirties
only tiles intersecting its old or new bounds plus the required halo.

The cache key includes the complete settings and source signature set for a
tile. A cache hit performs no rasterisation, contouring, or encoding. A removed
source invalidates its old tiles even though it no longer appears in the new
source list.

The artifact root changes only after every requested tile is valid. A partial
preview may show completed tiles, but it is never labelled publishable.

### Authoring jobs and progress

Offline and Studio baking may run across frames because it is authoring work,
not fixed-tick simulation. A `NavigationBake` exposes:

- current stage;
- total, queued, active, completed, cached, failed, and cancelled tile counts;
- current byte estimates;
- bounded diagnostics by tile and source;
- elapsed wall time and worker busy time;
- cooperative cancellation state.

Workers build owned tile results. The owner accepts a result only when its job
id, source revision, settings signature, and tile coordinate still match. A
cancelled or superseded bake discards late completions safely.

No worker mutates the ECS, asset manifest, published navigation snapshot, or
Studio widget directly.

### Runtime geometry changes

Moving objects are handled immediately by dynamic obstacle and cost overlays.
They do not trigger a tile bake.

A game that truly changes static topology has three explicit choices:

1. enable or disable an authored traversal link;
2. install or remove a bounded dynamic obstacle;
3. request a new artifact, wait outside the fixed tick, then publish it at an
   authority-controlled mutation barrier.

Publishing a runtime artifact records its content identity and publication
tick for replay and diagnostics. Until publication, every query continues to
read the old complete snapshot. There is no half-new mesh.

## Path queries

### Query inputs

A path request copies:

- origin and destination positions;
- agent profile;
- allowed travel modes;
- named area cost overrides;
- permitted and forbidden link tags;
- whether a partial result is acceptable;
- maximum search nodes, cost, distance, and output waypoints;
- current navigation publication revision;
- request tick and stable sequence.

The server chooses or clamps all budgets. A client cannot request an unbounded
search by supplying a large number.

### Start and destination projection

Origin and destination are projected to compatible navigation domains within a
bounded radius and height. Projection returns the chosen point, polygon or cell,
distance, and reason when none is found.

Equal candidates are chosen by distance, vertical error, area cost, then
canonical tile and polygon key. The nearest item in an unordered container is
not a valid tie-break.

### Hierarchical search

Long paths use two levels:

1. a coarse A* search over tile clusters and cross-tile links;
2. a detailed A* search through polygons or cells along the admitted cluster
   corridor.

Short paths skip the coarse level. Every open-set comparison is total: estimated
cost, paid cost, canonical node key, then insertion sequence. Costs are finite,
non-negative, and quantised before comparison where platform float differences
could alter an equal branch.

The search returns `OverBudget` with its best bounded partial corridor when it
reaches the expansion limit. It never reports that prefix as a complete path.

### Corridor and waypoint generation

A successful surface search first returns a polygon corridor. A funnel pass
extracts corners from its portals. The result preserves mandatory traversal
links and area changes even when geometric simplification could remove their
positions.

Waypoint simplification may remove nearly collinear points only within the
profile tolerance and only when a direct constrained segment stays inside the
corridor. Path smoothing never raycasts through a wall merely because the end
points are visible in broadphase bounds.

The route stores enough corridor state to advance and repair locally. It does
not store a pointer into a tile. A publication revision mismatch makes it stale
and schedules a bounded repath.

### Batched execution

Requests close at a named phase. They are sorted by request tick, agent stable
key, and sequence. Independent searches may run through `Jobs::For`, but the
batch joins before results are committed in request order.

Below a measured crossover, searches run inline. The release profile records
the serial and parallel crossover for representative small, medium, and long
routes. Thread count is not allowed to change result order or selected paths.

### Route repair and repathing

An agent requests repair when:

- its corridor becomes stale;
- a dynamic obstacle blocks its next bounded segment;
- it leaves the corridor beyond tolerance;
- its target moves beyond the repath threshold;
- a traversal link changes revision;
- a required tile becomes unavailable.

Repair first tries to reproject onto the remaining corridor, then performs a
local search, then requests a complete path. Repath frequency is capped per
agent and per world. A moving target cannot force one full A* search per frame
per pursuer.

## Traversal modes and links

### Walking

The route follower computes the next corridor target, preferred horizontal
velocity, stopping behaviour, and requested facing. It writes
`NavigationSteering`, not character motion.

The bot driver translates steering into neutral movement and facing fields.
The character controller decides whether the capsule can move, step, slide, or
fall. If actual progress differs from planned progress, the agent records a
blocked fact and may repair the route.

### Jumping and dropping

Jump and drop links are explicit. Generated jump links may be added later from
bounded ballistic checks, but an ordinary polygon edge never implies that an
agent will guess a jump.

A jump link declares entry speed bounds, launch direction hints, landing
volume, supported profiles, and cost. The bot driver requests the named jump
action. The character state system chooses and executes the actual jump. The
navigation agent observes landing or failure and advances or repairs the route.

### Climbing

Climb strips and ladder links expose entry, climb axis, extent, and exit frames.
The route names the climb traversal mode. The character controller activates
its climb mode only after its own sensors and state rules accept the request.

Navigation does not set a `Climbing` state or pose limbs. It only keeps the
route and desired progress along the link.

### Swimming and flying

Swimming and flying use true three-dimensional navigation cells or authored
lanes. Their neighbour sets are bounded, their clearance includes agent radius,
and their vertical costs are explicit.

The common route is a list of typed segments. Surface, water, and air searches
may therefore share scheduling and result objects without sharing a false
two-dimensional representation.

### Doors, lifts, and game actions

A traversal link may require a named action. On approach, the bot driver emits
an action request with link name, expected revision, and sequence. A gameplay
system may accept it, deny it, or leave it pending.

The route follower waits only for a bounded timeout. Failure adds a temporary
link penalty or blocks the link for that agent before repathing. It never calls
an arbitrary script from inside A* or holds a navigation lock while gameplay
runs.

### Portals and cross-world links

A portal link uses the same seam transform as visual and physical traversal.
The path contains a source world name, portal name, destination world name, and
destination entry frame. It never carries a `world::World *` or an entity id
from another world.

Within one world, the portal joins two navigation clusters. Across worlds, a
resident universe-level route contains copied world segments. Each world still
computes and owns its local path. The session or world-streaming layer arranges
destination residency and transfer; navigation does not move the entity across
the boundary.

## Dynamic obstacles and local avoidance

### Dynamic overlay

Dynamic obstacles are indexed separately from immutable navigation tiles. Each
tick builds or updates a compact, stable-order overlay from ECS components.
Obstacle source ids are sorted before raster or polygon overlap updates.

The overlay may:

- mark a bounded set of polygons blocked;
- add a temporary named cost;
- reserve a traversal link capacity;
- participate only in near-field avoidance.

The global tile artifact remains immutable. Removing an obstacle removes its
overlay contribution without rebuilding the tile.

### Local avoidance contract

Local avoidance changes preferred velocity for the next fixed tick. It cannot
change a route, teleport an agent, or bypass a traversal link.

For each agent, the solver gathers a bounded set of neighbours and obstacle
segments from spatial indexes. Candidates are ordered by distance, time to
collision, then stable entity key. If the set overflows, the nearest canonical
prefix is used and overflow is counted.

The first solver should use a bounded velocity-obstacle method with:

- a fixed time horizon;
- a fixed maximum neighbour count;
- a fixed number of constraint passes;
- quantised input velocities and output steering;
- explicit priority for yielding;
- a deterministic fallback to reduced speed and then stop.

Do not add random jitter to break symmetry. Symmetry breaks by stable agent key
and declared avoidance priority. A seed is required only for behaviour that is
genuinely stochastic, and then it is explicit and recorded.

### Crowds

Crowd evaluation is data-parallel after positions, radii, preferred velocities,
and neighbour indexes are frozen for the tick. Agents read the same snapshot
and write separate steering rows. Results commit in stable agent order.

Crowd groups may reserve link capacity and use queue points near narrow links.
They do not maintain private authoritative transforms. The character or object
simulation remains the only owner of actual position.

Crowd quality tiers may reduce neighbour count or update visual-only steering
for distant non-authoritative agents. Server gameplay agents always preserve
their fixed-tick intent and action events.

## Perception

### Perception pipeline

Perception has four explicit stages:

1. gather candidate targets and immutable sensor descriptions;
2. emit bounded generic world-query requests for checks such as occlusion;
3. let physics resolve the query batch against its prepared indexes;
4. reduce candidates and query results into canonical perception facts.

No stage calls behaviour code. The complete memory snapshot is committed before
goal evaluation begins.

### Generic world-query batch

The scene-level request format supports ray, overlap, and shape-cast requests.
Each request includes:

- request owner and stable sequence;
- query kind and copied geometry;
- collision layer mask;
- ignored entities in a small bounded list;
- maximum distance and result count;
- result precision needed;
- source tick.

Physics writes status, overflow, and a bounded canonical hit list. Results are
sorted by distance and stable entity key. The batch is useful beyond AI and is
not named after sight.

Requests and results are transient ECS resources drained in the same tick. They
are not saved or replicated.

### Sight

Sight first filters by range, view cone, target tags, and target bounds. It then
asks the world-query batch for occlusion only on surviving candidates.

A visible fact includes target stable entity reference, last seen position,
velocity if known, confidence, first seen tick, last seen tick, and source
sensor name. Multiple sight sensors merge through a documented rule rather than
duplicating one target in memory.

Partial visibility may sample a bounded set of target points. The point set is
part of the sensor definition and does not grow with mesh complexity.

### Hearing

Gameplay sound events are not read back from an audio mixer. An authoritative
system emits a bounded `PerceptionSoundEvent` with position, loudness, category,
source, tick, and optional occlusion policy. Audio may render the same event,
but it is not the owner of whether AI heard it.

Hearing applies range and falloff, then optional occlusion through the generic
query batch. Events expire by tick and are drained in stable sequence order.

### Proximity and damage

Proximity uses the navigation spatial index and stable tag filters. Damage
perception consumes authoritative gameplay damage events. Neither invents a
second collision or health system.

### Perception memory

`PerceptionMemory` is a bounded ECS component or resource owned by the AI
agent. Facts use stable names and a closed set of value types:

- boolean;
- finite number;
- `Vector3`;
- stable entity reference within the same world;
- stable name;
- tick and bounded age.

Entries are ordered by fact name, target key, and source sensor. Replacement and
expiry rules are explicit. A full memory rejects the lowest-priority or oldest
fact according to configured policy and increments an overflow counter.

Perception memory is evidence, not gameplay truth. A target may have moved
since it was last seen. Behaviour that needs current truth must ask the owning
gameplay system.

## Goals and behaviour authoring

### Separation from character state

AI behaviour answers high-level questions such as:

- which target matters;
- whether to patrol, flee, investigate, wait, or interact;
- which destination and action should be requested;
- when a failed action should be retried or abandoned.

Character state answers physical questions such as whether the capsule is
grounded, falling, jumping, climbing, swimming, or dead. Animation state
answers how that result should look. None of these state machines includes the
other.

### Typed blackboard

Each AI agent owns one bounded typed blackboard. Keys are stable names and
values use the same small set as perception facts plus bounded strings where
authoring needs them. Each key declares:

- type;
- default value;
- saved, replicated, or transient policy;
- writable source classes;
- optional numeric range;
- diagnostic display name.

Unknown fields from a newer behaviour asset are preserved when a document is
round-tripped but are not executable until their type is known.

The blackboard does not mirror character, health, or navigation components.
Bindings read those facts through named read-only inputs when evaluating a
condition.

### Goals

A goal has a stable name, eligibility conditions, a bounded score expression,
cooldown, minimum hold time, and one behaviour plan entry point. Goal scoring
runs at a declared cadence and after relevant fact changes.

Equal scores are resolved by explicit author priority then stable goal name.
Switching records the reason and source facts. A goal cannot oscillate every
tick unless the author explicitly chooses zero hold time and zero hysteresis.

Scripts may add, remove, enable, disable, or directly request named goals on
the authority. A forced goal still passes validity and permission checks.

### Domain-specific behaviour plan

The authored behaviour format supports only the concepts needed for agent
decisions:

- ordered sequence and selector;
- typed condition;
- bounded utility selection;
- wait by fixed ticks;
- set or clear a declared blackboard key;
- request path, follow path, face target, and stop;
- emit a bounded named character action;
- wait for a named gameplay acknowledgement;
- succeed, fail, retry with a bound, and choose a declared fallback;
- call a registered native task by stable name.

It has no arbitrary loops, recursion, allocation, reflection, script bytecode,
file access, network calls, or direct component writes. Every retry, child
count, task duration, and active stack depth is bounded and validated before
publication.

Custom game logic may implement a registered native task or use a script to
write goals and facts outside the plan. The format does not grow into a second
general programming language.

### Compile format

Studio may use its `nodegraph` library to edit a behaviour document. Editor
node ids, positions, groups, comments, and collapsed state are authoring data.
The engine compiler consumes a canonical domain document containing stable node
types, named ports, typed fields, and stable connections.

Compilation performs:

- schema and version validation;
- port type checking;
- cycle rejection except for explicit bounded retry nodes;
- reachability and dead-node diagnostics;
- key and task name resolution;
- bound aggregation for stack depth, active tasks, and work per tick;
- canonical ordering and content hashing;
- emission of an immutable flat `BehaviourPlan`.

Runtime evaluates the flat plan. It never walks Studio graph objects. Missing
node or task types keep the source document openable and visibly broken, but a
broken document cannot publish an executable artifact.

### Task lifecycle

A task returns `Running`, `Succeeded`, `Failed`, or `Cancelled` plus a bounded
reason code. Entry, tick, exit, and cancellation are explicit. Changing goal
cancels the old active task stack before entering the new one.

A task cannot hold an ECS pointer across ticks. Persistent task state is a
bounded row indexed by stable plan slot and agent. World references are stable
names or same-world entity handles validated on every use.

One agent executes at most the configured number of plan operations per tick.
Exhausting the budget yields `Running` at the same program counter and records
an over-budget diagnostic. It does not spin until completion.

## Bot drivers and character intent

### Driver pipeline

The server bot pipeline is:

1. perception commits a memory snapshot;
2. goal selection chooses or keeps one goal;
3. the behaviour plan requests a route or action;
4. navigation computes or follows the route;
5. local avoidance resolves preferred steering;
6. `BotController` writes one neutral `CharacterIntent`;
7. the character system consumes intent and reports actual progress next tick.

The intent includes desired movement, facing, jump and action edges, source
tick, sequence, and expiry as defined by the character plan. It contains no bot
pointer or player id.

### Control lease

A higher-level driver authority owns the control lease. The bot controller may
write intent only while its stable driver token holds that lease. A player may
possess the same character later through `Players`; possession changes revoke
the bot lease and neutralise its intent atomically.

An inactive player character may be handed to a bot without respawning or
changing the character core. A player can then swap back by changing the lease.
No character has two independent writers. Shared control would require an
explicit mixer that resolves one intent before this pipeline.

### Feedback

Navigation compares planned motion with character feedback:

- actual root position and velocity;
- grounded and active traversal mode;
- accepted or refused named actions;
- blocked and collision facts;
- death or disabled state.

It does not read animation pose or infer success from visible movement. A dead
or disabled character stops its bot route and emits neutral intent.

### Deterministic variation

Patrol choice, idle duration, and other stochastic behaviour use named random
streams derived from world seed, agent stable identity, behaviour content, and
an explicit sequence. Random draws occur only at named decision points and are
recordable.

Wall-clock time, thread id, container layout, and frame rate never seed agent
behaviour.

## Fixed-tick schedule

The complete authority schedule is:

1. commit authored and replicated writes at the mutation barrier;
2. freeze the published navigation snapshot and dynamic source revisions;
3. gather navigation agents, obstacles, sensor descriptions, and gameplay
   events in stable order;
4. update the dynamic overlay and navigation spatial indexes;
5. gather perception candidates and emit generic world queries;
6. physics resolves the bounded world-query batch;
7. reduce complete perception memories;
8. evaluate due goals and behaviour plans;
9. close and sort path requests;
10. solve the path batch, joining all worker jobs;
11. repair and advance route corridors;
12. solve local avoidance from one frozen crowd snapshot;
13. commit steering, route, perception, and behaviour state in stable order;
14. bot drivers resolve and publish character intent;
15. character controllers consume the intent;
16. record profiling, overflow, and diagnostic counters.

Structural ECS changes are queued and applied at the next allowed barrier.
Parallel phases write disjoint preallocated output slots. A worker does not add
or remove components.

Authoring bake jobs run outside this sequence. The fixed tick sees only the old
published artifact or a complete replacement committed at step 1.

## World streaming integration

### Tile residency

Navigation tile coordinates align with the spatial cell convention selected by
the world-streaming system. Tile size may be a divisor or multiple of a stream
cell, but the mapping is exact and stored in the artifact.

Residency requests come from:

- active authoritative agents;
- current route corridors plus a bounded lookahead;
- pending path origins and destinations;
- portal and traversal-link endpoints;
- Studio preview cameras.

Navigation reports requested tile content names and priorities. The streaming
system owns I/O, delivery, decompression scheduling, and eviction. Navigation
validates and adopts completed immutable tiles at a barrier.

### Coarse metadata

The cluster graph and tile bounds are small enough to remain resident for the
published region. This allows a long query to distinguish `NoPath` from
`Unloaded` and request the missing tiles on an otherwise viable coarse route.

The detailed search never invents a route through unavailable data. Depending
on request policy it returns `Unloaded`, queues a residency request, or returns
a labelled partial route to the last resident boundary.

### Pinning and eviction

An active detailed search pins its bounded tile set until that fixed-tick batch
ends. An active route pins only its current tile and bounded lookahead. Pins are
counts owned by the streaming system, not raw pointers held by navigation.

Eviction cannot remove a tile during a query. After eviction, a route that
reaches the missing lookahead becomes `Unloaded` and stops or follows its
declared fallback. It does not walk through absent navigation.

### Seam validation

Adjacent tiles carry matching canonical boundary keys. Loading a tile verifies
its neighbour declarations against every resident neighbour. A mismatch is a
content error with tile coordinates and hashes, not a tiny gap patched at
runtime.

Cross-world routes pin no foreign pointer. They request the next world's stable
name and entry region from the universe or session layer. Transfer occurs only
after that layer confirms destination readiness.

## Save, asset, and replication model

### Saved world data

Saved authoring data includes:

- navigation service settings;
- agent profiles and named areas;
- static modifiers and authored links;
- navigation agents and their declared settings;
- perception sensor definitions;
- bot controllers, goal sets, and behaviour asset references;
- blackboard fields marked saved;
- the published artifact content name or hash.

Compiled polygons are not duplicated inside every world snapshot. They are
immutable content-addressed assets referenced by stable name or hash.

### Transient data

The following are derived and not saved by default:

- decoded tile indexes and polygon handles;
- open sets, closed sets, query batches, and worker slots;
- local avoidance neighbours and constraints;
- current perception query hits;
- debug draw geometry;
- Studio bake progress;
- cache hit and timing counters.

Current route, behaviour program counter, perception memory, and random stream
sequence are included in deterministic recordings. A normal authored world
save may omit them unless it is a live-state save.

### Asset format

Navigation and behaviour assets use versioned envelopes with:

- format kind and version;
- uncompressed length and bounded section counts;
- compatibility version;
- source signature;
- content root;
- canonical payload sections.

Parsing and construction are separate. The parser validates lengths, counts,
indices, finite numbers, names, topology, bounds, and cross references into a
checked description. Only then may runtime structures allocate and build.

Tile payloads are independently addressable so streaming does not fetch the
whole world for one cell. The manifest groups likely neighbouring tiles into
delivery bundles without changing their individual content roots.

### Replication

The server owns navigation, perception, goals, behaviour, bot intent, and path
publication. Clients normally receive only gameplay-visible state:

- published navigation content identity when they need local prediction or
  debug views;
- replicated service and agent properties marked public;
- bot character transforms through ordinary scene replication;
- selected target, route status, or goal only when the game or Studio requests
  diagnostics;
- named traversal actions that presentation must animate.

Open sets, full paths for every bot, perception memory, and behaviour stacks are
not broadcast by default. Studio attaches a permission-checked diagnostic
stream with explicit byte and update-rate budgets.

A client path request is advisory. The server validates the requester, profile,
origin, destination, permitted regions, query cadence, and budgets before using
it for gameplay. A client-supplied path is never authoritative.

### Recording and replay

A deterministic recording includes:

- published navigation content identity and publication tick;
- authoritative path request inputs and statuses;
- goal changes and their reason codes;
- random stream seeds and sequence positions;
- traversal action requests and acknowledgements;
- bot intent written each tick;
- overflow and over-budget decisions that altered behaviour.

Debug traces may additionally record chosen corridors and avoidance neighbours,
but those are diagnostic payloads with bounded retention.

## Script API

### Basic path use

The simple case remains simple:

```luau
local NavigationService = game:GetService("NavigationService")
local path = NavigationService:CreatePath({ AgentProfile = "Humanoid" })

local status = path:ComputeAsync(origin, destination)
if status == Enum.NavigationPathStatus.Success then
	for _, waypoint in path:GetWaypoints() do
		moveTo(waypoint.Position)
	end
end
```

`GetWaypoints()` returns a copied bounded array. Repeated reads do not expose a
mutable view into a worker or tile cache.

### Agent control

Scripts may:

- assign a destination or a previously computed path;
- cancel a route;
- read route status, steering, next waypoint, and current link;
- change allowed modes, area costs, avoidance masks, and repath policy;
- listen for route blocked, link requested, route completed, and route failed
  events;
- supply a named traversal acknowledgement;
- set declared goal and blackboard values on authority-owned agents.

Every mutating method validates authority and finite bounded input. Setting a
destination creates a request for the next closed path batch. It does not run
A* immediately inside a property setter.

### Perception and behaviour

Scripts can read copied perception facts and subscribe to bounded gained,
updated, forgotten, and overflow events. They can declare target filters from
stable tags and names. They cannot install an arbitrary predicate callback into
a parallel sensor pass.

Behaviour plans expose active goal, active task path, last transition reason,
budget use, and failure diagnostics. Runtime source editing is a Studio or
asset-authoring operation. Live games switch to another validated immutable
plan by content identity at a barrier.

### Binding parity

All classes, properties, enums, methods, and events are registered once and
generated into Luau and TypeScript declarations. Tests compare both surfaces.
Neither language receives a private implementation-only route.

## Studio tools

### Navigation dock

A `View > Navigation` dock contains:

- active world and published artifact;
- agent profile and area editors;
- bake bounds, settings, and source filters;
- incremental or full bake controls;
- stage progress and cancellation;
- tile, polygon, link, and byte counts;
- validation errors grouped by tile and source;
- cache hit rate and elapsed versus worker busy time;
- publish and revert controls.

The dock stores no duplicate settings. It edits scene Instances and reads the
current `NavigationBake` state.

### View overlays

Overlays may show:

- walkable polygons by area;
- tile and cluster boundaries;
- clearance and ledge edges;
- traversal links and direction;
- dynamic blocked polygons;
- active routes, corners, and stale sections;
- avoidance radius, preferred velocity, and resolved velocity;
- perception cones, candidates, occlusion rays, and remembered targets;
- current goal and bounded behaviour task path.

Overlay geometry is rebuilt only when its source signature changes. Hidden
overlays and docks do no tessellation, upload, or query work.

### Behaviour editor

Studio uses the existing nodegraph library as a view and editing tool. The
navigation module owns the domain schema and compiler. The editor provides:

- searchable domain-specific nodes;
- typed ports and inline bounded fields;
- compile diagnostics attached to nodes and ports;
- live but rate-limited inspection of one selected agent;
- breakpoint-like pause for Studio simulation only;
- content diff and compiled bound summary;
- save as immutable behaviour asset.

The editor does not execute the graph. Closing it does not change AI behaviour.

### Path probe

An author can place origin and destination probes, choose a profile, and inspect
the exact path status, projection, corridor, costs, links, tile loads, expanded
nodes, and budget use. The probe uses the same engine query as scripts and bots.

There is no Studio-only pathfinder that can disagree with the server.

## Security and limits

### Untrusted content

Game files, navigation assets, behaviour assets, and server-delivered tile
bytes are hostile inputs. Validation must reject:

- oversized files, sections, names, and arrays;
- integer overflow in count times stride calculations;
- non-finite transforms, vertices, costs, and radii;
- invalid polygon winding, indices, adjacency, and tile bounds;
- negative edge or area costs;
- links with missing endpoints, invalid worlds, or unbounded actions;
- behaviour cycles without a bounded retry node;
- undeclared blackboard keys or type mismatches;
- plans whose aggregate depth or work exceeds engine limits;
- compressed payloads whose declared output exceeds the limit.

Navigation and behaviour decoders require fuzz targets. A failed asset leaves
the old published snapshot usable and reports a bounded diagnostic without host
paths, tokens, or raw addresses.

### Runtime budgets

Hard settings cap:

- published and resident tiles per world;
- vertices, polygons, links, clusters, and bytes per tile;
- dynamic obstacles and changed polygons per tick;
- path requests per tick and per script identity;
- search expansions, cost, distance, and waypoints per request;
- active routes and pinned tiles;
- avoidance neighbours and constraints per agent;
- perception candidates, world queries, hits, facts, and events per sensor;
- goals, blackboard keys, behaviour operations, task depth, and retries per
  agent;
- debug stream bytes and retained history.

Every cap has a status and counter. Silent drop is allowed only for a declared
best-effort diagnostic stream, never for authoritative intent or action edges.

### Authority checks

Clients cannot:

- publish navigation or behaviour content;
- assign a bot controller or control lease;
- authoritatively set another agent's destination or goal;
- forge perception facts or traversal completion;
- raise their own query budget;
- ask the server to stream arbitrary host paths or unsigned assets.

Server scripts are still bounded. Authority is permission, not infinite work.

## Diagnostics and observability

Every major branch records a bounded reason code. Important examples are:

- projection failed;
- start or destination tile unloaded;
- profile incompatible;
- no coarse route;
- detailed search exhausted;
- expansion or waypoint budget reached;
- corridor stale;
- obstacle blocked;
- link disabled, full, timed out, or refused;
- perception query overflowed;
- goal held by hysteresis;
- plan operation budget reached;
- control lease lost;
- intent expired.

Metrics distinguish gauges from drained counters. Required gauges and counters
include:

- resident tiles, bytes, polygons, links, agents, routes, and pinned tiles;
- bake tile counts, cache hits, encoded bytes, and stage milliseconds;
- path requests, successes, partials, failures, expansions, and wall time;
- route repairs and complete repaths;
- avoidance neighbours, constraints, stops, and overflows;
- perception candidates, exact queries, visible targets, facts, and overflows;
- goal switches, plan operations, active tasks, cancellations, and budget hits;
- bot intents, neutralisations, and rejected lease writes.

`ENGINE_PROFILE` scopes cover source collection, each bake stage, batch path
search, corridor generation, dynamic overlay update, perception gather and
reduce, avoidance, behaviour evaluation, and bot intent publication. Worker
durations are reported through the existing reported-span path after join.

Logs use world name, request id, agent stable key, artifact content, and reason
code. A dynamic log level may trace one agent without logging every crowd.

## Migration from current character control

The migration keeps the engine working after each phase.

1. Land the neutral character intent and control lease cuts described by the
   character plan. Existing player input writes the new intent.
2. Add generic scene world-query request and result rows. Physics resolves them
   while existing direct query APIs remain for local callers.
3. Add navigation scene classes and registration with no active runtime.
4. Add the `navigation` module, artifact decoder, one test tile, and path probe.
5. Add static bake from current scene colliders and publish one complete tile
   set.
6. Add bounded path queries and corridor following with no avoidance or AI.
7. Add `NavigationAgent` steering and a simple server bot that writes neutral
   character intent.
8. Move existing examples that need bots to the generic path. Delete any
   temporary direct-transform bot movement introduced during bring-up.
9. Add dynamic obstacles, route repair, and deterministic local avoidance.
10. Add perception facts and sensors through the generic world-query batch.
11. Add goal selection and the bounded domain-specific behaviour compiler.
12. Add Studio baking, overlays, path probes, and behaviour editing.
13. Add tile asset delivery and world-streaming residency integration.
14. Add climb, jump, action, and portal links. Add swim and fly domains only
    after their character locomotion modes exist.

There is never a second player-only pathfinder or an AI-specific movement
controller. Each temporary compatibility adapter has an owner and a deletion
phase.

## Delivery phases and gates

### Phase 0: contracts and measurements

Deliver:

- representative authored scenes for flat ground, stairs, caves, stacked
  floors, narrow doors, a moving blocker, and a crowd;
- release-build measurements for current spatial and exact query cost;
- declared default limits and target agent counts;
- the final module graph proposal and component purpose rows;
- canonical coordinate, quantisation, and tie-break rules.

Gate:

- the same source snapshot produces byte-identical canonical artifacts across
  worker counts on each supported platform;
- no proposed dependency runs upward or sideways;
- test scenes have expected route answers written independently from the
  implementation.

### Phase 1: artifact and static surface bake

Deliver:

- versioned checked tile descriptions and codecs;
- source collection from current colliders and modifiers;
- surface raster, regions, contours, polygons, adjacency, and seams;
- full and incremental tile builds;
- cooperative cancellation and progress;
- content-addressed publication.

Gate:

- malformed assets fail before construction and pass fuzzing;
- unchanged tiles are cache hits with zero bake-stage work;
- changed bounds invalidate exactly the intersecting halo tiles;
- all test tile seams match exactly;
- cancellation leaves no published partial artifact.

### Phase 2: queries and route following

Deliver:

- point projection;
- hierarchical deterministic A*;
- corridor and funnel waypoints;
- query batching and fixed-tick result commit;
- `NavigationPath` and script bindings;
- `NavigationAgent` route following and repair.

Gate:

- worker count, request order within canonical sorting, and repeated runs select
  identical routes;
- every failure and overflow status is covered;
- a route never references a stale tile pointer;
- path request and waypoint caps hold under hostile input;
- release profiles meet the recorded budgets for small, medium, and long maps.

### Phase 3: bot driver and links

Deliver:

- `BotController` and control lease validation;
- neutral intent output and immediate neutralisation on loss;
- authored door, jump, drop, climb, and portal links;
- named traversal actions and acknowledgements;
- character feedback and stuck detection.

Gate:

- a bot and a player can drive the same character at different times without
  changing character components;
- a player possessing a bot character revokes the bot before the next character
  intent read;
- link failure never sets character state directly;
- cross-world link data contains no pointer or foreign entity handle.

### Phase 4: obstacles and crowds

Deliver:

- dynamic blocking and cost overlay;
- deterministic local avoidance;
- link capacity and queue points;
- bounded crowd updates and debug views.

Gate:

- the same crowd produces the same steering under different worker counts;
- overflow chooses the same canonical neighbour prefix;
- removing an obstacle restores the old mesh without a bake;
- avoidance cannot steer through a blocked corridor edge;
- release profiles name the serial and parallel crossover.

### Phase 5: perception

Deliver:

- sight, hearing, proximity, damage, and navigation sensors;
- generic world-query batches resolved by physics;
- bounded perception memory and events;
- per-agent diagnostics and filters.

Gate:

- occlusion uses the same exact collider truth as other physics queries;
- complete perception commits before behaviour reads it;
- sound gameplay events do not depend on the client audio mixer;
- query and memory overflow are visible and deterministic;
- no sensor callback runs inside a parallel gather pass.

### Phase 6: goals and behaviour plans

Deliver:

- typed blackboards and goals;
- bounded goal scoring and hysteresis;
- domain-specific behaviour documents and compiler;
- immutable runtime plans and task lifecycle;
- Studio graph editor and live selected-agent inspection.

Gate:

- invalid cycles, types, bounds, and missing tasks cannot publish;
- runtime uses no Studio nodegraph objects;
- operation budgets suspend safely without losing task state;
- goal state does not duplicate character or animation state;
- custom scripts can drive goals without extending the plan language.

### Phase 7: streaming and extended domains

Deliver:

- tile delivery, residency, pinning, and eviction;
- coarse routes through unloaded regions;
- partial and unloaded statuses;
- world and portal route segments;
- water and air volumes after matching character modes exist.

Gate:

- a tile cannot be evicted during its fixed-tick query;
- missing detail never becomes a complete route;
- adjacent streamed tiles validate canonical seams;
- cross-world transfer waits for destination readiness;
- swim and fly routes use three-dimensional clearance tests.

## Focused test plan

### Bake and format tests

- empty source world;
- one floor and one agent profile;
- steep slope exclusion at the exact threshold;
- step just below, at, and above maximum height;
- head clearance just below, at, and above agent height;
- cave and bridge with two walkable levels at one horizontal coordinate;
- modifier overlap order and named area costs;
- tile halo geometry and matching boundary keys;
- source removal invalidating old bounds;
- identical bake under one worker and many workers;
- malformed counts, indices, NaNs, links, and compressed lengths;
- cancellation and stale late worker result;
- artifact round trip and stable content root.

### Query tests

- origin equals destination;
- projection outside navigation;
- one corridor, equal-cost corridors, and disconnected islands;
- stacked floor selection by vertical error;
- one-way and disabled links;
- area-cost route choice;
- partial, unloaded, stale, invalid, and over-budget results;
- exact expansion limit and waypoint limit;
- funnel corners around a narrow obstacle;
- mandatory link waypoint preserved by simplification;
- canonical results across worker counts and repeated runs;
- batch commit in request order after out-of-order worker completion.

### Route and traversal tests

- moving target below and above repath threshold;
- agent displaced inside and outside corridor tolerance;
- obstacle appearing on the next segment;
- jump accepted, refused, timed out, and landed outside its volume;
- climb request accepted only by the character controller;
- door link capacity and deterministic queue order;
- portal route using the seam transform;
- route made stale by publication revision;
- lost tile residency stopping at the last valid point.

### Avoidance tests

- two agents meeting head-on;
- symmetric crossing with stable id tie-break;
- dense doorway and queue points;
- stationary obstacle and moving obstacle;
- avoidance mask and priority;
- neighbour overflow prefix;
- fixed iteration exhaustion fallback;
- same output under serial and parallel evaluation;
- no movement when preferred velocity is zero.

### Perception tests

- empty sensor set;
- sight inside and exactly on cone and range boundaries;
- visible, occluded, and partially visible targets;
- ignored owner collider;
- candidate and exact-query overflow;
- hearing falloff with and without occlusion;
- event expiry by tick;
- proximity tag filters;
- damage fact from authoritative event;
- memory merge, replacement, expiry, and full-cap policy;
- deterministic fact order across worker counts.

### Goal and behaviour tests

- no eligible goal;
- equal goal scores and stable tie-break;
- hold time, hysteresis, cooldown, and forced goal refusal;
- sequence, selector, utility, wait, retry, and fallback nodes;
- cancellation order on goal change;
- missing native task and invalid blackboard type;
- exact operation and stack-depth limits;
- plan resume after a budget yield;
- explicit seeded choice and replayed sequence;
- source document preserved when it contains an unknown node;
- unknown node cannot publish executable content.

### Driver, script, and network tests

- bot drives a character with no `Player`;
- player revokes bot control and later returns it;
- bot disable and destruction neutralise intent;
- dead character stops path and actions;
- Luau and JavaScript declaration parity;
- hostile path budgets are clamped;
- client cannot publish artifacts, facts, goals, or leases;
- diagnostic replication stays within byte and cadence limits;
- save and live-state recording restore the declared facts only.

Integration tests should cover the complete useful cuts: bake then query, query
then follow, perceive then choose, choose then drive, and stream then continue.
Do not create one smoke test per deleted placeholder or one test per trivial
getter.

## Release profiling plan

All shipped-cost claims use the `release` preset and name the platform, scene,
tick rate, worker count, agent profile, tile settings, resident tile count,
agent count, and query cadence.

Required bake measurements:

- source collection milliseconds and copied bytes;
- each tile stage wall and busy milliseconds;
- tiles per second by source triangle count;
- peak and retained bytes;
- encoded bytes per polygon and per square metre;
- full versus incremental build;
- cache hit with zero build work;
- serial versus parallel crossover.

Required runtime measurements:

- tile adoption and index update time;
- path latency and expansions by route length;
- query batch scaling and join idle time;
- route repair versus full repath;
- dynamic overlay update by obstacle count;
- avoidance time by agent density and neighbour cap;
- perception candidate and exact-query time;
- goal and behaviour operations per agent;
- full AI tick at target agent counts;
- live, peak, and churn bytes;
- replication and diagnostic bytes per second.

Initial budgets are hypotheses written in Phase 0 after measuring representative
scenes. They are not hard-coded folklore. Every parallel path is compared with
its serial form. Every cache hit is checked for zero downstream work, upload,
or allocation.

Soak tests run settled crowds, repeated target changes, streamed tile churn,
and repeated behaviour switches. A leak requires a sustained live-byte slope
with a credible fit, not a high allocation total.

## Open decisions

These choices require measurements or contracts from another planned system.
They do not change the ownership cuts above.

- Choose default tile size, raster cell size, height precision, and contour
  tolerance from the Phase 0 test scenes and release profiles.
- Select the exact bounded velocity-obstacle solver after comparing stable
  output, crowd flow, and cost against the fixed-iteration fallback contract.
- Align navigation tiles with the final world-streaming cell convention before
  either file format becomes stable.
- Decide whether normal world saves preserve active routes and behaviour stacks
  or reserve those fields for live-state snapshots and recordings.
- Choose the canonical behaviour source encoding when the asset schema is
  implemented. The compiled plan and validation rules remain format-neutral.

## Explicit non-goals

The first complete system does not include:

- a universal AI virtual machine;
- a general script language inside behaviour graphs;
- direct behaviour writes to transforms, velocities, character states, or
  animation states;
- player ownership, player input, cameras, teams, respawn, or matchmaking;
- motion matching or animation pose selection;
- learned navigation, neural policies, or online model training;
- automatic arbitrary parkour generation;
- a full strategic planner across every game system;
- world-sized fully dynamic navigation mesh rebuilding every tick;
- authoritative client paths, perception, goals, or bot intent;
- one navigation representation forced onto walk, swim, and fly;
- exact cloth, vehicle, or ragdoll motion planning;
- editor node objects in runtime builds;
- cross-world pointers or foreign ECS handles;
- random crowd jitter used to hide unstable ordering.

Games may build richer planners, squad tactics, cover systems, influence maps,
and domain-specific sensors on the typed facts, goals, requests, and intent
cuts. Those systems should arrive only when a real game demonstrates the need.

## Completion definition

The planned system is complete when:

- current collider geometry bakes into deterministic content-addressed surface
  tiles with incremental invalidation and visible progress;
- bounded path queries return canonical success, partial, unloaded, failure,
  stale, and over-budget results within the defined fixed-tick phase;
- walking routes, authored traversal links, dynamic obstacles, and local
  avoidance work without writing character state or transforms;
- perception commits complete bounded facts before behaviour reads them;
- typed goals and immutable bounded behaviour plans select actions without
  becoming a general virtual machine;
- a server bot drives a player-independent character only through neutral
  `CharacterIntent` and relinquishes it atomically to another driver;
- saved worlds reference immutable navigation and behaviour assets by stable
  content identity;
- replication sends only permission-checked gameplay and diagnostic state;
- streamed tiles, portals, and cross-world route segments use copied stable
  identities with no pointer across a world boundary;
- Luau and JavaScript expose the same simple and advanced surfaces;
- Studio can bake, cancel, validate, publish, probe, inspect, and edit without
  owning runtime semantics;
- focused unit, integration, format, fuzz, determinism, and authority tests
  pass;
- release profiling demonstrates bounded cost at the declared target scenes and
  agent counts;
- temporary movement, path, and AI compatibility paths have been deleted.
