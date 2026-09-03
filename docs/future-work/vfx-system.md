# Visual effects system plan

## Status

This document defines the next production stages of the visual effects system.
It is not a claim that every item below exists today.

The engine already has a substantial effects foundation. Particle emitters,
beams, trails, decals, tiled face textures, deterministic particle spawning,
CPU and device-owned particle stepping, bounded pools, portal carry, asset
demand, script classes, save support, and Studio visibility controls already
exist. New work must extend those paths instead of placing a second effects
runtime beside them.

The first new production feature is a compiled multi-output visual effect that
can drive the existing primitive types. Mesh particles, projected decals,
bounded visual collision, fields, authoring graphs, and richer Studio tools are
then added as capabilities of the same runtime.

## Product goal

An author can build one deterministic visual effect, preview it in Studio,
publish it as an asset, trigger it from either script runtime, and see the same
authored event on every relevant client. Each client may reduce visual detail
to fit its device budget, but gameplay timing and outcomes never depend on
which visual particles it chose to simulate or draw.

The complete system must support:

- billboard and mesh particles;
- beams, trails, ribbons, face decals, projected decals, and flipbooks;
- deterministic bursts, continuous rates, distance emission, and child
  emission;
- colour, size, transparency, rotation, material, and motion curves;
- bounded fields, forces, collision responses, and visual collision events;
- CPU reference stepping and GPU-owned high-volume stepping;
- explicit quality, distance, visibility, and per-view budgets;
- portals, mirrors, surface cameras, multiple Studio viewports, and future VR
  views without advancing an effect more than once;
- graph authoring that compiles to a small effects program;
- Luau and JavaScript parity;
- compact replicated trigger events with stable effect names and seeds;
- save formats that contain authored state but not transient particle pools;
- useful diagnostics, captures, progress, cancellation, and profiling;
- strict limits for hostile assets, scripts, and network messages.

## Non-negotiable rules

### Visual particles never decide gameplay

No damage, hit, movement, status effect, loot result, or authoritative state may
depend on a visual particle, visual collision callback, render depth, camera,
quality level, or GPU result.

Gameplay systems compute their own authoritative result. They may publish a
compact visual effect event after the result is known. A bullet impact can emit
sparks at the confirmed hit, but a spark cannot report a hit that damages an
entity.

This rule permits aggressive visual culling and client-specific quality without
creating a different game on two machines.

### A particle is not an entity

The existing pool remains the storage model. An emitter, effect player, field,
beam, trail, or decal may be an ECS entity. Its individual particles, ribbon
segments, and transient projected decals are packed entries in bounded pools.

Creating and destroying an ECS row for every spark would turn the hottest
lifetime path into archetype churn and make the half-million-particle target
unreachable.

### One simulation advance per world tick

An effect advances from world time, never from the number of cameras that draw
it. Portals, mirrors, split screen, Studio viewports, and capture views consume
one completed simulation state.

View-dependent culling, sorting, ribbon facing, and projected sampling may run
per view. Spawn counts, particle ages, fields, trails, and effect clocks may
not.

### Randomness is explicit and stable

Every random value is derived from a stable seed tuple:

```text
(effect seed, emitter stable id, spawn ordinal, module purpose)
```

The tuple is hashed without mutable random state. Pool slots, addresses, worker
order, draw order, and camera count never enter it. A new random-consuming
module receives a new stable purpose name so it cannot shift the values used by
existing modules.

### Authored facts and derived state stay separate

Scenes and assets save authored properties, graph documents, effect references,
parameter overrides, and explicit playback state. They do not save live
particle arrays, trail history, sorted draw lists, GPU buffers, query results,
or Studio preview caches.

Every derived object carries the source signature and revision that produced
it. A changed input invalidates only the dependent artifact.

### Existing graphs keep their jobs

The render graph schedules render resources and passes. The animation system
evaluates skeletal motion. The Studio nodegraph library supplies an editor
widget and asynchronous document evaluator.

The visual effects system does not replace any of them. Its authoring graph
compiles to effect data. Rendering that data uses ordinary render nodes. An
effect may read an attachment or bone transform already produced by animation,
but it does not evaluate animation clips.

## Current foundation

### Effects module

`mono.engine/effects` is a shared L8 module. It currently owns:

- `ParticleEmitter` authored properties;
- deterministic spawn sampling;
- packed `ParticleInstance` and `ParticleState` rows;
- emitter blocks and reusable pool slots;
- particle curve tables;
- CPU reference simulation;
- device-owned simulation state and revision handoff;
- `Beam`, `Trail`, `Decal`, and tiled `Texture` components;
- shared ribbon geometry generation;
- authored-field serializers and runtime-state reset readers;
- the script-visible class tree for these instances.

Its invariants remain authoritative:

- per-frame stepping reads compact emitter blocks, not the large authored row;
- particles remain inside their emitter block;
- block retirement and reuse are bounded;
- trail history samples during simulation, not presentation;
- renderer and asset types do not enter the shared module;
- stable names cross asset and world boundaries.

The module policy still describes an earlier CPU-only state while the current
source includes a device-owned compute path. The first implementation change
under this plan must reconcile that documentation with measured current code.
The source, tests, captures, and release profile decide which behavior is kept.

### Existing particle surface

The current `ParticleEmitter` already supports:

- size, transparency, squash, and colour sequences;
- lifetime, speed, rotation, spin, and flipbook rate ranges;
- box, sphere, cylinder, and disc shapes;
- surface or volume emission and inward or outward direction;
- time rate and distance rate;
- acceleration, drag, speed caps, radial force, tangential force, and noise;
- velocity inheritance and locked-to-parent motion;
- several camera and velocity orientation modes;
- texture flipbooks, additive blending, brightness, and time scale;
- explicit maximum particles;
- script `Emit(count)` and clear support;
- deterministic seeds based on emitter and spawn ordinal.

New work should not hide these properties behind a graph-only API. A simple
standalone emitter remains a first-class and useful path.

### Ribbons and face effects

Beams and trails already share one ribbon stream and renderer path. Beams use
two attachments and a fixed segmented curve. Trails record a fixed-capacity
history at simulation rate. Face decals and tiled textures also use the ribbon
stream because each is a small dynamic quad rather than a resident mesh.

The shared builder remains. New ribbon and projected-decal features extend its
packed output or add a purpose-built packed stream. They do not create a mesh
asset every frame.

### Renderer

The renderer already has:

- blended and additive particle pipelines;
- blended and additive ribbon pipelines;
- world-owned particle device state;
- device compute pipelines for emission, stepping, and scatter;
- emitter and material grouping;
- world and emitter visibility bounds;
- per-view frustum culling;
- changed-revision preparation and resident data reuse;
- portal seam data for particle carry;
- batched rendering of multiple views and worlds;
- GPU memory and frame counters.

Effects remain nodes in the existing render graph. A new effect primitive adds
the smallest required node or extends an existing geometry node. There is no
private effects pass scheduler.

