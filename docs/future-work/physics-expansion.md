# Physics expansion plan

## Goal

Expand the existing fixed-tick rigid-body pipeline into a complete gameplay
physics system. The finished system supports authored joints, driven mechanisms,
vehicles, ragdolls, named collision groups, reliable fast-body collision,
triggers, exact world queries, physical materials, and bounded destruction.

This is a behavioural plan. Performance experiments remain in
[`OPTIMISATIONS_PHYSICS.md`](OPTIMISATIONS_PHYSICS.md). An optimization from
that document enters this plan only after a release-preset profile names the
scene, cost, and crossover that justify it.

Physics remains one authority over body motion. Weld assemblies, general
constraints, character capsules, vehicles, and ragdolls must feed the same
pipeline. They must not introduce private integrators or write transforms after
physics has published its tick.

## Existing foundation

The engine already contains most of the storage and several important runtime
paths. Expansion starts by wiring and generalizing them.

| Existing piece | Current role | Required change |
|---|---|---|
| `scene::RigidBody` and `scene::Simulated` | Authored body properties and participation | Add explicit lifecycle operations and derived assembly mass data without storing solver state in scene components |
| `scene::Collider` | Exact shape, layer, mask, geometry, and trigger flag | Resolve named collision groups into its existing hot masks and add compound ownership rules |
| `scene::PhysicsProperties`, `scene::Surface`, and `SurfaceTable` | Density, friction, and elasticity | Add explicit pair-combine rules and contact overrides without duplicating material data |
| `physics::PhysicsWorld` | Per-world broadphase, manifolds, solver rows, sleeping, CCD counters, and rigid assemblies | Own transient joint rows, islands, break reports, query snapshots, and diagnostic counters |
| `IntegrateMotion`, `BroadPhase`, `NarrowPhase`, `Solve`, and `Publish` | Fixed-tick contact pipeline | Fold general constraints and lifecycle events into the same ordered step |
| `SweepFastBodies` | Conservative fast-body sweep before broadphase sync | Grow into configurable CCD modes with an exact time-of-impact path where needed |
| `scene::Constraint` | Authored generic six-axis joint | Wire into the sequential impulse solver and preserve its one-component, many-class model |
| `scene::JointInstance` and `scene::WeldConstraint` | Legacy welds and rigid assembly projection | Migrate their runtime work into the common constraint and assembly preparation path |
| `physics::Raycast`, overlap queries, and `ShapeCast` | Allocation-free exact queries over current indexes | Add reusable filter objects, complete hit data, query batches, and exact casts |
| Contact events | Deterministic began, persisted, and ended transitions | Expose bounded trigger and contact event surfaces with stable ordering |
| `scene::NetworkOwner` | Expresses client simulation ownership | Make authority meaningful only after validation, correction, and revocation exist |

The current generic `Constraint` is registered, saved, visible to scripts, and
inspectable, but physics does not solve it. Rigid `Weld` and `WeldConstraint`
instances use a separate assembly projection path. The first migration target
is therefore known: keep the public classes, route both implementations through
one prepared constraint and assembly model, then delete the redundant runtime
path.

## Non-negotiable rules

1. The ECS owns authored, saved, and replicated facts. `PhysicsWorld` owns only
   transient indexes, caches, accumulated impulses, sleeping state, and reports.
2. Simulation advances only on the fixed physics step. Presentation may
   interpolate published transforms but may not feed a later result into an
   earlier tick.
3. Every iteration with visible effects has a canonical order. Entity ids,
   stable names, and explicit sequence numbers break ties.
4. A name crosses a file, process, or network boundary. Dense group bits,
   solver indexes, body slots, and island indexes stay local to one process.
5. No pointer crosses a world boundary. Portal and streaming integration use
   copied descriptions or messages.
6. Scene stores data at L7. Physics at L8 reads and writes permitted scene rows.
   Script, replication, Studio, and product code consume physics through their
   own higher-layer adapters.
7. One body has one motion authority per tick. A character controller,
   constraint drive, vehicle input, or network owner contributes intent or
   impulses to physics rather than running a competing integrator.
8. Parallel work must join inside the same physics step. The output must not
   depend on worker count, job completion order, or wall-clock timing.
