# Terrain system plan

## Status

This document defines future terrain work. It does not describe a complete
runtime that exists today.

The first production path is heightfield terrain. Sparse signed-distance
volumes extend it for caves, overhangs, arches, and destructive edits that a
heightfield cannot represent. The two domains share graph authoring, chunk
identity, scheduling, materials, streaming, and publication, but they do not
pretend to be the same data structure.

The current `examples/Terrain.luau` and `TerrainCore` remain useful correctness
and performance probes. Their Luau APIs and box output are not contracts for the
engine implementation.

## Product goal

An author can create, edit, stream, save, and script deterministic terrain from
one reusable graph. The same compiled graph supports Studio previews, offline
bakes, headless server generation, and runtime visual streaming.

The system must provide:

- heightfields for the common ground case;
- sparse signed-distance volumes for caves and other non-heightfield geometry;
- typed node graphs with reusable subgraphs and exposed parameters;
- stable terrain materials and material weights;
- interactive brushes, stamps, splines, and deterministic runtime edits;
- chunked collision, rendering, level of detail, and world streaming;
- bounded CPU, GPU, memory, disk, network, and script work;
- useful progress, cancellation, diagnostics, and profiling;
- deterministic output from canonical inputs;
- formats that refuse hostile or unsupported data before allocating from it.

## Current foundation

### Saved world recipe

`scene::Terrain` is already a one-per-world ECS resource. It owns the authored
recipe, not generated chunks. Its saved fields are:

- `Seed`;
- stable generator `core::Name`;
- chunk extent in metres;
- vertical extent;
- view distance;
- chunk resolution;
- enabled state.

The generator name serialises as text. Reads clamp chunk extent and resolution
to declared ceilings. A world with no resource behaves like terrain is disabled.
Workspace properties expose the recipe without creating a second copy.

This remains the world-level entry point. New authored settings extend the
resource through versioned save and replication schemas. Generated samples,
meshes, collision, and GPU allocations remain derived artifacts.

### Editable mesh bridge

The current editable-mesh path already supplies several required publication
rules:

- scripts submit one complete geometry transaction;
- worker preparation validates arrays and computes a content signature;
- owner-thread commit checks the expected revision;
- identical geometry is a no-op;
- changed geometry advances one revision;
- collision and rendering consume the revision;
- object transforms do not rebuild object-space vertices;
- Luau and JavaScript can suspend on a ticket while other work continues.

Early terrain mesh output should use this bridge so the system can ship in
small steps. Long-term terrain artifacts need content-addressed chunk residency
instead of one `EditableMesh` instance per permanent chunk.

### Terrain example

`TerrainCore` is pure Luau data and never touches an Instance. It demonstrates:

- coherent seeded noise;
- a heightfield separated from its edit layer;
- climate and surface selection;
- gated three-dimensional cave sampling;
- crater records that save and replicate as plain data;
- spatial buckets for edits;
- local edit invalidation;
- cached immutable samples and incrementally updated mutable samples;
- greedy merging of adjacent equal columns;
- a runtime adapter with chunk rebuild queues and a bounded part pool.

Those are useful lessons. The production system must not inherit the example's
unbounded arena cache, per-column script cost, box-only geometry, Roblox
material strings, or assumption that caves can always be expressed by stacked
columns.

### Related engine systems

- `parallel::Jobs` supplies measured fork-joined work within a batch.
- `scene::CollisionShapes` holds derived collision geometry by stable name.
- `physics::PhysicsWorld` already separates static and dynamic broadphase data.
- `assets::MaterialData` publishes named PBR texture sets.
- `render::MeshTable` and editable-mesh residency already separate geometry
  content from instance transforms.
- Studio owns the client-only nodegraph widget. Engine modules cannot link it.
- `graph` owns render decisions and its render graph. It is not a generic
  procedural-authoring runtime.
- `bakegraph` describes the closed asset-import pipeline. Terrain node kinds do
  not belong in that persisted enum.

## Architecture and ownership

### New terrain module

A dedicated `mono.engine/terrain` module is justified. No current module owns
terrain value domains, terrain graph compilation, generation requests, chunk
artifacts, or terrain caches. Putting those pieces in `scene`, `graph`, `bake`,
or `render` would violate an existing module's stated purpose.

The proposed module is shared and provisionally sits at L8. It remains usable by
headless servers and has no dependency on ECS, Studio, a renderer, an importer,
or a scripting VM. Its initial dependencies are limited to lower-layer value,
job, spatial, and collision primitives that it actually consumes. The exact
edge list and layer must be added to `expected_graph.json` and pass both
architecture checks before code lands.

Responsibilities are divided as follows:

| Owner | Responsibility |
|---|---|
| `scene` | Authored world recipe, edit references, stable artifact names, and revisions visible to the ECS |
| `terrain` | Graph schema, validation, compilation, CPU execution, requests, artifacts, signatures, scheduling policy, and bounded caches |
| `game` or host integration | Read ECS recipes, submit immutable requests, and commit accepted results at world barriers |
| `collision` | Collision value types and deterministic collision builders used by terrain |
| `physics` | Consume published terrain collision without knowing how terrain was generated |
| `render` | GPU execution backend, terrain residency, draw selection, and device retirement |
| `assets` and `resources` | Published graph, stamp, material, and baked terrain payloads |
| scripting adapters | Luau and JavaScript parity over the engine-owned API |
| Studio | Graph editing, brushes, previews, progress, diagnostics, and export |