### Asset and material paths

Effect image names already participate in content demand. The texture catalogue
provides flipbook facts and a revision so steady emitters can skip repeated
lookups. Studio can pick texture assets for emitter, beam, trail, decal, and
tiled texture properties.

The asset system already supports content-addressed textures, meshes, materials,
animations, remote sources, on-demand fetch, and renderer residency. New effect
assets use that publication and demand path. They do not introduce an effects
filesystem or direct URL loader.

### Script and Studio paths

The classes are visible through `Instance.new`, reflected properties, generated
bindings, and the generic Properties and Components panels. `ParticleEmitter`
has a checked `Emit(count)` method. Studio can hide particle emitters, prevents
workspace-less preview emitters from running, and presents effects through the
same world and renderer path as a game.

These are the base for richer APIs and preview controls. A separate Studio-only
simulation would drift from runtime behavior and is forbidden.

## Ownership and layer boundaries

No new engine module is required for the core runtime. `mono.engine/effects`
already owns the domain and remains the shared source of effect behavior.

Ownership is divided as follows:

- `effects` owns authored effect values, compiled programs, deterministic spawn
  descriptions, CPU reference stepping, packed presentation streams,
  serializers, and class registration.
- `scene` owns transforms, attachments, hierarchy, world time, cameras, portal
  descriptions, and shared texture facts.
- host integration reads ECS revisions, prepares immutable effect inputs, runs
  world systems, and publishes copied presentation data.
- `render` owns the device simulation backend, GPU pools, material variants,
  per-view culling and sorting, render nodes, and device retirement.
- `assets` owns effect, image, mesh, and material file formats, validation,
  content addresses, and publication.
- `delivery` and products fetch named dependencies and report progress or
  failure.
- scripting adapters own Luau and JavaScript methods, events, tickets,
  validation, and generated declarations.
- Studio owns the graph editor, inspectors, timeline, preview controls,
  diagnostics, and asset publication.
- replication owns compact reliable and unreliable effect messages after the
  replica registers the required classes.

`effects` must not link `render` or `assets`. It stores stable names and packed
plain data. The client adapter is where effect output and resolved content meet
the renderer.

Particle collision must not create an undeclared sideways edge from `effects`
to `physics`. The integration section below defines a copied collision-query
snapshot supplied by a higher layer. If that proves too expensive, collision
stays client-render-only until an architecture change is reviewed and enforced.

## Vocabulary

The following names keep four different things distinct:

- **effect definition**: immutable authored content published as an asset;
- **effect program**: validated packed data compiled from one definition;
- **effect player**: an ECS instance that plays one named definition;
- **effect event**: a compact request to play an effect at a tick and transform;
- **emitter**: one output stream inside a program or one standalone
  `ParticleEmitter` instance;
- **particle state**: a transient pooled simulation row;
- **presentation stream**: packed particles, ribbons, meshes, or projected
  decals consumed by the renderer;
- **preview**: an ordinary effect player in a Studio-owned preview world.

An effect definition is not a world snapshot. An effect player is not an asset.
An effect event is not a replicated particle list.

## Effect definition and compiled program

### Asset identity

The published asset kind is provisionally `VisualEffect`. Its external
identifier is a stable text name resolved through the existing content source
order. Its bytes are content-addressed like every other asset.

The file contains:

- format magic and version;
- canonical graph document or canonical compiled section;
- stable node and output names;
- parameter schema and defaults;
- dependency table for textures, meshes, materials, and child effects;
- precomputed bounds where they are finite;
- compiler feature requirements;
- optional editor metadata in a separate ignored section;
- content checksum.

Runtime loading never trusts precomputed offsets or counts. It validates the
document, recompiles or validates the packed program, and verifies every range
before allocating.

### Program shape

The compiled program is data, not a general bytecode VM. It contains bounded
arrays grouped by the work they drive:

- spawn descriptions;
- curve tables;
- particle integration modules;
- output descriptions;
- renderer material keys;
- field references;
- child-event descriptions;
- parameter bindings;
- static bounds and dependency names.

The compiler topologically orders the authored graph and folds constants. It
rejects cycles except through explicit bounded delay or feedback nodes that have
a defined state size. The first release has no feedback node.

Each output becomes one direct execution plan. A particle output should reach
the current emitter-block path with sampled settings. A beam output should reach
the current ribbon builder. Compilation must not leave a graph traversal inside
the per-particle loop.

### Stable ids

Node, output, parameter, marker, and module ids are stable text in the authored
document. The compiler may intern them to dense process-local integers after
load. Serialized ordinals and pointer-derived ids are forbidden.

Deleting and recreating a node gives it a new stable id. Duplicating a node
gives the copy a new id. Renaming a display label does not change identity.

### Parameters

Parameters have a declared type, stable name, default value, optional numeric
range, and usage flags. Initial types are:

- boolean;
- integer;
- number;
- `Vector2`;
- `Vector3`;
- `Color3`;
- transform;
- stable asset name.

The runtime accepts a bounded override set. Unknown names and wrong types are
diagnostics, not implicit conversions. Overrides are sampled into packed player
state only when their revision changes.

For an instance-authored `VisualEffect`, overrides are represented by reflected
child parameter instances or one engine-owned typed parameter table. The final
choice must preserve generic Studio editing, save support, and script parity
without walking children each frame.

## Effect players and events

### `VisualEffect` instance

Add a script-visible `VisualEffect` instance for persistent or controllable
effects. Its minimal surface is:

| Property | Meaning |
|---|---|
| `Effect` | Stable visual-effect asset name |
| `Enabled` | Whether looping outputs may emit |
| `Seed` | Explicit base seed, or zero for a stable seed assigned at play time |
| `TimeScale` | Bounded visual clock multiplier |
| `Looped` | Whether the program restarts after its authored duration |
| `PlaybackPosition` | Read-only local effect time |
| `Playing` | Read-only playback state |
| `Priority` | Budget priority within a bounded enum |
| `Quality` | Optional per-player ceiling, never an expansion past device policy |

Methods are `Play(seed?)`, `Stop(clear?)`, `Restart(seed?)`, and
`Emit(outputName, count, seed?)`. Methods validate finite values and bounded
counts before touching a queue.

`Stop(false)` stops new emission and lets existing visual state retire.
`Stop(true)` clears that player's transient state. Destroying the player is
equivalent to `Stop(false)` unless the caller explicitly clears it.

### One-shot service API

Most impacts should not create an instance. A `VisualEffects` service offers a
bounded one-shot request:

```luau
VisualEffects:Play("effects/metal-impact.avfx", {
    CFrame = hitFrame,
    Seed = eventSeed,
    Parameters = {
        Intensity = 0.8,
        SurfaceColor = hitColor,
    },
})
```

The service validates the asset name, transform, parameter count, payload size,
and per-caller rate. It appends a copied event to a world-owned bounded queue.
The queue is consumed at the simulation barrier.