9. All untrusted counts, forces, speeds, query sizes, and authored graphs are
   bounded before allocation or simulation.
10. No feature is complete until its old path is removed, its save and wire
    policy is named, and its release-preset cost is measured.

## Body and assembly model

### Body kinds

Keep the current dynamic, kinematic, and static distinction.

- A dynamic body integrates forces and responds to contacts and constraints.
- A kinematic body follows an authority-authored target and contributes velocity
  at contacts. It has infinite effective mass in the solver.
- A static body has no per-tick motion row and changes only through an authored
  structural edit.

Changing body kind is an owning-thread mutation committed at the structural
barrier. It wakes the complete connected island, invalidates cached manifolds,
updates the correct broadphase index, and clears stale accumulated impulses.
It never leaves the same body in static and dynamic indexes for one tick.

`RigidBody::Mass` remains the solver input. Density-derived mass is resolved by
the existing scene rule when authored physics properties or collider dimensions
change. Mass must be finite and greater than zero for a dynamic body. Invalid
script values are rejected at the setter rather than repaired inside the hot
solver loop.

### Compound bodies and rigid assemblies

A rigid assembly has one canonical root, one integrated linear and angular
state, and any number of collider-bearing child parts. Its mass, centre of mass,
and inertia tensor are derived from all active child colliders in root-local
space. The derivation is cached by an assembly revision and rebuilt only when a
part, collider, mass property, or rigid edge changes.

The canonical root is selected deterministically. Prefer an explicitly authored
assembly root when valid, otherwise use the lowest entity id in the connected
component. A root selection is process-local derived state and is never saved.

Rigid links still preserve authored part transforms. The runtime must not
project every child independently before and after contact solving. Contacts on
any child resolve against the assembly body, then one publish pass derives all
child transforms from the solved root pose. This removes the current split
between rigid assembly projection and contact response.

Cycles in rigid links are accepted only if their authored relative transforms
agree within a documented tolerance. Invalid cycles are disabled as a complete
set and produce one bounded diagnostic. Silently choosing one contradictory
edge would make the result depend on traversal order.

### Lifecycle and sleeping

Physics exposes explicit owning-thread operations for:

- wake one body or its full constraint island
- apply force, torque, impulse, and impulse at position
- set linear and angular velocity
- set a kinematic target
- teleport a body or assembly
- rebuild mass properties

Every operation states whether it wakes the target. A teleport invalidates
contacts and CCD history. A velocity or impulse wakes the connected island. A
read-only query never wakes anything.

Sleeping remains solver-owned. An authored `Sleeping` component would duplicate
the fact and force sleeping bodies back into ordinary ECS scans. The solver
tracks rest time, speed thresholds, force changes, and connected-island state.
An island sleeps as a unit and wakes as a unit so a resting chain cannot leave
one link frozen while another moves.

Static and sleeping bodies perform no integration. They remain queryable and
may still generate a wake when an awake body, moving kinematic body, changed
constraint target, or explicit operation reaches them.

## Constraints and joints

### One six-axis representation

`scene::Constraint` remains the authored representation. Its two attachment
frames define three linear and three angular axes. Each axis is locked, limited,
or free. Drives use the same axes, target frame, stiffness, damping, force cap,
and torque cap.

The registered classes remain presets over that representation:

| Public class | Axis setup |
|---|---|
| `BallSocketConstraint` | Linear axes locked, angular axes free or limited |
| `HingeConstraint` | One angular axis free or limited, all other axes locked |
| `PrismaticConstraint` | One linear axis free or limited, all other axes locked |
| `CylindricalConstraint` | One linear and matching angular axis free or limited |
| `RopeConstraint` | Distance limited on extension with no compression row |
| `SpringConstraint` | Soft limited or driven linear axis |
| `Weld` and `WeldConstraint` | All six axes locked, eligible for rigid assembly folding |

There is no runtime `Kind` switch beside the axis modes. Class construction
copies safe defaults and the visible properties remain the source of truth.

### Prepared joint rows

At the start of each physics step, gather enabled constraints in entity-id order.
Resolve attachments, their owning assembly bodies, and local frames once. Reject
missing, destroyed, cross-world, or self-referential endpoints before row
generation. A null second endpoint means the fixed world frame.