The terrain compiler remains pure. It accepts owned graph bytes and request
values and returns owned artifacts. It does not reach into an ECS store.

### One copy of each fact

Authored state lives in the ECS. Derived fields and artifacts live in the
terrain cache until publication. Render and collision residency carry only the
consumer-specific form and the source signature that made it.

There is no second mutable chunk table in Studio, scripting, or rendering.
Editor panels query the same scheduler and cache state used by the runtime.

Nothing crossing a world or process boundary is a pointer. Messages contain
stable graph names or canonical graph bytes, seeds, integer chunk coordinates,
LOD, revisions, and owned artifact bytes.

## Scope boundaries

### Included

- procedural graph authoring;
- heightfield and sparse volumetric terrain;
- caves, overhangs, islands, rivers, and material layers;
- deterministic destructive and constructive edits;
- terrain rendering, collision, LOD, residency, and streaming integration;
- placement outputs for foliage, rocks, water, and gameplay markers;
- offline baking and runtime generation;
- script construction, queries, edits, tickets, and diagnostics.

### Not included in the first production release

- a general-purpose graph virtual machine shared by unrelated systems;
- fully dynamic fluid simulation;
- deformable soil physics;
- arbitrary per-sample Luau or JavaScript callbacks;
- globally simulated erosion every frame;
- distributed runtime generation across untrusted clients;
- a replacement for authored mesh worlds;
- automatic conversion of every existing part into terrain;
- editable planet-scale precision before flat-world chunking is proven.

Offline erosion, river solves, and distributed trusted build workers can arrive
after the deterministic local compiler and artifact formats are stable.

## Heightfield, voxel, and signed-distance decision

### Heightfield is the default

A heightfield stores one surface elevation per horizontal sample, plus named
layers. It is compact, easy to filter, fast to mesh, simple to collide with, and
fits most outdoor ground.

Heightfield chunks are the first shipping representation for plains, mountains,
coasts, roads, river beds, and ordinary craters that do not create an underside.

### Signed-distance volumes are the extension

A signed-distance field stores a scalar in metres. Negative values are solid,
positive values are empty, and zero is the surface. Nodes may return an exact
distance or a conservative approximation, but must declare which contract they
provide.

SDF bricks are allocated only where a graph or edit declares volumetric detail.
They represent:

- caves and tunnels;
- overhangs and arches;
- mesh and primitive stamps with undersides;
- deep cuts through terrain;
- constructive geometry above the surface;
- future smooth destructive edits.

A production cave is an SDF region. A heightfield may create a depression or a
masked hole entrance, but it cannot claim to represent a tunnel ceiling.

### Voxels are storage cells, not the public model

Dense occupancy voxels are useful for tests, coarse edits, and block-styled
output. They are not the canonical authored format. A binary cell loses smooth
surface location, makes LOD harder, and multiplies storage.

The volumetric sampler can quantise an SDF into block occupancy when an output
explicitly requests block terrain. Greedy meshing then emits visible faces.
Faces are partitioned into the six cardinal groups, `Front`, `Back`, `Left`,
`Right`, `Top`, and `Bottom`, so rendering can omit groups that face away from
all relevant views. This grouping is an output optimisation, not authored
terrain state.

### Hybrid boundary

One chunk may have a heightfield base and sparse SDF override regions. The
compiler produces a single surface artifact after applying overrides. It must
not draw coincident heightfield and SDF surfaces.

The first hybrid prototype must settle:

- how an SDF brick clips or replaces the heightfield surface;
- how materials cross the boundary;
- how normals and collision remain continuous;
- how edits promote a heightfield-only region into a volumetric region;
- when an empty volumetric region is compacted back to heightfield-only data.

Until that prototype passes seam tests, a chunk uses one meshing domain at a
time and declares it in its artifact metadata.

## Coordinate and determinism contract

- World distances are metres.
- Chunk addresses are signed integer coordinates plus an unsigned LOD.
- Samples derive from integer chunk and sample coordinates. Adjacent chunks do
  not accumulate independent floating-point origins.
- Local sample positions remain near zero. A world-origin transform places the
  artifact at large coordinates.
- Seed derivation hashes stable graph, node, output, chunk, and user seed text or
  values. It never uses registration order or `core::Name::Id()`.
- Random streams are explicit node inputs and are independent of traversal or
  worker order.
- A node declares bitwise, strict IEEE, or tolerance-based parity.
- Reduction order is fixed. Parallel execution writes independent ranges and
  merges them in stable index order.
- A compiled compatibility version is part of every content signature.

Visual terrain may finish at different wall-clock times. Gameplay terrain may
not change based on which machine completed first. The authoritative host
chooses an activation tick after required collision is ready, records or
replicates that activation, and prevents simulation from entering an inactive
cell. Replays consume the recorded activation sequence.

## Authored data model

### Terrain graph asset

A terrain graph asset contains:

- graph format version;
- stable graph name and compatibility version;
- nodes with stable instance names and stable type names;
- typed input and output socket names;
- canonical parameter values;
- connections;
- exposed parameters;
- named outputs;
- versioned subgraph references;
- optional authoring metadata in a separate non-semantic section.

Node ids derived from array position never leave a process. Canonical
serialisation sorts unordered declarations by text and normalises floating-point
and string encodings. Editor positions, selection, collapsed state, comments,
preview pins, and diagnostic history do not affect generated content.

Unknown required node types or versions are errors. Studio preserves an unknown
node's source record so opening a graph in an older build does not destroy work,
but that graph cannot compile until every reachable required node resolves.

### Value domains

The type system supports:

- scalar, integer, boolean, enum, seed, vector, and colour values;
- curves and gradients;
- two-dimensional scalar and vector fields;
- three-dimensional scalar and vector fields;
- signed-distance fields;
- heightfields;
- named layer sets;
- material-weight fields;
- splines and bounded stamp sets;
- mesh and collision artifacts;
- placement sets with stable item ids, transforms, prototypes, and attributes.

Socket types include units, coordinate space, dimensions, sampling convention,
and whether a value is uniform or varies per sample. A world-metre field cannot
silently connect to an index-space field.

### Named layers

Named layers let one expensive result feed several consumers. Initial standard
names include:

- `height`, `distance`, `mask`, and `water`;
- `slope`, `aspect`, `curvature`, and `normal`;
- `flow`, `flow_direction`, `sediment`, `deposition`, `debris`, and `wear`;
- `temperature`, `humidity`, and biome weights;
- material weights such as `rock`, `soil`, `sand`, `snow`, and `vegetation`;
- ambient occlusion and placement exclusion.

Names are contracts. Custom names are allowed within declared length and count
limits. A consumer asks for a name and handles absence explicitly.

### World recipe extensions

The existing `scene::Terrain` resource should grow only with world-wide authored
policy:

- graph asset name and seed;
- exposed parameter overrides;
- base chunk extent and sample resolution;
- vertical bounds;
- visual, collision, and edit distances;
- maximum visual and collision LOD;
- terrain material set;
- enabled and runtime-editable flags;
- save and replication policy for edits.

Large override maps and edit journals are referenced by stable asset or resource
name instead of embedded into a fixed-size component.

### Generation request

An immutable request contains:

- graph content signature or canonical bytes;
- graph compatibility version;
- world seed and parameter signature;
- integer chunk coordinate and LOD;
- requested domain and outputs;
- sample resolution and halo;
- source edit revision;
- target identity and expected revision as values;
- stable priority class and monotonic request ticket;
- explicit resource limits.

The request signature includes every field that can change output. Priority,
ticket, editor layout, and destination identity do not affect content identity.

### Artifacts

A completion can own any combination of:

- height and named field tiles;
- SDF bricks;
- render mesh groups, bounds, normals, tangents, UVs, and material weights;
- simplified collision heightfield or triangle mesh;
- water and river geometry;
- placement sets;
- diagnostic ranges, histograms, and node timings.

Each artifact carries its content signature, domain, chunk key, LOD, bounds,
source edit revision, byte size, and format version. Bounds are recomputed and
validated from payload data at hostile boundaries.

## Node catalogue

The first catalogue should establish the type system without attempting every
expensive simulation.

### Coordinates and input

- world, chunk, local, polar, cylindrical, and spherical coordinates;
- constants and exposed parameters;
- stable seed derivation and random streams;
- curves, gradients, images, splines, imported fields, and stamps;
- translate, rotate, scale, repeat, mirror, warp, and quantise.

### Noise and patterns

- value, Perlin, simplex, OpenSimplex2, cellular, and white noise;
- fractal Brownian motion, ridged, billow, ping-pong, hybrid, and weighted
  fractals;
- domain warp, curl, and turbulence;
- checker, stripe, radial, Voronoi region, and blue-noise patterns;
- explicit frequency, octave, lacunarity, gain, jitter, distance metric, and
  normalisation settings.

### Terrain sources and SDF primitives

- plane, dome, cone, ridge, mountain, dune, terrace, plateau, crater, canyon,
  island, and coastline;
- river and road splines;
- sphere, box, capsule, torus, cylinder, and mesh SDF stamps;
- add, subtract, intersection, min, max, smooth union, smooth subtraction,
  masked selection, and blend.

### Field operations

- arithmetic, clamp, absolute, power, logarithm, bias, gain, and remap;
- curve, terrace, posterise, blur, sharpen, dilate, erode, and resample;
- select, blend, overlay, mask, invert, threshold, and falloff;
- gradient, normal, slope, aspect, curvature, laplacian, distance, and flow
  accumulation.

### Simulation

Later catalogues add:

- hydraulic and thermal erosion;
- weathering, talus, sediment transport, and deposition;
- river and drainage solves;
- coastal erosion;
- snow and glacier accumulation.

Every iterative node declares iteration bounds, neighbourhood radius, halo,
temporary memory, cancellation granularity, determinism contract, backend
support, and secondary outputs.

### Biomes, materials, and placement

- masks from height, slope, aspect, latitude, climate, water, distance, shapes,
  and painted layers;
- hierarchical biome views compiled into ordinary named mask composition;
- material priority, blending, and normalisation;
- density fields, exclusion masks, Poisson placement, and blue-noise placement;
- stable prototype choice, scale, rotation, colour, and custom attributes;
- erosion-aware and biome-aware vegetation and rock placement.

### Outputs

- preview, probe, thumbnail, and field export;
- heightfield and SDF tiles;
- render mesh, collision, material, water, and placement artifacts;
- baked image, layer set, graph intermediate, or terrain package.

## Graph validation and compilation

### Validation

Compilation refuses a graph before expensive work when:

- node or socket names do not resolve;
- required sockets are absent;
- domains, units, coordinate spaces, or sampling rules disagree;
- a cycle exists outside an explicit bounded iteration node;
- requested outputs are unreachable;
- a referenced graph or asset is absent or recursively cyclic;
- node, edge, layer, parameter, iteration, halo, sample, or memory limits are
  exceeded;