The returned handle is local and optional. It can stop or update a persistent
local event but never crosses a world or network boundary. Network messages use
the stable event key assigned by replication.

### Lifecycle

An effect player moves through explicit states:

```text
unresolved -> loading -> ready -> playing -> draining -> retired
                  |          |                     |
                  +-> failed +---------------------+
```

Asset loading never blocks a tick. A request may choose one of three declared
late-load policies:

- skip if its start deadline is missed;
- start late from time zero;
- catch up by simulating bounded fixed steps before first presentation.

The default for one-shot combat effects is skip after a short deadline. The
default for persistent environment effects is catch up within a bounded step
count, then start at the correct age with emission before the catch-up window
dropped.

## Deterministic spawning

### Canonical spawn description

All emitter types compile into a canonical spawn description. It includes:

- stable emitter id;
- shape and shape parameters;
- local transform source;
- time rate and distance rate;
- burst times and counts;
- lifetime and initial-motion ranges;
- parent velocity inheritance;
- curve-table indices;
- material and output kind;
- maximum live entries;
- base seed and spawn ordinal;
- child-event policy;
- finite local bounds or an explicit dynamic-bounds flag.

The same description feeds CPU and GPU backends. Backend-specific packed tables
may differ, but they are generated from this one value and carry its signature.

### Spawn ordinal

Each logical emitter has a monotonically increasing 64-bit spawn ordinal. It is
not reset when pool slots recycle. Restarting an effect deliberately resets it
because the base seed and local effect clock also restart.

If a backend can represent fewer ordinals internally, compilation or dispatch
must segment the range without changing the tuple used for randomness.

### Fixed time and catch-up

Continuous rate accumulation uses the world's fixed simulation delta. Bursts
are located on the effect timeline and fire once when the clock crosses them.
The interval rule is explicit so seeks and catch-up cannot double-fire:

```text
previousTime < burstTime <= currentTime
```

Loop wrap splits the interval at the duration boundary. Reverse playback is not
part of the first runtime. Studio may scrub by rebuilding preview state from the
nearest cached checkpoint.

### Child emission

Death, age, and visual collision may spawn another output or child effect. Child
spawns are bounded by:

- maximum generation depth;
- maximum children per source entry;
- maximum total child events per player per tick;
- cycle validation in the dependency graph;
- the same stable seed derivation as every other spawn.

When a limit is reached, events are dropped and counted. They never wrap, retry
forever, or allocate an unbounded queue.

Age and death child spawns can remain inside a device-owned effect program. They
do not require a per-particle readback. A device-local child is visual state on
that client and may be reduced by quality policy. Any reliable cross-client
marker is scheduled from the effect timeline on the world owner, not discovered
later by reading a GPU particle.

## Particle outputs

### Billboard particles

The current `ParticleEmitter` behavior remains the default output. Compiled
effects reuse its compact curves, shape sampling, integration, flipbooks,
blending, and orientation modes.

New billboard features should be added only when they are consumed by both the
reference path and the renderer. Candidate additions are:

- soft-particle depth fade;
- lit material variant;
- camera-distance size clamp;
- alpha-cutout mode;
- per-output shadow receive policy;
- normal-map support where a material supplies one.

Stored properties with no consumer are not considered implemented. The current
light-emission and light-influence fields need either a real lit consumer or an
explicit compatibility status in the inspector.

### Mesh particles

A mesh-particle output references one stable mesh name and one material name per
output. Individual particles carry only transform, colour, age, seed, and output
slot. They do not carry asset strings.

The renderer groups visible entries by mesh, material variant, blend mode, and
view policy. It draws instanced resident meshes. A missing mesh uses the standard
visible fallback mesh; a missing material uses the standard fallback material.

The first release supports rigid mesh particles only. It does not skin a mesh,
run an animation track per particle, or create one material instance per
particle. Flipbook animation remains a texture concern.

Mesh output caps are lower than billboard caps because each entry has more
geometry and transform data. The budget system accounts for submitted triangles,
not only live particle count.

### Ribbons

Keep `Beam` and `Trail` on the shared ribbon builder. Extend it with an authored
`RibbonEmitter` only after the two existing types cannot express a required
effect.

Useful additions include:

- variable segment count with a strict ceiling;
- width and colour curves sampled along length;
- texture modes for stretch, tile, and per-segment repeat;
- optional local normal for non-camera-facing strips;
- break markers that prevent joining across teleports;
- a packed general ribbon output for procedural paths.

Trail samples stay tied to simulation ticks. A renderer may simplify or skip
segments for a distant view, but it does not change the recorded history.

### Beams

Beam endpoints remain attachment references within one world. A missing or
unresolved endpoint produces no geometry and a diagnostic counter. It does not
fall back to a stale transform.

The fixed ten-segment path remains until a release profile shows a need for
adaptive tessellation. If adaptive tessellation is added, the error metric is
view-independent for simulation and bounded per view for drawing. The beam's
texture phase must not change when segment count changes.

### Decals

The existing `Decal` and tiled `Texture` remain face-bound authored instances.
They are ideal for signs, labels, and surfaces that share a part face.

Add a separate transient `ProjectedDecal` output for impacts and effect graphs.
It contains:

- world transform and half extent;
- material or image name;
- colour and transparency;
- normal fade and depth bias;
- lifetime and fade curve;
- projection layer and receiver mask;
- stable spawn seed.

Projected decals are pooled instances rendered through a bounded projection
volume. They do not alter mesh UVs, write into source textures, or become
permanent scene geometry.

The first release clips against the scene depth and normal buffers where the
active render pipeline exposes them. A pipeline without those capabilities
skips projected decals with a visible capability diagnostic. It does not build a
private depth pass.

## Fields and forces

### Field types

Effect-local fields provide visual motion without coupling particles to gameplay
physics. Initial field types are:

- uniform vector;
- radial attraction or repulsion;
- vortex around a declared axis;
- directional drag;
- bounded procedural noise;
- kill volume.

Each field has a stable id, transform, shape, falloff, strength, priority, and
finite bounds. Supported shapes initially match emitter primitives: box, sphere,
cylinder, and disc where meaningful.

### Field lookup

The compiler binds effect-local fields directly to outputs. World fields, if
added later, are collected into a bounded spatial grid once per simulation tick.
Particle work queries only cells overlapping an emitter's conservative bounds.

There is no all-fields times all-particles loop. An output declares a maximum
number of fields it can consume. Excess fields are ranked by priority and stable
distance, then dropped with a counter.

### GPU representation

Fields compile to a small packed table shared by CPU and GPU preparation. Noise
uses the same explicit seed and mathematical definition on both paths. Exact
floating-point equality across devices is not required for decoration, but the
same inputs must produce bounded, visually equivalent motion and identical spawn
counts.

## Visual collision

### Scope

Visual collision is optional and off by default. Supported responses are:

- kill;
- bounce;
- slide;
- stick and become a projected decal;
- emit a bounded child visual event.

Collision results are local decoration. They cannot call arbitrary gameplay
scripts or mutate authoritative entities.

### Query inputs

The effects module consumes an immutable `EffectCollisionScene` value assembled
by host integration. It contains only the bounded shapes or acceleration data
needed for the enabled effects. It contains no physics-world pointer and no
callback into the physics module.

The first CPU implementation supports simple static primitives and terrain
collision proxies. Dynamic rigid bodies and deforming meshes wait until a
measured use case justifies the copy and update cost.

The device path may support depth-buffer or signed-distance collision as a
quality feature. Such results are camera and pipeline dependent, so they remain
visual-only and never produce replicated events.

### Event delivery

Collision notifications use a bounded aggregate stream. One notification can
identify the effect player, output stable name, position, normal, and spawn seed.
It does not expose a particle pointer or pool index.

Scripts may subscribe only to explicitly enabled local visual notifications.
Delivery is capped per effect and per world per tick. Overflow increments one
counter and drops the remainder. A script that needs reliable hits must use the
gameplay collision system instead.

## CPU and GPU ownership

### Canonical behavior

The CPU step is the executable reference for deterministic spawn counts,
lifetime, integration order, curves, and event timing. Headless tests exercise
it without a graphics device.

The GPU path owns high-volume visual state on clients. It receives changed
spawn descriptions, fields, curve tables, explicit events, and time increments.
It does not upload and download every particle each frame.

### Backend selection

Backend selection is per world or compatible output family, not per particle.
It considers:

- device compute support;
- required modules;
- collision mode;
- readback requirements;
- debug capture mode;
- pool size and measured crossover;
- active render-pipeline capabilities.

An unsupported GPU module falls back to CPU for the whole compatible output
group. Splitting one emitter between CPU and GPU would require two pool owners
and is forbidden.

### Transfer contract

The host-to-device transfer contains only dirty program tables, dirty player
state, queued spawn events, and timing. Presentation revisions distinguish:

- program layout changes;
- resident parameter changes;
- simulation advances;
- view-only ordering changes.

A quiet looping effect advances on the device without a full emitter-table walk
or full table upload. A paused unchanged world submits no effect simulation work.

### Device loss

Device state is derived and disposable. On device loss or renderer recreation:

1. retire all effect GPU allocations;
2. preserve authored players and their logical clocks;
3. rebuild programs and pool claims from world state;
4. apply the declared late-load policy;
5. resume without reading dead buffers.

An exact reconstruction of every old spark is not required. The restart seed and
logical time remain stable, and persistent effects catch up within their bounded
policy.

## Pools, residency, and memory

### Pool families

Use separate bounded pools where entry layouts and retirement rules materially
differ:

- billboard particles;
- mesh particles;
- projected decals;
- ribbon vertices and runs;
- collision notifications;
- effect players and pending events.

Do not create one universal maximum-sized effect entry. Paying mesh-transform
bytes for every billboard would waste the common path to simplify a rare one.

### Claims and generations

Each player output claims a contiguous block or a bounded set of pages. Pool
references use indices plus generations. Reusing a block invalidates old state
without clearing unrelated memory.

Claims are sized from authored maxima and device quality. An unbounded computed
maximum is a compile error. A derived maximum is clamped to the player and world
ceilings before allocation.

### Growth and shrink

Pools grow geometrically up to explicit byte and entry ceilings. Growth occurs at
a frame-safe barrier. Failure refuses the lowest-priority claim and records why.

Pools do not shrink every time effects retire. A high-water policy releases large
unused pages only after a quiet interval or world unload. Device resources use the
renderer retirement queue so in-flight command buffers never observe freed data.

### Residency accounting

Diagnostics report independently:

- live and peak bytes by pool family;
- committed capacity and used entries;
- live players and outputs;
- claims refused;
- spawns dropped;
- child events dropped;
- collision events dropped;
- buffer growth and retirement counts;
- host-to-device bytes;
- visible, culled, and drawn entries per view;
- submitted triangles for mesh particles and ribbons.

Cumulative allocation is not presented as live memory. Logical payload is not
presented as driver heap commitment.

## Render integration

### Existing graph only

Effects declare requirements to the render graph:

- particle simulation compute, when device-owned;
- transparent or additive geometry;
- optional depth and normal reads for soft particles and projected decals;
- optional shadow or lighting inputs for lit variants;
- output target and view data.

The graph decides order, resource lifetime, capability fallback, and cache reuse.
The effects system never records a hidden sequence of render passes beside it.

### Material key

Each output compiles a compact material key from:

- primitive kind;
- blend mode;
- lighting mode;
- texture or material name;
- flipbook layout;
- depth-write and depth-test policy;
- soft-particle mode;
- shadow policy;
- user shader variant, if allowed.

The renderer sorts groups by this key and resolves names through existing
resident asset tables. No material lookup occurs per particle.

### Transparency

The first path keeps the current split:

- additive effects avoid depth sorting;
- ordinary blended groups sort by stable group and view depth rules;
- opaque or cutout mesh particles use the appropriate geometry path.

Per-particle full sorting is enabled only where it is already measured and
bounded. Weighted blended transparency or another order-independent method is a
render-pipeline feature, not a private effects feature.

### Lighting and shadows

Unlit remains the fast default. Lit particles and mesh particles consume the
active pipeline's existing light data. They do not build their own light list.

Shadow casting is off by default and permitted only for mesh or cutout outputs
with an explicit authored flag and a separate shadow budget. Billboard smoke
should not silently multiply shadow work.

### Caching

Cache at the narrowest useful layer:

- compiled effect program by content hash and compiler version;
- dependency resolution by program signature and content-catalogue revision;
- sampled parameter block by player and override revision;
- GPU program tables by program signature and device generation;
- emitter ordering by layout revision;
- view visibility by world presentation revision, view signature, and portal
  carry signature;
- Studio graph preview by document hash, parameters, seed, and preview time.

Simulation time never enters a cache that is meant to settle. Dynamic particle
buffers are state, not cache entries.

## Limits, LOD, and quality

### Budget hierarchy

Limits are enforced in this order:

1. hard safety ceilings compiled into the runtime;
2. server or universe policy ceilings;
3. client device quality budget;
4. world budget;
5. per-effect authored maximum;
6. per-view visibility budget.

A lower layer may reduce work. It cannot exceed a ceiling from a higher layer.

### Cost model

Particle count alone is not enough. Each output estimates cost from:

- simulated entries;
- bytes of state;
- spawn and module work;
- sampled fields;
- collision queries;
- submitted vertices and triangles;
- overdraw estimate;
- texture and material residency;
- number of views in which it is visible.

The estimate is calibrated from release profiles. It is not used to claim exact
GPU time. It ranks work under a known budget.

### Deterministic admission

When an event exceeds a simulation pool, admission uses stable priority and
stable event order. It never depends on unordered-map iteration or worker finish
order.