Each active axis produces a standard sequential-impulse row:

- locked rows correct positional and velocity error
- limit rows activate only outside their permitted interval
- motor rows target velocity with a finite impulse cap
- servo rows target position through stiffness and damping
- friction rows resist motion up to their configured cap

Angular error uses a representation with a documented shortest-arc rule.
Singular configurations must choose a stable fallback axis from attachment
space rather than from floating-point noise.

Warm-start impulses live in a sorted cache keyed by constraint entity and axis.
They are invalidated when endpoints, modes, limits, target, or solver rate
change. The cache never reaches snapshots or replication.

### Motors, servos, and limits

Extend the authored constraint data with per-axis drive mode and target velocity
only if the existing target, stiffness, and damping cannot state the required
behaviour cleanly. Do not add a second motor component.

All motors and servos have finite force or torque caps. Limits define lower and
upper bounds in attachment-local units. Setters reject a lower bound greater
than its upper bound. A limit may expose restitution and contact distance, but
only after tests prove a single global slop is insufficient.

Drive targets are inputs to the fixed tick. A Studio preview may scrub them,
but runtime script writes take effect at the next mutation barrier. This keeps a
drive from changing halfway through island solving.

### Breakage

Constraints may opt into finite break force and break torque. The solver records
the largest accumulated linear and angular impulse for the step, converts each
with the actual step length, and compares after all iterations. A break decision
therefore cannot remove rows midway through a solve.

Broken constraints become disabled through a deferred owning-thread mutation.
Physics emits one ordered break report containing the constraint entity, both
endpoints, measured force and torque, tick, and reason. The authored instance is
not destroyed automatically. Scripts may repair or destroy it later.

Break thresholds are authoritative and server validated. Clients may predict a
visual break, but the replicated `Enabled` state and event sequence settle the
result.

## Solver islands and bounded parallel work

Build islands from awake dynamic bodies, contacts, and active non-rigid joint
edges. Kinematic and static bodies constrain an island but do not merge two
otherwise independent dynamic islands through a shared immovable body.

Island construction and solving follow a canonical order:

1. gather bodies by entity id
2. gather contact and joint edges by stable key
3. union connected dynamic bodies with deterministic tie breaking
4. sort islands by their lowest body id
5. solve rows within each island in canonical row order
6. publish island results by body id

Independent islands may run on workers. Each job writes only its island's body
and row ranges. The owning thread joins all work before publish. No atomic
floating-point accumulation and no shared work-stealing order may affect the
answer.

Small islands stay inline. The dispatch threshold must be measured in release
and recorded beside the constant. Oversized islands remain serial initially.
Splitting one island into parallel colour or spatial batches belongs to the
optimization plan and requires a parity gate before adoption.

Expose counters for island count, largest island bodies, contact rows, joint
rows, serial rows, worker time, join time, and sleeping islands. These counters
describe why a scene is slow without steering runtime decisions.

## Collision groups and filtering

### Named authoring, dense runtime masks

Add one `PhysicsService` collision-group registry per world or universe as the
save format requires. Authors and scripts use stable group names. The registry
resolves them to dense local bits used by the existing `Collider::Layer` and
`Collider::Mask` checks.

The registry stores a symmetric collision matrix. Setting whether `Characters`
and `Projectiles` collide changes one authoritative relation, advances the
registry revision, invalidates affected contact pairs, and wakes affected
dynamic bodies. Pair filtering never performs a string lookup in the broadphase.

Unknown names fail clearly on authoring calls. Loading an absent group may map
to `Default` only when the serialized format explicitly requests that fallback.
Deleting a group migrates assigned colliders to `Default` in one deterministic
transaction.

The initial group count is bounded by the width of `spatial::LayerMask`.
Increasing that limit is a save and hot-layout change, not a UI-only edit.

### Pair exclusions

Assembly self-collision is disabled by default and can be enabled only through
one explicit assembly policy. Direct pair exclusions, if later required, live
in a bounded sorted physics resource keyed by ordered entity pair. They do not
become a `NoCollisionConstraint`, because collision filtering is not a solver
joint.

Constraints may request endpoint collision through one authored flag. The
filter resolves this before narrowphase and invalidates the pair when the flag
changes.