- a node lacks the required determinism or backend contract;
- a graph requests a volumetric operation from a heightfield-only output.

Diagnostics are structured and name the graph, node, socket, rule, and bounded
suggested fix. Runtime evaluation does not discover type errors halfway through
a chunk.

### Compiled runtime

The compiler lowers only reachable nodes into a terrain-specific intermediate
representation. It performs:

- constant folding;
- dead-node removal;
- common-subexpression elimination;
- coordinate-transform folding;
- field lifetime analysis;
- mask and arithmetic fusion;
- backend stage formation;
- fixed temporary-memory planning.

Element-wise chains fuse into one SIMD loop or GPU kernel. Neighbourhood,
simulation, meshing, collision, and explicit cache nodes form materialisation
boundaries. The compiled plan uses dense handles inside one process, but retains
stable source names for diagnostics.

Compilation is separate from execution. A graph edit compiles once and reuses
the plan across chunks whose semantic inputs match.

### CPU backend

- structure-of-arrays field tiles;
- runtime-selected SIMD lanes;
- no allocation inside a fused sample loop;
- measured serial fallback for small previews and chunks;
- `parallel::Jobs::For` only above release-build crossover measurements;
- independent range writes followed by stable ordered merges;
- explicit scratch budgets reused by stage shape.

### GPU backend

The GPU backend is optional per node and follows the same validated intermediate
representation. It keeps fields, parameters, indirect work, and artifacts
resident where profitable. It uploads compact dirty inputs instead of every
intermediate field.

Readback occurs only for an explicit CPU consumer, save, diagnostic, or parity
check. A graph cannot silently switch to a backend with a weaker determinism
contract.

## Chunks, halos, LOD, and seams

### Chunk identity

`TerrainChunkKey` is `(domain, x, y, z, lod)`. Heightfields keep `y` at zero.
SDF uses three-dimensional brick coordinates. The key is a value and has a
canonical byte encoding.

Chunk extent doubles with each coarser LOD while sample count remains bounded.
Sample coordinates come from the key and integer sample index.

### Halos

Every node declares how many neighbouring samples it reads. The compiler
propagates the maximum required halo backward through reachable stages. Requests
with a halo beyond the configured ceiling fail validation.

Simulation runs on overlapped tiles. Publication crops the halo. Nodes that
require blending declare an explicit transition width and combine rule. Hidden
implicit padding is forbidden because it makes cache keys and seams lie.

### Heightfield meshing

The baseline mesh is a regular indexed grid with shared boundary samples.
Normals derive from the same halo-backed field on both sides of an edge.
Material weights sample at the same world positions.

LOD neighbours differ by at most one level in the active set. Stitch index
patterns join coarse and fine edges. Skirts remain a conservative fallback for
missing neighbours and streaming transitions, not the primary crack fix.

### Volume meshing

Dual contouring, marching cubes, and surface nets require measured prototypes.
The chosen mesher must provide:

- deterministic vertex and index ordering;
- crack-free same-LOD brick boundaries;
- a documented coarse-to-fine transition scheme;
- bounded temporary memory;
- stable material assignment;
- collision output that agrees with the rendered zero surface;
- graceful handling of empty and completely solid bricks.

### LOD selection

Visual LOD is per-view and belongs beside render culling. Generation demand is
the union of relevant views, not one camera chosen as special. Shared chunks are
generated once.

Collision LOD follows gameplay interest, not camera distance. The server may
keep fine collision ahead of a moving body while rendering uses a coarser mesh.
Navigation and placement consumers declare their own required artifact LOD.

Selection uses hysteresis so hovering around one threshold does not rebuild or
swap every frame. LOD changes select resident artifacts and do not rewrite ECS
terrain state.

## Terrain materials

Terrain materials are stable asset names resolved through the existing material
and resource systems. Generated data stores names or bounded layer indices from
a named material set. Session-local material handles never enter a save or wire
format.

The baseline supports:

- up to a measured, fixed number of material weights per surface sample;
- deterministic weight normalisation and priority ties;
- triplanar world-space mapping for steep and volumetric surfaces;
- planar or authored UV output where a graph requests it;
- normal, roughness, occlusion, height, emissive, and metalness maps already
  carried by `MaterialData`;
- material distance scaling and future virtual-texture integration;
- a separate physical surface name for friction, restitution, footsteps, and
  gameplay queries.

Rendering blends visual material weights. Physics resolves one stable physical
surface per collision region using a documented dominant-weight rule. A raycast
returns the stable material or surface name used at the hit.

Material-only graph edits invalidate material artifacts and render bindings.
They do not rebuild an unchanged heightfield or collision mesh.

## Editing model

### Edit operations

Runtime edits are bounded, server-authoritative commands over stable shapes:

- add or subtract sphere, capsule, box, and cylinder;
- raise, lower, flatten, smooth, terrace, and noise brush;
- paint material and named masks;
- spline road, river, and tunnel stamps;
- place a versioned mesh or SDF stamp;
- restore a bounded region to its generated base.

Each operation contains a stable operation id, authoritative sequence, shape,
transform, parameters, material effect, seed, bounds, and graph compatibility
version. Operations never contain a callback or a pointer.

### Edit journal and compaction

The base graph and seed remain immutable inputs. Edits form an ordered diff.
Spatial indexing maps an edit to every affected chunk and halo. Only affected
chunks and reachable artifact stages are invalidated.

The journal cannot grow forever. A deterministic compaction job folds a bounded
prefix into a versioned patch field, verifies its signature, then replaces that
prefix atomically. The save retains the patch plus later operations. Compaction
must produce identical output regardless of worker count.