Client quality may reduce visual output through declared scalable controls:

- emission multiplier;
- maximum live entries;
- ribbon segment count;
- collision disable;
- field sample count;
- shadow disable;
- mesh-to-billboard fallback;
- maximum draw distance;
- update divisor for non-event visual integration.

Spawn markers and replicated effect starts remain ordered even if their visual
children are dropped.

### Distance and visibility

Each output supplies finite conservative bounds or declares why bounds are
dynamic. An output with unbounded speed, lifetime, or field motion is clamped or
treated as always visible within its effect distance.

Offscreen effects may:

- stop drawing while continuing a cheap logical clock;
- skip visual integration and analytically age out entries;
- stop emission after a policy delay;
- restart or catch up when visible.

The selected policy is authored per output and cannot suppress reliable marker
or child-event delivery if those events are used to drive other visual outputs.

## Portals and multiple views

### Simulation space

Particle state stays in the world and space where it was emitted. Portal carry
is a presentation transform. A particle crossing a visible seam is transformed
for that view without overwriting its canonical simulation position.

Physical teleport of an emitter or gameplay object is handled by world movement
systems. The effect observes the resulting transform and the trail builder inserts
a break marker when the displacement exceeds its authored continuity threshold.

### View families

All views of one completed world state share:

- effect clocks;
- spawn results;
- pool state;
- curve and field tables;
- program residency;
- content residency.

They own separately:

- frustum and portal visibility;
- view-facing orientation;
- transparent ordering;
- soft-particle depth sampling;
- projected-decal depth and normal sampling;
- per-view draw budgets;
- output target.

### Portal recursion

Portal recursion obeys the renderer's existing surface limit and view budget.
An effect visible through several recursive mouths is simulated once and may be
drawn several times. Each draw is charged to the per-view cost budget.

Portal seam data is flattened once per world presentation revision and reused by
all compatible effect outputs. Particle, ribbon, mesh-particle, and projected
decal paths must not each derive a different seam transform.

### Cross-world portals

A cross-world portal reads the destination world's copied presentation stream.
No effect pool, ECS handle, or pointer crosses worlds. The host resolves the
stable world name and hands the renderer owned or lifetime-safe presentation
data, matching the existing portal model.

## Assets, materials, and shaders

### Dependencies

The effect compiler emits a unique sorted dependency list. Dependencies include:

- texture and flipbook assets;
- mesh assets;
- material assets and their texture maps;
- child effect assets;
- optional user shader names.

The content client requests dependencies through the existing demand and source
priority path. Repeated names across outputs produce one request.

### Partial readiness

An effect definition may become ready before all visual dependencies. Each
output follows one declared fallback:

- visible built-in fallback;
- skip output until ready;
- skip the whole effect.

Persistent effects retry when the content-catalogue revision changes. One-shot
effects obey their late-load policy and do not appear seconds after an impact
unless explicitly authored to do so.

### Materials

Material instances are immutable resolved descriptions plus a bounded parameter
block. Effect outputs may override only parameters declared overrideable by the
material. They cannot clone an unrestricted material object per particle.

Mesh particles use ordinary material assets. Billboards, ribbons, and decals use
an effect-surface material family that maps cleanly to the existing fast
pipelines. Shared texture, sampler, and shader residency remains owned by the
renderer.

### Shader scripts

An effect may name a `ShaderScript` only through the render graph's existing
shader-library and capability path. Compile failure produces a diagnostic and a
fallback or skipped output. It is not fatal to the world.

Effect shaders receive a fixed reflected input contract. They cannot request
arbitrary storage buffers, sample unrestricted render targets, or write world
state. Instruction, texture, storage, and output limits use the renderer's
existing shader capability checks.

## Procedural authoring graph

### Purpose

The graph describes how an effect is spawned and visually evolves. It is not a
general gameplay language. Initial node families are:

- parameters and constants;
- time, normalized age, and deterministic random values;
- spawn rate, burst, distance, and event nodes;
- shapes and transform sampling;
- motion, drag, noise, and fields;
- scalar, vector, colour, and curve operations;
- particle, mesh-particle, beam, trail, ribbon, and decal outputs;
- child visual events;
- quality switches with declared fallback branches.

There are no arbitrary script callback nodes in the runtime graph.

### Editor and runtime split

Studio uses its existing nodegraph library as the editor canvas and asynchronous
preview evaluator. The engine effect graph document and compiler do not depend on
Dear ImGui or `mono.studio/nodegraph`.

The runtime receives compiled effect data. It does not ship the editor evaluator
or call `std::any` payloads in a particle loop.

### Types and validation

Ports are typed. Connections require an exact type or a declared conversion.
Unknown node types remain visible in Studio with diagnostics but cannot compile.

Validation checks:

- unique stable ids;
- no illegal cycles;
- all required inputs connected or defaulted;
- finite constants and ranges;
- curve key order and count;
- maximum outputs and modules;
- bounded spawn and child-event counts;
- dependency cycles;
- compatible material and output kinds;
- available platform capabilities or declared fallbacks;
- finite or conservatively declared bounds.

### Procedural determinism

Random nodes require a stable purpose name generated from their stable node id.
Noise nodes state their coordinate space and seed. Time comes only from effect
time, normalized age, or a declared world-time input.

The compiler rejects wall-clock time, address-derived values, unseeded random
sources, and iteration over unordered collections.

### Baking

Studio can bake a procedural effect into the canonical asset. Baking performs:

1. graph validation;
2. constant folding and dead-node removal;
3. curve resampling under an explicit error tolerance;
4. static bound analysis;
5. dependency extraction;
6. packed program generation;
7. CPU preview verification for selected seeds;
8. canonical serialization and content hashing;
9. publication through the normal asset pipeline.

The bake report lists warnings, dependency sizes, program bytes, estimated pool
cost, and capability fallbacks. Cancellation leaves no partially published
manifest entry.

## Scripting API

### Parity

Luau and JavaScript expose the same classes, properties, methods, errors, and
events. Generated declarations are updated in the same change as class
registration.

The core surface includes:

- create and configure standalone `ParticleEmitter`, `Beam`, `Trail`, `Decal`,
  and `Texture` instances;
- create and control `VisualEffect` instances;
- play bounded one-shot effects through `VisualEffects`;
- preload a definition and its declared dependencies;
- read compile, load, and budget diagnostics;
- subscribe to explicitly enabled local visual markers and collision summaries;
- construct graph documents or effect-definition buffers through a validated
  builder API;
- bake authored definitions to immutable bytes;
- import or export canonical effect bytes where script permissions allow it.

### Script-built effects

Procedural scripts build an `EffectDefinitionBuffer`, analogous in purpose to
the animation buffer. It owns canonical authoring data, not live particles.

Mutations occur through checked bulk methods or typed builder calls. `Commit()`
validates the complete document, computes its signature, increments one revision,
and leaves the previous valid program active on failure.