Filtering order is fixed: lifecycle validity, same assembly, direct exclusion,
group matrix, mutual layer masks, then trigger or solid narrowphase.

## Continuous collision detection

### Modes

Expose an authored CCD mode on dynamic bodies or colliders:

- `Discrete` uses ordinary overlap detection.
- `Swept` uses the current conservative fast-body sweep and clamps motion before
  broadphase synchronization.
- `Continuous` computes a bounded first time of impact for supported convex
  pairs and advances the body through the remaining step in ordered segments.

`Swept` remains the safe default for fast character capsules and common
projectiles until the exact path proves its cost. `Continuous` is opt-in for
small, fast bodies where conservative clamping causes visible errors.

### Time-of-impact rules

Continuous casting supports convex primitive and hull pairs first. Mesh and
heightfield targets use their accelerated triangle candidate path. Unsupported
pairs fall back to conservative sweep and report a diagnostic counter, never to
discrete tunnelling.

Each body has a bounded number of impact segments per step. Candidate impacts
sort by fraction, then ordered entity pair. After each impact, solve the contact,
advance the remaining fraction, and repeat until the segment budget or minimum
motion threshold is reached. Exhausting the budget clamps remaining motion and
increments a visible counter.

CCD never creates a hidden higher-rate simulation. It operates within one fixed
step, joins before publish, and emits the same contact transition ordering as a
discrete contact.

## Vehicles

Vehicles are compositions of ordinary bodies, constraints, contact materials,
and a vehicle controller. Physics does not gain a separate vehicle integrator.

The first supported vehicle is a wheeled chassis with:

- one dynamic chassis assembly
- hinge or cylindrical wheel constraints
- suspension drives with travel limits, stiffness, and damping
- wheel motor torque and brake torque
- steering targets on front wheel axes
- per-wheel contact state and slip data
- a bounded `VehicleIntent` sampled on the fixed tick

`VehicleIntent` contains steering, throttle, brake, handbrake, and a monotonic
sequence. Player input, bots, scripts, and replays may produce it. The vehicle
does not store a player reference.

Begin with physical wheel bodies because they exercise the same constraint and
contact stack shipped for every other mechanism. A later raycast-wheel mode may
be added for high vehicle counts, but it must expose compatible suspension and
contact results and must be justified by a release profile.

Tracked vehicles, hover vehicles, boats, and aircraft are not separate solver
types. They may add controllers that apply bounded forces to ordinary bodies.

Network authority for a vehicle covers its complete constrained assembly.
Ownership cannot be split wheel by wheel. The server validates speed, force,
possession, and divergence, and may revoke authority atomically.

## Character ragdolls

The character plan keeps the default upright character on one kinematic capsule.
Ragdoll is an explicit controller switch, not a permanent second set of active
bodies.

Entering ragdoll performs one fixed-tick transaction:

1. freeze the last authoritative pose
2. disable capsule motion and solid contact response
3. activate a bounded set of limb bodies at the frozen bone transforms
4. connect them with ball, hinge, or limited six-axis constraints
5. transfer root linear and angular velocity to the ragdoll bodies
6. switch animation output to read-only or additive presentation
7. publish the ragdoll state and revision

The ragdoll rig description names bones, collider shapes, mass fractions,
joint limits, and self-collision groups with stable names. It contains no player
data. NPCs, bots, artificial characters, and player-owned characters use the
same path.

Recovery waits for a bounded pose-validity test. It finds a legal capsule pose,
projects the visual rig toward the recovery animation, transfers aggregate
ragdoll momentum to the capsule, disables limb simulation, and resumes the
character controller at a fixed-tick boundary. Failure to find room leaves the
character ragdolled rather than teleporting through geometry.

Player respawn, camera, possession, and input routing remain in `Players` and
client bindings. Physics reports the bodies and character state only.

## Triggers and contact events

Triggers use the same broadphase and exact narrowphase as solids but generate no
solver rows. They still participate in began, persisted, and ended tracking.

Expose two bounded event streams:

- contact events for solid pairs, including point, normal, relative velocity,
  normal impulse, tangent impulse, and material names
- overlap events for trigger pairs, including ordered endpoints and tick