Undo in Studio records inverse authoring operations or restores a prior patch
revision. Runtime gameplay undo is an explicit new command. It does not mutate
history in place.

### Editing domain promotion

An ordinary raise or shallow crater remains a heightfield edit. An operation
that creates an underside promotes affected chunks to a volumetric override.
Promotion is explicit in diagnostics and subject to volume memory limits.

Removing the last volumetric feature does not immediately demote a chunk. A
background compaction may prove that the SDF result is heightfield-equivalent
and publish a heightfield artifact with the same visible and collision surface.

## Collision and gameplay queries

Terrain generation produces collision independently from render mesh packing.
The first heightfield path can publish a deterministic triangle mesh through
the existing collision shape table. A native heightfield collider is a later
measured optimisation, not a prerequisite.

Rules:

- collision artifacts are built from the same sampled field and edit revision
  as visible artifacts;
- collision names derive from stable terrain and chunk signatures;
- shape bounds are recomputed from geometry;
- owner-thread publication updates the authoritative shape table atomically;
- the static broadphase is marked dirty once per accepted batch;
- unchanged signatures cause no collision rebuild;
- collision remains resident far enough ahead of authoritative bodies;
- a missing gameplay collision chunk is inactive space, never guessed flat
  ground;
- raycasts, overlap queries, footsteps, and placement queries return stable
  surface information.

The authority chooses when a newly ready chunk becomes active. Render may fade
or skirt a visual transition, but physics activation happens at one fixed-tick
barrier.

## Rendering and residency

Terrain render artifacts are immutable for a content signature. Instance or
world-origin transforms are separate resident data.

The render path provides:

- content-addressed terrain mesh residency;
- frustum, occlusion, and distance culling;
- per-view LOD selection with multi-view demand merging;
- indexed material-weight streams;
- indirect submission grouped by mesh and material layout;
- cardinal face-group rejection for block terrain;
- deferred resource retirement after in-flight GPU work;
- device-loss rebuild from CPU artifacts or canonical generation requests;
- zero geometry uploads while a view moves through an already resident area.

Moving, rotating, or scaling a terrain-bearing world transform updates instance
data only. It never changes object-space terrain content signatures.

Resident memory is bounded separately for CPU fields, CPU artifacts, collision,
GPU meshes, GPU fields, and preview images. Eviction prefers invisible coarse or
reconstructible data and never evicts active gameplay collision.

## Streaming integration

The future world-streaming system supplies stable interest sets. Terrain does
not discover players or cameras by itself.

Interest requests state:

- source id and kind;
- world position or explicit region;
- visual, collision, navigation, and edit radii;
- priority and deadline class;
- required LOD bounds;
- lifetime revision.

Terrain merges overlapping demands into one chunk plan. Priority order is
stable within a class. A default policy ranks active collision lookahead above
near visible terrain, then distant visible terrain, Studio previews, and
offline background work. An authority may choose stricter gameplay ordering.

Portals and surface cameras contribute additional visual interests only when
their view is active. Their chunks reuse ordinary residency and signatures.
They do not create a second terrain world.

Seamless world travel prewarms target-world terrain from copied request values.
No source-world pointer enters the target scheduler.

## Save, bake, and replication

### Saved state

Save files contain:

- the world recipe and stable graph reference;
- graph parameter overrides;
- stable material-set reference;
- edit patch signature and bytes or asset reference;
- ordered edits after the patch;
- format and compatibility versions.

Generated chunks are not saved by default because they are derived from those
inputs. An offline baked terrain package is allowed as an explicit content
artifact. It carries the source signature and is ignored or rebuilt when the
recipe no longer matches.

### Replication

The server replicates recipe changes, accepted edit commands, compaction
checkpoints, and authoritative activation events. It does not stream arbitrary
client-generated collision conclusions.

Clients may generate visual artifacts locally when their backend meets the
declared parity contract. A server can instead distribute signed baked artifact
content through the resource or CDN path. Collision remains authoritative on
the server.

Clients submit edit requests, not edit records. The server validates permission,
shape, bounds, rate, material, target world, and resource cost before assigning
the authoritative sequence.

Late joiners receive a bounded checkpoint plus later edits. They do not replay
an unbounded world history.

## Scripting surface

Luau and JavaScript expose the same concepts and errors. Suggested operations
are:

- read and set the world terrain recipe through Workspace properties;
- load or assign a terrain graph by stable asset name;
- set exposed graph parameters by stable name;
- submit a bounded generation, bake, or preview request;
- await, cancel, and inspect a ticket;
- query height, normal, material, distance, or occupancy over a bounded region;
- apply a validated edit command;
- subscribe to chunk-ready, chunk-evicted, edit-accepted, and edit-rejected
  signals;
- request a bounded field or mesh artifact for procedural tools;
- inspect cache and timing counters available to the caller's permission level.

Queries state whether they require active authoritative collision, generated CPU
fields, or may sample the pure graph directly. A call never blocks the world
thread on disk, network, or GPU work.

Per-sample script callbacks are forbidden. Scripts assemble graphs and submit
data-shaped requests. One million terrain samples do not cause one million VM
crossings.

Every region, sample count, output byte count, edit radius, ticket count, and
concurrent request count has a hard limit. Awaiting suspends only that script.
Completion resumes in stable ticket order at a documented scheduler barrier.

## Studio authoring

Studio edits the shared graph document through canonical operations. The
client-only nodegraph widget is a view and input adapter. It does not own terrain
semantics.