A `VisualEffect` may reference either a published asset name or an in-world
buffer. The reference is explicit and serializable. The live effect samples the
new revision at a simulation barrier and applies a declared restart or migrate
policy.

### Asynchronous work

Compile, preload, and bake calls return the repository's normal ticket or
promise shape. They do not block the world thread while reading, decoding, or
compiling assets.

Worker results contain owned bytes and diagnostics. The world owner validates
the expected source revision before committing. Stale work is discarded without
overwriting a newer edit.

### Errors and limits

Script-facing errors name the method, bad field, accepted range, and active
limit. They never expose a raw allocator failure or GPU handle.

Every count is checked before narrowing. Every number is checked for finiteness.
Large parameter tables, burst counts, curve keys, graph nodes, and emitted events
are refused before allocation.

## Save and replication

### Saved state

Save these authored values:

- standalone emitter, beam, trail, decal, and texture properties;
- `VisualEffect` asset reference and playback settings;
- explicit parameter overrides;
- effect-definition buffer canonical bytes or a stable published reference;
- persistent player's logical start time, seed, loop state, and revision when
  resume-on-load is enabled.

Do not save:

- particles or projected-decal pool entries;
- trail history;
- emitter block indices or generations;
- GPU buffers and pipeline handles;
- resolved texture, mesh, or material handles;
- visibility, sort, or portal-view caches;
- pending collision callbacks;
- Studio preview state.

Readers reset all derived state and validate versions before allocating.

### Replicated event

A one-shot replicated event contains only:

- stable effect asset name;
- stable event key for deduplication;
- authoritative simulation tick or timestamp mapping;
- transform and optional source velocity;
- explicit seed;
- bounded typed parameter overrides;
- relevance scope and priority;
- declared late-arrival policy.

It never contains particle positions, random samples, pool slots, renderer state,
or ECS pointers.

### Persistent effects

Persistent world effects replicate as compact player state after client replicas
register the required effect classes. Attachment references use the replication
layer's stable instance identity and resolve only after both endpoints exist.

An unresolved reference leaves the output inactive. A later resolution revision
activates it. It never guesses from a local ECS id.

### Reliability

Use reliable delivery for persistent starts, stops, and rare authored events
whose absence is conspicuous. High-rate cosmetic events may use an unreliable
sequenced channel when their effect definition declares that loss is acceptable.

Reliable replay and late join do not resend historical sparks. They reconstruct
persistent effects from player state and include only recent one-shots still
inside their authored relevance window.

### Validation and abuse control

The server validates that a client may request an effect, that the stable name is
allowed, and that transform and parameters are in policy. It assigns or validates
the seed and authoritative tick.

Per-client token buckets limit events, bytes, unique asset names, parameter
count, and estimated visual cost. Refused requests are counted by reason. They do
not allocate a player or trigger a content fetch.

## Studio authoring

### Effect editor

Add a `View > Visual Effects` dock widget. It contains:

- effect graph canvas;
- selected-node properties;
- output list and budget estimates;
- parameter list;
- dependency list and readiness state;
- compile diagnostics;
- seed, quality, and time controls;
- play, pause, restart, step, scrub, and burst controls;
- CPU reference and GPU backend selector for comparison;
- publication status and bake progress.

The panel is closable, appears in the View menu, participates in the default
dockspace only after the dockspace version is deliberately bumped, and restores
its graph selection without creating a floating orphan panel.

### Preview world

Preview uses a small Studio-owned world and the normal effect systems. The panel
may choose built-in preview geometry, a selected scene object, or a neutral grid.
It never runs a separate particle simulator.

The preview supplies:

- fixed and orbiting cameras;
- optional floor and collision primitives;
- lighting presets;
- portal and mirror test preset;
- scale reference;
- transparent and checker backgrounds;
- deterministic seed cycling;
- quality and device-capability emulation.

Preview rendering stops when the panel is closed, hidden behind another dock tab,
or has zero extent. Compilation may continue in a worker, but no hidden panel
continuously advances effects or reads back images.

### Scrubbing

Scrubbing rebuilds from time zero or from a bounded checkpoint cache. Checkpoints
contain compact CPU preview state only and are keyed by program signature, seed,
parameters, quality, and checkpoint time.

The cache has a byte ceiling and least-recently-used eviction. Editing an upstream
node invalidates only checkpoints for the changed document hash.

### Properties and pickers

The generic Properties and Components panels remain the property authority.
Specialized controls add:

- curve and gradient editors;
- asset pickers filtered by texture, mesh, material, or effect kind;
- attachment reference picker when that Studio feature exists;
- bounds and collision visualization;
- current pool, cost, and fallback diagnostics;
- a button to open and focus the effect editor for a selected definition.

The specialized editor writes through the same command and undo path as generic
properties. It does not maintain a second mutable copy of the effect document.

### Undo and async safety

One graph edit is one Studio command. A worker compile result includes the
document revision it read. It may commit only if that revision is still current.

Undoing an edit immediately restores the prior document and schedules or reuses
its compiled program. A late worker result from the undone revision is discarded.

### Import and publication

Import recognizes canonical effect assets and supported external effect formats
through explicit converters. Conversion produces warnings for unsupported
modules and never silently changes gameplay semantics because effects have none.

Bake and publish show stage progress:

```text
validate -> compile -> resolve dependencies -> preview checks -> encode -> publish
```

Cancel is cooperative. Already published content-addressed chunks may remain in
the local cache, but no manifest name points to an incomplete asset.

## Scheduling and frame order

The intended world order is:

1. consume authoritative and local visual effect events;
2. resolve newly ready definitions and player revisions;
3. sample moving emitters and attachment transforms;
4. record trail points at the fixed simulation rate;
5. update persistent effect clocks and produce spawn descriptions;
6. run the CPU visual step or publish device-step commands;
7. collect bounded visual collision and child events;
8. commit player retirement and pool claims at the mutation barrier;
9. build copied presentation metadata and ribbon streams;
10. render each view from the same completed state;
11. retire frame-safe device resources and drain diagnostics.

Independent CPU emitter blocks may use `Jobs::For` within a tick because the
call joins before the tick ends. Blocks do not share particle ranges. Completed
worker durations are reported to the frame owner after the join.

Asynchronous asset compilation may span ticks because it is not simulation. Its
result commits only against the source revision it was built from.

## Security and robustness

### Asset limits

The reader validates before allocation:

- file bytes;
- graph nodes, ports, and links;
- outputs and modules per output;
- parameters and override bytes;
- curve keys and sampled table bytes;
- dependency names and total dependency count;
- child-effect depth and cycle count;
- requested pool entries and estimated resident bytes;
- shader references and capability declarations;
- string lengths and UTF-8 validity;
- every offset, multiplication, and count conversion.

Unknown versions, truncated sections, overlapping sections, trailing data where
forbidden, invalid enum values, non-finite numbers, and checksum mismatches are
refused with bounded diagnostics.