Event order is pair id, event kind, then contact feature id. Destruction,
disablement, group edits, streaming removal, and teleport all synthesize the
required ended event exactly once.

High-frequency persisted events are opt-in per collider or listener. Began and
ended remain available by default. Event queues have configured limits and
report overflow. They never allocate without bound because a malicious client
created many overlapping triggers.

Callbacks run after physics publish. A callback may queue mutations for the next
barrier but cannot change the manifold currently being dispatched.

## Query expansion

Keep the caller-owned output convention and thread-safe immutable indexes.
Extend the query surface with:

- exact block, sphere, capsule, and convex hull casts that return first hit data
- overlap capsule and overlap convex queries
- raycast all with stable distance and entity ordering
- closest point and signed distance for supported shapes
- point material and surface lookup
- batched ray and shape queries with caller-owned result ranges

A `PhysicsQueryParams` value contains a collision-group name, resolved mask,
trigger policy, maximum results, and a bounded include or exclude set. The hot
query receives resolved dense data. Script adapters perform stable-name and
instance validation once before calling physics.

Every query reports truncation explicitly. A capped result cannot look like a
complete result. First-hit queries break equal-distance ties by entity id and
feature id. Querying through portals remains a higher-level composition over
single-world queries, with copied hits at world boundaries.

Queries during a physics step read the last synchronized index. No public query
may race structural mutation. Studio edit queries either synchronize first or
state that they read the last committed snapshot.

## Materials and contact rules

`Surface` continues to name one shared `SurfaceTable` entry. Per-part
`PhysicsProperties` remains the explicit override. Extend a surface definition
only with contact facts used by physics, such as static friction, dynamic
friction, restitution, rolling resistance, and optional density.

Every coefficient pair has an explicit combine mode: average, minimum, maximum,
or multiply. Pair resolution uses a deterministic precedence rule. A proposed
default is highest-priority authored mode, followed by the mode attached to the
lower stable material name when priorities tie. The exact rule must be frozen in
tests before content depends on it.

Contact modification is a bounded data rule, not an arbitrary callback inside
the solver. A `ContactRule` may match two stable material names or collision
groups and override friction, restitution, one-way response, or enabled state.
Rules compile into a sorted local table outside the hot loop.

One-way platforms use relative approach direction and a stable surface normal.
They must define behaviour for bodies that start overlapped, teleports, moving
platforms, and CCD. Conveyor surfaces add a target tangent velocity through the
ordinary friction row.

Material names, combine modes, and contact rules save and replicate. Resolved
coefficients and contact impulses do not.

## Destruction seam

Physics detects stress and reports facts. A gameplay destruction system decides
what breaks, what is replaced, and what damage means.

The seam consists of bounded reports for:

- constraint break force and torque
- contact impulse above an authored threshold
- sustained stress on an assembly or destructible region
- body speed and kinetic energy at impact

Reports contain stable source names, involved entities, tick, contact point,
normal, and measured values. They do not directly subtract health, split a mesh,
spawn debris, or destroy an instance.

Destructible content is authored as a stable graph of pieces and breakable
links, or as a recipe that a separate system can bake into that graph. Runtime
breaks activate an already bounded subset. Arbitrary live mesh fracture, which
has unbounded topology and network cost, is outside the initial system.

New debris receives a total body, collider, replication, and lifetime budget.
When a budget is exhausted, the gameplay system chooses a deterministic
fallback such as visual-only debris or no split. Physics never evicts unrelated
bodies to make room.

## Tick order

The fixed-step order becomes:

1. apply queued structural, body, group, and constraint mutations
2. resolve changed assembly mass and collision-filter data
3. gather character, vehicle, script, and kinematic intent
4. begin the physics clock step
5. integrate forces and kinematic targets
6. prepare rigid assemblies and general constraints
7. run CCD motion bounds and time-of-impact work
8. synchronize static and dynamic broadphase indexes
9. generate filtered candidate pairs and exact manifolds
10. build contact and joint islands
11. solve islands and join all jobs
12. publish body and rigid-child transforms and velocities
13. update sleeping and contact transition state
14. commit deferred breaks and destruction reports
15. dispatch bounded events after publish