The Terrain panel provides:

- graph asset selection and creation;
- searchable typed node catalogue;
- typed sockets with domain, coordinate, and unit labels;
- inline constants and exposed parameters;
- reusable versioned subgraphs;
- named layer and output browser;
- 2D field, 3D surface, SDF slice, material, and placement previews;
- probes, histograms, ranges, NaN display, and boundary inspection;
- paint, sculpt, spline, stamp, material, and region tools;
- tile, halo, LOD, collision, and normal overlays;
- CPU and GPU backend comparison;
- cache hit, invalidation, and residency overlays;
- per-node and per-stage time, allocations, bytes, and queue delay;
- pause, cancel, rebuild selection, bake, export, and clear-cache controls;
- undo and redo over graph and edit operations;
- progress that names compile, sample, simulate, mesh, collide, upload, and
  publish phases.

Preview work runs at lower priority and may use a lower resolution, but it uses
the same compiler and node semantics. Preview-only terrain code would hide seam
and parity failures.

Selection changes and a hidden panel do not continuously regenerate previews.
Individual caches invalidate only when graph inputs, preview settings, or the
selected region change. GPU previews stop submitting when the panel is closed,
hidden behind another tab, or its world is inactive.

## Jobs, cancellation, and publication

### Request lifecycle

1. The owner captures an immutable request and target revision.
2. Validation and compilation produce a bounded plan or structured refusal.
3. The scheduler deduplicates an identical in-flight or cached signature.
4. A coordinator runs bounded stages and uses fork-joined jobs within a stage.
5. Cancellation is checked between stages, iterations, rows, and meshing blocks.
6. The completion queue receives an owned artifact and request identity.
7. The world owner drains completions in stable ticket order at its barrier.
8. A stale, destroyed, stopped, or superseded target is dropped without side
   effects.
9. An accepted batch publishes revisions, collision dirtiness, and render
   artifact names once.

Long-lived work owns its inputs. It never captures an ECS pointer or reference.
The coordinator should use `std::jthread` and `std::stop_token` if a dedicated
thread is needed, so teardown requests cancellation and joins. Internal parallel
ranges continue to use the existing process-wide job system unless profiles
prove a separate pool is necessary.

`parallel::Jobs::For` remains fork-joined. A terrain API must not add a general
`Jobs::Async` handle to solve its own lifetime problem.

### Cancellation semantics

- Cancel means stop as soon as the current bounded unit reaches a check.
- Cancelled work publishes nothing.
- Replacing a request cancels by revision and does not reuse its destination.
- A cacheable upstream artifact completed before cancellation may enter the
  cache only if its full signature and validation succeeded.
- GPU cancellation prevents later stages and publication. Submitted device work
  retires normally.
- World shutdown drains or rejects completions and joins owned coordinators.

### Cache hierarchy

Cache keys are complete semantic signatures. Layers are:

- compiled graph plans;
- immutable source and upstream fields;
- edited patch fields;
- meshes, collision, materials, water, and placement artifacts;
- device-resident resources;
- disk-backed baked artifacts.

Every cache has a byte ceiling, an entry ceiling, deterministic key equality,
and visible hit, miss, build, eviction, stale-drop, and rejection counters.
Graph edits invalidate reachable downstream stages only. Device resources use
deferred retirement.

## Hostile input and resource limits

Every file, network message, graph, edit, script request, and CDN artifact is
untrusted at its decoding boundary.

Decoders must:

- check magic and supported format version;
- use checked arithmetic before sizes, products, offsets, and allocations;
- bound node, edge, socket, layer, subgraph, string, and parameter counts;
- bound field dimensions, chunk resolution, halo, volume brick count, mesh
  vertices, indices, materials, placements, edits, and iterations;
- reject invalid UTF-8 where names require it;
- reject NaN and infinity unless a diagnostic format explicitly carries them;
- validate mesh indices and finite positions;
- derive bounds instead of trusting serialized bounds;
- verify signatures before installing cached or downloaded artifacts;
- leave destination state unchanged on failure;
- count and log refusals without dumping hostile payloads.

Runtime policy also limits generated bytes per world, requests per source,
edits per tick, edit reach, concurrent previews, disk use, and GPU residency.
Limits are configuration with safe ceilings, not values a script can raise.

## External design lessons

The design borrows concepts, not formats or implementations.

### World Machine

World Machine demonstrates generator, selector, modifier, erosion, and output
devices in one graph. Its erosion outputs expose wear, deposition, and flow.
Tiled builds use overlap because simulations need neighbours, and build history
makes cached previews practical.