### Runtime limits

Runtime guards include:

- one hard pool byte ceiling per family;
- one world event queue ceiling;
- one collision-event ceiling;
- one child depth and event ceiling;
- one content-demand ceiling for unresolved effects;
- script and network token buckets;
- compile and bake cancellation;
- device dispatch bounds checked against resident allocation;
- no unchecked multiplication of count and stride;
- no user-provided workgroup size.

### Failure behavior

Missing visual content shows a visible fallback or skips the declared output.
It does not stop world simulation. A malformed effect is quarantined by content
hash so the loader does not retry it every frame.

Diagnostics identify the effect name, content hash, stage, stable node or output
name, and bounded reason. Repeated per-particle failures aggregate into counters
instead of flooding logs.

## Diagnostics and profiling

### Runtime panel

The diagnostics surface reports per world and selected effect:

- backend in use;
- active players and outputs;
- logical and device simulation revisions;
- pool capacity, live entries, and peak entries;
- emitted, retired, and dropped entries;
- culled and drawn entries per view;
- ribbon vertices and projected decals;
- mesh-particle triangles;
- field and collision query counts;
- host-to-device bytes;
- effect asset and dependency readiness;
- program compile and cache status;
- active quality reductions;
- refused claims and the limiting ceiling.

### Profile scopes

Use existing profiling macros around meaningful boundaries:

- resolve effect programs;
- refresh effect players;
- build spawn commands;
- CPU particle step;
- field application;
- visual collision;
- record trails;
- build ribbons and projected decals;
- prepare effect residency;
- GPU emit, step, scatter, and draw nodes;
- per-view cull and sort;
- Studio compile and preview capture.

Count bytes and operations at the transfer or allocation boundary. Worker scopes
remain on worker threads for Tracy and report completed durations to the owner for
the in-game frame graph.

### Release measurements

Every performance claim names:

- `release` preset;
- platform and graphics backend;
- device and driver;
- world count and camera count;
- portal depth and visible views;
- effect asset and seed;
- active emitters and live entries by primitive;
- collision, fields, lighting, and shadow modes;
- CPU or GPU ownership;
- warm-up and sample window;
- median, high percentile, and worst observed frame;
- host and device memory peaks;
- upload bytes and draw counts.

The existing half-million-particle scene remains a baseline. New stress scenes
must include mixed primitives, high churn, multi-view portals, and constrained
memory rather than replacing it with one favorable case.

## Migration plan

### Preserve the simple classes

`ParticleEmitter`, `Beam`, `Trail`, `Decal`, and `Texture` remain public. Existing
saved worlds and scripts continue to work. Their implementation becomes one set
of direct effect-program outputs where sharing is useful, but the compatibility
surface does not require authors to create a graph.

### Reconcile runtime ownership first

Before adding new output kinds:

1. document the current CPU and device paths accurately;
2. pin their common spawn, lifetime, curve, and retirement behavior in tests;
3. measure the backend crossover in release;
4. name one authoritative source for packed emitter input;
5. delete any stale path only after capture and fallback coverage prove it is
   redundant.

### Introduce compiled programs beside direct emitters

The first `VisualEffect` program wraps one particle output and must render
identically to an equivalent standalone emitter for a fixed seed. Then add
multiple outputs and parameters. This establishes the compiler and player
lifecycle before new rendering features enlarge the problem.

### Move no saved runtime state

Current trail serializers already omit history, and pool resources reset on
load. New player and graph serializers follow that model. A migration never
copies live GPU state into a file to preserve a preview frame.

### Replication is opt-in and ordered

Do not add `effects.` to a broad replicated prefix. First register all needed
effect classes on replicas, define reference remapping, add compact event
messages, and test a late join. Only then enable persistent effect-player state
for selected classes.

## Delivery phases

### Phase 0: baseline and contract

- reconcile module policy with the current device-owned implementation;
- capture current standalone emitter, beam, trail, decal, portal, and save
  behavior;
- add compact CPU and GPU comparison fixtures for fixed seeds;
- measure release CPU, GPU, upload, draw, and memory baselines;
- document active safety ceilings and refusal counters;
- keep all public behavior unchanged.

Gate: current examples and tests pass, current image output is pinned where a
pixel test is stable, and baseline measurements are recorded.

### Phase 1: effect asset and compiler

- add canonical effect-definition schema and versioned asset kind;
- add validation, stable ids, typed parameters, dependency extraction, and
  content signatures;
- compile one particle output to the current emitter-block representation;
- add asynchronous load and compile tickets;
- add malformed-input, determinism, save, and cache tests;
- publish through the existing asset pipeline.

Gate: one compiled output matches one standalone emitter for fixed inputs, and a
headless process can load, compile, step, and inspect it without a renderer.

### Phase 2: player and script API

- add `VisualEffect` and the `VisualEffects` service;
- add lifecycle, late-load policy, stable seeds, parameter overrides, and one-shot
  queues;
- add Luau and JavaScript parity plus generated declarations;
- add `EffectDefinitionBuffer` builder, validation, commit, import, and export;
- add bounded script and event limits;
- add save and restore of authored player state.

Gate: both script runtimes build, bake, play, stop, restart, and parameterize the
same effect with matching diagnostics.

### Phase 3: Studio graph and preview

- add the Visual Effects dock widget over the existing nodegraph editor;
- add graph commands, undo, properties, curves, gradients, and asset pickers;
- add ordinary-world preview, deterministic seeds, quality controls, and
  visibility gating;
- add async compile status, cancellation, stale-result rejection, and bake
  progress;
- add dependency, cost, and capability diagnostics.

Gate: a headless graph test covers compile and undo data paths. Windowed Studio
inspection is requested for docking, interaction, and visual preview before the
phase is marked complete.

### Phase 4: compact replication

- add replicated one-shot event schema and stable deduplication keys;
- validate authority, relevance, rates, bytes, parameters, and late arrival;
- add replica effect-class registration before persistent player state;
- add attachment remapping and unresolved-reference behavior;
- add late-join reconstruction for persistent effects;
- add multi-client disagreement tests.

Gate: two clients receive the same logical event and seed while running different
visual quality levels, and authoritative world state remains identical.

### Phase 5: mesh particles and richer ribbons

- add mesh-particle packed state, residency, grouping, instanced drawing, and
  triangle budgets;
- add ribbon output, segment ceilings, curve properties, texture modes, and
  teleport breaks;
- add material integration and visible fallbacks;
- add portal and multi-view rendering for both paths;
- add image and stress tests.

Gate: mixed billboard, mesh, beam, and trail scenes remain inside declared memory
and frame budgets with no per-entry asset lookup.

### Phase 6: projected decals, fields, and visual collision

- add projected-decal pool and graph output;
- add bounded field tables and spatial lookup;
- add copied collision-scene contract and CPU reference responses;
- add optional device visual collision where render capabilities permit;
- add bounded collision and child-event delivery;
- add security, overflow, and fallback tests.