Additional physics substeps repeat steps 5 through 13 within the same world
tick. Input and structural mutations remain frozen for the full world tick.

## Save, replication, and replay

### Saved facts

Save authored body kind and properties, collider shape and group name, surface
name and overrides, enabled constraints and their axis settings, break limits,
vehicle configuration, ragdoll rig descriptions, and destruction recipes.

Do not save broadphase proxies, manifolds, warm-start impulses, island ids,
sleep timers, dense collision-group bits, query scratch, or diagnostic counters.
Loading rebuilds all derived state and begins with a deterministic wake policy.

### Replicated facts

Replicate authoritative body transforms and velocities according to interest,
constraint and group edits, break decisions, active ragdoll state, and relevant
contact or destruction events. Stable names cross the wire. Entity references
use the replication layer's existing stable entity mapping.

The server remains authoritative by default. A client with `NetworkOwner` may
submit bounded state or intent only for the complete assembly granted to it.
The server checks ownership revision, tick window, finite values, speed and
acceleration limits, permitted contacts, and world interest before acceptance.

Ownership transfer is atomic. The old owner stops at one sequence, the server
publishes an authoritative handoff snapshot, and the new owner starts from that
revision. Constraint endpoints cannot be owned by different clients while in
one dynamic island. The server keeps the island or transfers it as one unit.

Prediction and reconciliation must not change server simulation order. Clients
may keep presentation history and replay accepted inputs. They may not replicate
solver caches or claim authoritative break and collision events.

Replay records fixed-tick inputs, structural physics mutations, ownership
transfers, and authoritative external impulses. A replay with the same build and
initial snapshot must reproduce body transforms, velocities, contact transitions,
breaks, and sleep decisions byte for byte where the existing determinism policy
requires it.

## Script surface

Expose simple operations through `PhysicsService` and existing instances:

- create, remove, and configure named collision groups
- set whether two groups collide
- raycast, shape cast, overlap, and closest-point queries
- apply force, torque, impulse, and impulse at position
- wake a body and query whether it sleeps
- set kinematic targets and network ownership through validated adapters
- inspect current contacts and constraint state

Constraint classes expose enabled state, attachments, limits, target, drive
settings, force caps, break thresholds, and read-only current force, torque, and
broken state. Vehicle and ragdoll controllers expose their neutral intent and
state rather than player-specific methods.

All property setters reject non-finite values and enforce documented ranges.
Query result caps, include and exclude sets, applied impulse, motor caps, and
per-tick operation counts are bounded. Expensive batch operations yield a clear
error before allocation when the request exceeds its budget.

Events use ordered engine signals with immutable payload values. A script cannot
retain a pointer into a transient manifold or solver array.

The generated Luau and TypeScript declarations remain the single binding
surface. Handwritten declarations that can drift from registered properties are
not added.

## Studio surface

Studio adds editing and diagnostics over the same ECS rows:

- named collision-group matrix editor under Physics settings
- constraint creation tools and attachment gizmos
- axis, limit, target, motor, and break-threshold handles
- collider shape, centre of mass, inertia, and assembly-root overlays
- sleeping, CCD, trigger, island, and network-owner visual modes
- live contact points, normals, impulses, and material-pair inspection
- vehicle suspension and wheel-contact diagnostics
- ragdoll limit preview and recovery-volume validation

Properties and Components edit the registered instance data directly. Gizmos
queue the same property mutations as numeric fields, participate in undo, and
never keep a private authoritative copy.

Live simulation diagnostics are read-only. Editing a running world's structure
uses a queued fixed-tick transaction and clearly marks the point at which it
takes effect. Expensive overlays update only when visible and use cached data
until the physics revision changes.

Studio validates missing attachments, cross-world endpoints, contradictory
rigid cycles, invalid limits, unbounded motors, unsupported CCD shapes, collision
group exhaustion, and ragdoll self-collision before play.

## Security and resource limits

Hosts configure hard limits for bodies, colliders, active constraints, contact
pairs, trigger events, query candidates, query results, CCD segments, destruction
pieces, and script physics operations per tick.

Server checks include:

- client authority over the complete body assembly
- monotonically increasing input and ownership revisions
- finite transforms, velocities, forces, and constraint properties
- maximum translation and rotation per accepted update
- maximum generated events and debris
- legal collision groups and query scopes
- permitted attachment ancestry and same-world endpoints

Rate limits reject excess work without partially applying a batch. Diagnostics
identify the caller, world, operation, requested count, and configured limit.
Logs must not include secrets or unbounded user-provided strings.

A malformed asset or save file cannot request an unbounded convex hull, compound
body, joint graph, or destruction graph. Import and load validate counts and
referential integrity before creating ECS rows.

## Migration

Migration keeps the engine runnable after every phase.

1. Add joint-row and diagnostic storage to `PhysicsWorld` without changing
   behaviour.
2. Solve generic locked constraints beside existing weld projection and compare
   their results in focused tests.
3. Route `Weld` and `WeldConstraint` through prepared rigid assemblies, then
   delete the redundant projection writes.
4. Add limited and driven axes, warm starting, break reports, and island wake.
5. Add the named collision-group registry while preserving current layer and
   mask behaviour as the resolved runtime form.
6. Move all public filters through the registry and delete any direct
   author-facing numeric layer path that has become redundant.
7. Add exact query results and CCD modes while keeping conservative sweep as the
   fallback.
8. Build vehicles and ragdolls only after constraints, islands, and ownership
   are stable.
9. Add destruction reports last. They consume physics facts and do not alter the
   solver design.

Temporary comparison code is test-only or compiled out of release. No shipped
world runs two solvers and chooses the answer that looks better.

Snapshot and wire version changes land with explicit migration readers where
the repository supports them. Pre-release formats that intentionally break are
versioned and rejected clearly rather than guessed.

## Delivery phases

### Phase 1: general constraints

- prepare attachment and assembly endpoints
- solve locked, free, and limited axes
- warm start joint rows
- expose forces, limits, and diagnostics
- fold welds into the common path

Acceptance requires stable hinge, rope, slider, ball socket, and weld scenes at
multiple supported physics rates.

### Phase 2: drives, breakage, and islands

- add motors and servos with finite caps
- add break force and torque reports
- build whole-island sleep and wake
- dispatch independent islands with measured thresholds
- publish island and joint counters

Acceptance requires deterministic output across worker counts and no one-tick
lag in wake or break state.

### Phase 3: collision policy and queries

- add stable named collision groups and their Studio matrix
- add constraint endpoint collision policy
- complete exact cast and overlap families
- expose bounded script filters and all-hit ordering
- add material combine and compiled contact rules

Acceptance requires save, wire, script, and Studio round trips for every authored
field.

### Phase 4: continuous collision

- expose discrete, swept, and continuous modes
- implement bounded convex time of impact
- accelerate mesh and heightfield candidate lookup where profiles require it
- add fallback and exhausted-budget counters

Acceptance requires projectile, rotating obstacle, thin-wall, moving-platform,
and portal-seam cases without tunnelling or unbounded work.

### Phase 5: vehicles and ragdolls

- build the neutral vehicle controller and physical-wheel reference vehicle
- validate whole-assembly authority transfer
- build ragdoll activation, self-collision groups, and recovery
- connect character state without adding player data to physics

Acceptance requires human, bot, and replay drivers to use the same intent and
produce the same authoritative results.

### Phase 6: destruction seam and tooling

- publish impact and stress reports
- add bounded breakable-content recipes in the owning gameplay system
- complete Studio overlays and validation
- add host limits, audit logs, and soak coverage

Acceptance requires hostile-content tests to remain within configured memory,
event, and simulation budgets.

## Focused test plan

### Components and persistence

- every new public component has its own suite or documented coverage owner
- padding bytes and default construction are deterministic
- stable names survive save and replication round trips
- transient solver state is absent from snapshots
- invalid entity references fail predictably on load

### Bodies and assemblies

- body-kind changes update indexes without a missing or duplicate tick
- compound mass, centre, and inertia match analytic fixtures
- rigid assemblies react at child contact points as one body
- contradictory cycles disable deterministically
- teleport clears contacts, CCD history, and sleep state
- an island sleeps and wakes as one unit

### Constraints