- [Devices and the device workspace](https://help.world-machine.com/topic/devices-and-the-device-workspace/)
- [Erosion device](https://help.world-machine.com/topic/device-erosion/)
- [Tiled worlds](https://help.world-machine.com/topic/world-machine-professional-edition-addendum/)
- [Build history and caching](https://help.world-machine.com/topic/build-4015-mt-rainier-release/)

### World Creator

World Creator makes biome authoring hierarchical and filters where operations
contribute. Atomic can present that hierarchy while compiling it to ordinary
named masks and edges.

- [Biomes](https://docs.world-creator.com/reference/terrain/biome)
- [Biome filters](https://docs.world-creator.com/reference/terrain/biome/filters)
- [Terrain concepts](https://docs.world-creator.com/walkthrough/terrain-setup/understanding-terrains)

### Houdini heightfields

Houdini treats heightfields as named volume layers. Masks are ordinary layers,
and erosion publishes debris, sediment, water, flow, and direction. This is the
model for Atomic named terrain layers.

- [Heightfields](https://www.sidefx.com/docs/houdini/heightfields/index.html)
- [Heightfield masking](https://www.sidefx.com/docs/houdini/heightfields/masking.html)
- [Heightfield erosion](https://www.sidefx.com/docs/houdini/heightfields/erosion.html)

### Gaea

Gaea groups nodes by source, simulation, surface, modification, derived data,
colour, utility, and output. It exposes parameters and bakes intermediates.
Atomic should retain that discoverability while keeping editor layout separate
from execution.

- [Node families](https://docs.gaea.app/reference/nodes/)
- [Procedural graph workflow](https://docs.gaea.app/ui/interface/graph/procedural-workflow.html)
- [Simulation nodes](https://docs.gaea.app/reference/nodes/simulate/index.html)
- [Derived-data nodes](https://docs.gaea.app/reference/nodes/derive/index.html)

### FastNoise2

FastNoise2 separates node metadata from compiled execution and fuses operations
so intermediates can remain in SIMD registers. Atomic needs the same schema and
compiled-plan split, with explicit CPU and GPU parity.

- [Node graph architecture](https://github.com/Auburn/FastNoise2/wiki/Node-Graph-Architecture)
- [FastNoise2](https://github.com/Auburn/FastNoise2)

## Migration

Migration keeps one working path at every phase.

1. Keep `scene::Terrain` as the sole authored world recipe.
2. Keep the Luau example intact as a reference and regression workload.
3. Land graph schema, canonical encoding, validation, and CPU field evaluation
   without connecting runtime worlds.
4. Adapt one heightfield output to the existing bulk editable-mesh transaction.
5. Publish collision through the existing collision shape table.
6. Add host scheduling and chunk interest behind the disabled terrain recipe.
7. Move the example onto the engine API and compare output and timings.
8. Add dedicated content-addressed terrain residency.
9. Delete the box-part runtime and any duplicate chunk storage once the engine
   path covers its tests and demo.
10. Add sparse SDF chunks only after heightfield streaming and seams pass.

There is never a permanent `TerrainCore` runtime beside a native runtime. The
example becomes a test fixture or is removed after its useful cases move.

## Delivery phases and acceptance gates

### Phase 0: architecture and formats

- approve module layer and dependency edges;
- define graph, request, artifact, patch, and baked-package schemas;
- define limits and structured diagnostics;
- extend the world recipe without adding generated ECS state;
- add hostile decode and canonical round-trip tests.

Gate: malformed inputs cannot allocate past ceilings, canonical graphs produce
stable bytes and signatures, and both architecture checks pass.

### Phase 1: heightfield graph and Studio preview

- constants, coordinates, arithmetic, select, curve, basic noise, fractals,
  domain warp, masks, and heightfield output;
- compiled CPU plan with fusion and bounded scratch;
- 2D and 3D Studio previews from the shared compiler;
- compiled plan and field caches;
- cancellation and progress.

Gate: adjacent tiles share exact edge samples, previews stop when hidden,
results replay across worker counts, and graph edits invalidate only reachable
stages.

### Phase 2: chunk mesh and collision

- regular-grid heightfield meshing, normals, material weights, and bounds;
- bulk editable-mesh publication bridge;
- deterministic collision artifact and physical surfaces;
- edit operations, spatial invalidation, and owner-thread batch commit;
- no-op signatures and stale completion rejection.

Gate: a crater rebuilds only affected chunks, collision and visuals use one
revision, identical work causes no upload or broadphase rebuild, and all script
adapters resume deterministically.

### Phase 3: runtime streaming and LOD

- interest-set integration;
- multi-view request merging;
- visual and collision residency budgets;
- stitched coarse-to-fine heightfield edges and hysteresis;
- activation barriers and replicated readiness;
- portals and surface cameras as ordinary view interests.

Gate: a release-build traversal crosses chunk and LOD boundaries without a
crack, simulation hole, duplicate build, unbounded cache, or owner-thread stall.

### Phase 4: dedicated GPU residency and compute

- content-addressed terrain mesh table;
- GPU kernels from the shared intermediate representation;
- resident field cache and compact dirty uploads;
- explicit readback;
- device timestamps, transfer counters, heap counters, and deferred retirement;
- CPU and GPU parity tests.

Gate: moving through a resident region uploads zero terrain geometry bytes and a
device reset rebuilds the same visible terrain without stale handles.

### Phase 5: simulations, layers, and ecosystem outputs

- slope, curvature, flow, and distance layers;
- hydraulic and thermal erosion with secondary layers;
- material and biome composition;
- water, rivers, foliage, rocks, and gameplay placement outputs;
- offline baking and edit-journal compaction.

Gate: tiled erosion is seam-safe, material-only changes reuse upstream fields,
placement ids are stable, and checkpoints bound late-join data.

### Phase 6: sparse SDF terrain

- SDF graph primitives and smooth operations;
- sparse brick demand and edits;
- selected deterministic volume mesher;
- heightfield-to-volume promotion;
- same-LOD and coarse-to-fine seams;
- cave collision, materials, and Studio slices.

Gate: a cave crosses chunk and LOD boundaries with continuous visuals,
collision, normals, and materials while empty regions allocate no bricks.

### Phase 7: planet and distributed bake research

- spherical coordinate graphs and cube-sphere or another measured topology;
- origin and precision strategy;
- trusted distributed offline tile builds;
- package verification and CDN delivery.

Gate: proceed only with measured need. This phase must not replace the graph,
request, scheduler, cache, or artifact contracts proven by flat worlds.

## Verification matrix

### Unit tests

- canonical graph encoding and stable signatures;
- unknown versions, names, units, sockets, cycles, and limits;
- deterministic seed derivation and random-stream independence;
- integer chunk-to-sample coordinate conversion at negative and high values;
- field fusion parity against unfused reference evaluation;
- CPU parity across supported SIMD widths and worker counts;
- edit bounds, chunk invalidation, ordering, compaction, and replay;
- material weight normalisation and tie-breaking;
- empty, solid, flat, one-sample, and maximum-bound inputs;
- checked arithmetic and hostile decoder cases.

### Integration tests

- save and restore of recipe, patches, and edits;
- replicated edit permission, order, checkpoint, and late join;
- cancellation after world stop, graph edit, target destroy, and request replace;
- exact no-op and one-revision changed publication;
- collision broadphase dirtied once per accepted batch;
- process-wide deduplication across worlds and cameras;
- multi-view LOD demand merge;
- bounded cache eviction and deferred device retirement;
- script parity and stable ticket resume order;
- device loss and artifact rebuild.

### Seam and visual tests

- adjacent heightfield edges and halo crops;
- coarse-to-fine stitch patterns and missing-neighbour skirts;
- normals, tangents, material weights, shadows, and physical surfaces;
- SDF brick and hybrid-domain boundaries;
- caves, overhangs, block face groups, and destructive edits;
- high coordinates, negative chunks, mirrored world transforms, and nonuniform
  scale;
- portal, surface-camera, split-screen, and multi-world views.

Golden images complement numeric edge checks, field signatures, and artifact
comparisons. They do not replace them.

### Fuzz and soak tests

- graph and artifact decoder fuzzing;
- random valid graph generation within small bounds;
- random edit sequences with save, compact, load, and replay;
- streaming traversal under tiny cache budgets;
- repeated cancellation and world destruction;
- long-running edit and residency memory slopes.

## Profiling and budgets

Every meaningful stage uses `ENGINE_PROFILE` or a reported worker span. Bytes
and operation counts are recorded at allocation, upload, readback, collision,
and publication boundaries.

Required counters include:

- samples, cells, vertices, triangles, placements, and edits processed;
- compile, queue, sample, simulation, mesh, collision, upload, and publish time;
- cache hits, misses, partial hits, evictions, stale drops, and rejected inputs;
- CPU live, peak, allocated, and scratch bytes by artifact kind;
- GPU live, peak, uploaded, downloaded, and retired bytes;
- active, queued, cancelled, completed, and deduplicated tickets;
- visible, collision-active, prefetched, and resident chunk counts;
- seam fallbacks, skirts, domain promotions, and LOD swaps.

Benchmarks run in `release` and state processor, worker count, backend, graph,
chunk resolution, halo, edit count, and cache state. They cover:

- fused noise and mask samples per second;
- compile and incremental recompile latency;
- heightfield and volume mesh throughput;
- collision artifact production;
- serial versus job crossover for each stage shape;
- one worker through physical cores;
- GPU execution and transfer cost;
- cold, warm, partial-invalidation, and eviction paths;
- end-to-end camera and authoritative-body traversal;
- Studio edit-to-preview latency.

Initial budgets are measured rather than guessed. Before a phase exits, it pins
hard memory ceilings and target latency for representative desktop and server
hardware. Mobile budgets belong in the mobile plan and may select lower LODs or
CPU-only nodes without weakening format safety.

## Open decisions

These require prototypes and release measurements:

1. Independent chunk selection versus clipmaps for large heightfields.
2. The exact L8 terrain dependency set and whether collision artifacts are
   built inside `terrain` or by a higher consumer over a neutral field artifact.
3. Bytecode interpretation versus native template dispatch for fused CPU stages.
4. Generated shaders versus a fixed GPU operation interpreter.
5. Dual contouring, marching cubes, surface nets, or a hybrid volume mesher.
6. The hybrid heightfield and SDF clipping rule.
7. Native heightfield physics shape versus deterministic triangle meshes.
8. Terrain-specific mesh residency versus extending the current mesh table.
9. Persistent cache and baked terrain package formats.
10. Exact erosion algorithms and their bitwise or tolerance parity contracts.
11. Maximum active material weights and the GPU packing chosen from profiles.
12. Edit checkpoint representation and compaction threshold.
13. Flat quadtree chunks versus a later planet topology.
14. Whether trusted servers generate terrain at startup or consume signed baked
    packages for large production worlds.

Every decision record names the tested graph, data set, release preset,
hardware, memory, latency, seam result, and determinism result. A choice is not
accepted because it is fashionable or theoretically faster.

## Completion definition

The terrain system is complete for its first production scope when:

- a saved graph and seed generate the same heightfield on supported headless and
  client builds;
- Studio authors and previews that graph through the shared compiler;
- runtime streaming keeps visual and collision chunks ready within fixed
  budgets;
- edits save, replicate, compact, and invalidate only their affected region;
- collision, visuals, materials, and queries agree at one published revision;
- LOD and chunk transitions have no visible or physical cracks;
- hidden Studio panels and unchanged worlds perform no terrain work;
- all queues, caches, payloads, and script requests are bounded;
- malformed content is refused without partial state or excessive allocation;
- tests, fuzzers, release benchmarks, profiler counters, architecture checks,
  and headless builds pass;
- the Luau box runtime and any other replaced terrain path are removed.