Gate: collision and field features can be disabled without changing gameplay,
and hostile counts cannot exceed memory, dispatch, or callback ceilings.

### Phase 7: quality, caching, and final hardening

- calibrate cost weights from release captures;
- add deterministic admission and quality reductions;
- add per-view, portal, offscreen, and hidden-preview throttling;
- finish cache invalidation and device-loss recovery;
- fuzz asset, event, parameter, and graph readers;
- run mixed-world, high-churn, low-memory, and soak profiles;
- remove superseded adapters after all callers migrate.

Gate: completion criteria below pass on supported platforms.

## Test plan

### Effects module tests

- identical seeds and spawn descriptions produce identical spawn counts and
  sampled initial values;
- changing worker count does not change output;
- pool slot reuse does not repeat old spawn randomness;
- block generations invalidate retired state;
- bursts fire exactly once across ordinary steps and loop wraps;
- distance emission is independent of presentation frame count;
- disabled emitters stop spawning and let existing entries retire;
- child depth and count ceilings drop deterministically;
- fields rank and apply in stable order;
- collision responses remain bounded and visual-only;
- trail sampling is fixed-tick and teleport breaks do not join geometry;
- save writers omit pools, history, handles, and caches;
- readers restore defaults and reject malformed counts;
- program compile is canonical across node insertion order where semantics match;
- unknown nodes and wrong parameter types produce stable diagnostics.

### Asset and script tests

- canonical effect bytes round-trip byte for byte;
- content hashes change for semantic changes and settle for no-op commits;
- dependency lists are unique, sorted, and complete;
- cyclic child effects and oversized graphs are refused before allocation;
- stale async compilation cannot overwrite a newer revision;
- Luau and JavaScript expose equivalent methods and errors;
- script bulk commits are atomic on validation failure;
- token buckets refuse floods without creating players or fetching content.

### Replication tests

- a compact event round-trips stable names, seed, tick, transform, and typed
  parameters;
- duplicate reliable events play once;
- out-of-order sequenced cosmetic events do not rewind a player;
- late arrivals follow skip, start-late, or bounded-catch-up policy;
- late join reconstructs persistent players without historical particle lists;
- unresolved attachments activate only after stable reference resolution;
- two quality levels receive the same logical event and may draw different
  visual counts;
- a client cannot target a forbidden effect or exceed event policy;
- no ECS handle or pool index appears in a wire payload.

### Render and image tests

- fixed-seed billboard output matches a small reference image within declared
  backend tolerance;
- additive and blended outputs select the correct pipelines;
- soft particles fade at geometry intersections when depth is available;
- lit and unlit variants differ under a controlled light;
- mesh particles use fallback mesh and material when content is absent;
- beam and trail texture phase remains stable across segment counts;
- projected decals clip to receiver depth and obey normal fade;
- one effect seen by two cameras advances once;
- portal carry transforms particles, mesh particles, ribbons, and decals without
  changing canonical state;
- recursive portal limits do not allocate an unbounded view list;
- hidden Studio previews submit no effect draw or readback;
- device loss releases tracked memory and rebuilds from authored state.

Image tests use tiny controlled scenes, fixed cameras, fixed seeds, fixed assets,
and tolerant channel comparisons where backend rounding differs. They do not use
large opaque screenshots as the only assertion.

### Performance tests

- the existing half-million-particle benchmark remains;
- high emitter churn proves block and row reuse;
- mixed billboard and mesh particles measure triangle-aware budgets;
- many small effects measure event and claim overhead;
- fields and collision measure their crossover and spatial pruning;
- four views plus portals prove simulation advances once;
- offscreen and hidden-preview scenes settle to bounded work;
- content arrival changes only dependent program and material revisions;
- a quiet world performs no full authored-emitter walk or full table upload;
- memory soak proves live bytes settle after bursts retire;
- low-cap tests prove deterministic refusal and no allocation retry storm.

## Completion criteria

The system is complete for its first production release when:

- standalone effect classes still load and behave compatibly;
- one published effect can combine multiple primitive outputs;
- graph, buffer, and Studio authoring produce the same canonical asset;
- Luau and JavaScript have parity;
- deterministic seeds and compact events reproduce logical visual starts;
- client quality can reduce visuals without changing gameplay or event order;
- CPU reference and GPU paths pass their shared contract tests;
- mesh particles, ribbons, projected decals, fields, and visual collision obey
  explicit pool and work ceilings;
- every output renders correctly through ordinary, portal, and multi-view paths;
- hidden previews stop consuming render work;
- save files contain authored state and no transient pools;
- replication contains stable names and compact events, not particles or handles;
- malformed assets, scripts, and network events are refused before unbounded
  allocation;
- device loss, missing assets, compile failure, and pool exhaustion degrade with
  actionable diagnostics;
- release profiles record CPU, GPU, memory, uploads, draws, overdraw proxies, and
  quality reductions on supported platforms;
- architecture, formatting, focused tests, image tests, and sanitizer checks pass;
- old paths replaced by the program runtime are deleted after compatibility
  callers migrate.

## Explicit non-goals

The first production system does not include:

- gameplay damage or authoritative collision from visual particles;
- one ECS entity per particle, segment, or transient decal;
- arbitrary script execution inside particle or graph loops;
- a second render graph, material system, animation system, asset store, or
  physics world;
- unrestricted user compute shaders;
- exact particle-position agreement across different GPU vendors;
- saving or replicating live particle arrays;
- general fluid, smoke, fire, cloth, or destruction simulation;
- per-particle skeletal animation;
- unbounded child effects or event recursion;
- infinite trail history;
- full-scene mesh collision for every particle by default;
- automatic network replication of every local cosmetic effect;
- a universal graph virtual machine shared by unrelated systems.

Those features require separate measured designs. Their possible future need
does not justify putting unused fields or open-ended hooks in the first runtime.

## Decisions to make with measurements

These choices remain open until the named evidence exists:

1. Whether CPU output groups below a measured particle count are faster than
   device dispatch and residency updates.
2. Whether mesh-particle transforms remain full precision or can use a packed
   layout without visible instability at expected world scales.
3. Whether projected decals need a dedicated clustered cull or fit the existing
   transparent-instance cull at production counts.
4. Whether adaptive beam tessellation saves enough vertices to justify per-view
   error calculation and texture-phase tests.
5. Whether CPU visual collision can consume copied primitive grids cheaply enough
   to stay in the shared runtime.
6. Whether transparent particle sorting needs a new render-pipeline option after
   overdraw and order artifacts are captured in representative scenes.
7. Whether Studio scrub checkpoints save enough rebuild time to justify their
   memory on large effects.
8. Whether persistent effect replication belongs in selected reflected classes
   or a purpose-built compact player-state message after event replication ships.

Each decision records the release profile, scene, platform, result, and chosen
threshold beside the code it controls.