- locked axes preserve relative pose
- each linear and angular limit activates on the correct side
- motors and servos obey force and torque caps
- warm starting stays stable when step rate changes
- a break occurs once, after solve, at the same tick across worker counts
- destroyed or cross-world attachments cannot leave stale rows

### Collision and queries

- named group edits start and end contacts on the correct tick
- mutual masks, assembly filtering, direct exclusions, and trigger policy compose
  in the documented order
- every shape pair has symmetry and edge-contact coverage
- casts return stable nearest-hit and all-hit order
- capped overlaps report truncation
- query batches match individual queries bit for bit

### CCD

- fast spheres, capsules, boxes, and hulls do not cross thin static geometry
- dynamic targets and moving kinematics produce stable time-of-impact order
- multiple impacts respect the segment cap and report exhaustion
- unsupported shapes take the conservative fallback
- CCD contact events match discrete event semantics

### Vehicles and ragdolls

- suspension settles under known chassis mass
- steering, motor, and brake inputs are fixed-tick deterministic
- vehicle authority moves as one assembly
- ragdoll entry preserves pose and momentum
- ragdoll self-collision follows its named groups
- blocked recovery stays ragdolled
- successful recovery produces one capsule and no live limb bodies

### Events, destruction, and hostile input

- began, persisted, ended, and break events fire exactly once
- event order is unchanged by worker count
- destruction reports do not mutate gameplay state themselves
- malformed graphs, NaNs, huge query sets, and excessive operations are rejected
- queue overflow is visible and bounded

Prefer focused integration fixtures over repeated smoke suites. Determinism
fixtures run the same scene with different worker counts, supported physics
rates, and serialization boundaries.

## Profiling and release gates

All performance claims use the `release` preset. Each report names platform,
backend, physics rate, worker count, body count, active and sleeping split,
shape mix, contact count, joint count, and largest island.

Required benchmark scenes include:

- settled stacks with mostly sleeping bodies
- a dense awake contact pile
- many independent constrained mechanisms
- one large constrained chain
- high-speed projectile fields
- a vehicle pack on mixed materials
- simultaneous character ragdolls
- trigger and query stress without contacts
- bounded destruction activation

Record broadphase, narrowphase, contact setup, joint setup, island build, solve,
CCD, publish, event, and query time separately. Report allocations, live and
peak bytes, contact and joint rows, sleeping bodies, island spread, CCD segments,
fallbacks, and exhausted budgets.

Parallel work reports producer duration through the existing profiling path
after joining. It must not pretend overlapping worker spans were serial owner
time. A speedup claim includes parity output and the crossover where dispatch
starts paying.

Release gates:

1. architecture and component registration checks pass
2. focused unit and integration suites pass
3. determinism and replay fixtures match across worker counts
4. save and replication compatibility tests pass
5. sanitizer runs report no lifetime, race, or bounds failures
6. release benchmarks show no unexplained regression in existing scenes
7. Studio diagnostics add no cost while hidden
8. old weld, filter, query, or controller paths are removed when replaced

Live Studio inspection is valuable for constraint gizmos, vehicle diagnostics,
and ragdoll recovery. Ask the user before running it at final verification. If
approval is not given, record that specific visual check as skipped while still
running all headless checks.

## Non-goals

The first complete expansion does not include:

- fluids, aerodynamics, or deformable bodies
- cloth simulation
- arbitrary runtime mesh fracture
- a second third-party rigid-body engine
- nondeterministic GPU rigid-body simulation
- cross-world joint solving
- fully coupled train or vehicle simulation across streamed world boundaries
- per-contact script callbacks inside the solver
- unbounded user-defined collision filters
- a separate integrator for vehicles, ragdolls, or characters
- speculative broadphase or solver optimizations without release measurements

These may receive later plans when a real game requires them. The public seams
above preserve room for force-producing controllers, copied cross-world events,
and bounded baked destruction without committing the core solver to those costs.

## Definition of complete

The physics expansion is complete when an author can build a constrained,
motorized, breakable vehicle; drive it with a player, bot, or replay; collide it
reliably at supported speeds; query and filter it by stable names; transfer
authority safely; ragdoll and recover its occupants; inspect every relevant fact
in Studio; save and replicate the scene; and replay the same fixed-tick outcome
without a parallel physics path or unbounded work.
