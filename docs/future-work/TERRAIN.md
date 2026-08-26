# Terrain generation plan

## Status

Terrain generation is planned work, not a promise made by the current
`examples/Terrain.luau`. The example remains useful as a correctness and
performance probe. Its procedural loop is not the engine API to preserve.

The immediate engine work is intentionally smaller:

- scripts can submit one complete editable mesh instead of crossing the VM
  boundary once per vertex attribute and triangle;
- geometry is signed before publication, so identical results do not invalidate
  collision or resident GPU buffers;
- preparation runs as a fork-joined worker batch and publication happens at a
  deterministic owner-thread barrier;
- object transforms remain instance data. They never rebuild object-space mesh
  vertices.

A native `HeightAt(x, z)` replacement is deferred. It would make the demo faster
while committing the engine to an interface too narrow for biomes, erosion,
volumes, rivers, masks, material layers, and streamed level of detail.

## Product goal

An author builds a reusable typed graph that can generate a whole world, a
bounded region, or one requested tile. The same graph must support interactive
Studio previews, deterministic server generation, offline baking, and runtime
streaming without maintaining separate implementations.

The system must provide:

- deterministic output from graph content, seed, coordinates, resolution, and
  declared engine version;
- custom graphs, reusable subgraphs, exposed parameters, and named outputs;
- heightfields, scalar and vector fields, masks, materials, meshes, collision,
  and placement sets as first-class values;
- CPU SIMD and GPU compute execution from the same validated graph;
- tile-local invalidation, cancellation, prioritisation, and bounded caches;
- seam-safe LOD and streaming;
- enough diagnostics to explain cache misses, long nodes, seams, and different
  results.

## Lessons from existing tools

The design borrows concepts, not file formats or implementations.

### World Machine

World Machine demonstrates a device graph in which generators, selectors,
modifiers, erosion, and outputs can be composed. Its erosion device exposes
wear, deposition, and flow-related results rather than returning only a final
height map. Its tiled builds use overlap and blending because simulations need
neighbourhood data at tile edges. Build history and cached previews make graph
iteration practical.

Relevant references:

- [Devices and the device workspace](https://help.world-machine.com/topic/devices-and-the-device-workspace/)
- [Erosion device](https://help.world-machine.com/topic/device-erosion/)
- [Tiled worlds](https://help.world-machine.com/topic/world-machine-professional-edition-addendum/)
- [Build history and caching](https://help.world-machine.com/topic/build-4015-mt-rainier-release/)

### World Creator

World Creator makes biomes hierarchical and uses filters to control where
operations contribute. That is a useful authoring model for nested regions such
as climate, mountain family, rock exposure, and vegetation. The underlying
engine representation should still be ordinary named masks and graph edges, so
the hierarchy is a view over data rather than a separate execution system.

Relevant references:

- [Biomes](https://docs.world-creator.com/reference/terrain/biome)
- [Biome filters](https://docs.world-creator.com/reference/terrain/biome/filters)
- [Terrain concepts](https://docs.world-creator.com/walkthrough/terrain-setup/understanding-terrains)

### Houdini heightfields

Houdini models a heightfield as named volume layers. Masks are ordinary layers
that can be combined and reused. Erosion can publish debris, sediment, water,
flow, and flow direction for later nodes. This generalises cleanly beyond a
single height array and should be the model for Atomic terrain fields.

Relevant references:

- [Heightfields](https://www.sidefx.com/docs/houdini/heightfields/index.html)
- [Heightfield masking](https://www.sidefx.com/docs/houdini/heightfields/masking.html)
- [Heightfield erosion](https://www.sidefx.com/docs/houdini/heightfields/erosion.html)

### Gaea

Gaea groups nodes by role: terrain sources, primitives, simulation, surface
operations, modification, derived data, colour, utility, and output. It also
exposes graph parameters and supports baking intermediate results. Atomic should
adopt the discoverability of those families while keeping node identity stable
and execution independent from the editor.

Relevant references:

- [Node families](https://docs.gaea.app/reference/nodes/)
- [Procedural graph workflow](https://docs.gaea.app/ui/interface/graph/procedural-workflow.html)
- [Simulation nodes](https://docs.gaea.app/reference/nodes/simulate/index.html)
- [Derived-data nodes](https://docs.gaea.app/reference/nodes/derive/index.html)

### FastNoise2

FastNoise2 is the useful execution reference. Its node graph has source, hybrid,
and variable inputs, and its generator templates can fuse operations so
intermediate values remain in SIMD registers. Metadata drives creation and
configuration. Atomic needs the same separation between node description and
compiled execution, with CPU lanes selected at runtime and a GPU backend built
from the same validated intermediate representation.

Relevant references:

- [Node graph architecture](https://github.com/Auburn/FastNoise2/wiki/Node-Graph-Architecture)
- [FastNoise2](https://github.com/Auburn/FastNoise2)

## Engine ownership

The runtime belongs under `mono.engine`, split by the existing layer rules.
Studio only owns graph editing, previews, and diagnostics. It must not own node
semantics or generated terrain state.

The proposed responsibilities are:

- `graph`: general DAG validation, stable node and socket names, canonical
  serialisation, and graph editing operations;
- a new shared terrain module: terrain value types, node catalogue, validation,
  deterministic compile options, CPU execution, tile scheduling, cache keys,
  and output recipes;
- render: GPU kernels and resident terrain resources compiled from the shared
  terrain plan;
- scene: terrain component state and references to generated artifacts;
- collision and physics: consume published collision artifacts without knowing
  how they were generated;
- Studio nodegraph: edit and inspect the shared graph model.

Nothing crossing a world boundary is a pointer. A request contains a graph
content name or canonical graph bytes, seed, tile coordinate, resolution, LOD,
and requested outputs. A completion contains owned artifacts and the same
request identity.

## Data model

### Graph identity

Persistent nodes, sockets, parameters, and outputs use stable string names.
Dense numeric handles may be assigned by a compiled graph inside one process.
They are never saved or sent.

A graph signature includes:

- canonical node types and versions;
- canonical parameters, connections, exposed inputs, and named outputs;
- seed and coordinate convention;
- requested tile coordinate, resolution, LOD, and halo;
- execution compatibility version;
- signatures of referenced curves, images, meshes, and subgraphs.

Editor layout, selection, comments, and preview state do not affect output
signatures.

### Value domains

The first implementation should support these explicit value kinds:

- scalar parameter;
- integer, boolean, seed, and enum parameter;
- curve and gradient;
- two-dimensional scalar field;
- two-dimensional vector field;
- named layer set;
- heightfield, defined as dimensions, coordinate transform, height layer, and
  optional named layers;
- three-dimensional scalar field for caves and signed distance fields;
- mesh artifact;
- collision artifact;
- material-weight artifact;
- placement set containing stable ids, transforms, prototypes, and attributes.

Units and coordinate spaces are part of socket types. A world-space metre
field cannot silently connect to an index-space field.

### Named layers

Nodes publish semantically named layers such as:

- height;
- mask;
- slope and curvature;
- water, flow, and flow direction;
- sediment, deposition, debris, and wear;
- temperature, humidity, and biome weights;
- rock, soil, snow, vegetation, and material weights;
- distance and ambient occlusion.

Named layers keep downstream graphs readable and allow one expensive simulation
to feed meshes, materials, foliage, rivers, and gameplay.

## Node catalogue

The initial catalogue should be broad enough that extension does not require
changing the graph type system.

### Coordinates and inputs

- world, tile, local, polar, cylindrical, and spherical coordinates;
- seed derivation and stable random streams;
- constants, exposed parameters, curves, gradients, images, and imported fields;
- coordinate translate, rotate, scale, repeat, mirror, and quantise.

### Noise and patterns

- value, Perlin, simplex, OpenSimplex2, cellular, and white noise;
- fractal Brownian motion, ridged, billow, ping-pong, hybrid, and weighted
  fractals;
- domain warp, curl, turbulence, and nested warp;
- checker, radial, stripe, Voronoi regions, and blue-noise sampling;
- independent frequency, lacunarity, gain, octave, jitter, distance metric,
  and normalisation controls.

### Terrain sources and primitives

- plane, cone, dome, crater, ridge, mountain, dune, terrace, plateau, canyon,
  island, coastline, river path, spline stamp, and mesh stamp;
- analytic and sampled signed distance primitives for volumetric terrain;
- combine by min, max, add, subtract, smooth union, smooth intersection, blend,
  and masked selection.

### Field operators

- arithmetic, min, max, clamp, abs, power, logarithm, bias, gain, remap, and
  normalise;
- curve, terrace, posterise, quantise, blur, sharpen, dilate, erode, distance,
  and resample;
- select, blend, overlay, mask, invert, threshold, and falloff;
- derivatives, gradient, normal, slope, aspect, curvature, laplacian, and flow
  accumulation.

### Simulation

- hydraulic and thermal erosion;
- weathering, talus, sediment transport, and deposition;
- river and drainage solve;
- coastal erosion;
- snow and glacier accumulation;
- cellular and reaction-diffusion operators where they have a terrain use.

Every simulation declares its neighbourhood radius, halo requirement, iteration
count, temporary memory estimate, determinism contract, and secondary outputs.

### Biomes, materials, and placement

- masks from height, slope, aspect, curvature, latitude, climate, water,
  distance, field ranges, shapes, and painted data;
- hierarchical biome groups represented as nested mask composition;
- material weight normalisation and priority rules;
- density fields, exclusion masks, Poisson and blue-noise placement;
- stable prototype choice, scale, rotation, colour, and custom attributes;
- erosion-aware and biome-aware vegetation and rock placement.

### Outputs

- preview field and thumbnail;
- heightfield tile and volume tile;
- render mesh, skirts, seams, normals, tangents, and material weights;
- collision mesh or simplified collision heightfield;
- water and river geometry;
- placement set;
- baked image, layer set, or reusable graph artifact.

## Compilation and execution

### Validation

Compilation first validates the entire graph:

- all node names and versions resolve;
- required sockets are connected;
- value kinds, units, and coordinate spaces match;
- the graph is acyclic except inside an explicit bounded iteration node;
- requested outputs are reachable;
- simulation halos and temporary memory fit configured limits;
- every custom node declares determinism and backend support.

Validation returns structured errors attached to nodes and sockets. Runtime
execution never discovers a type error halfway through a tile.

### Intermediate representation

The validator lowers reachable nodes into a terrain-specific intermediate
representation. The compiler performs constant folding, dead-node removal,
common-subexpression elimination, coordinate transform folding, mask fusion,
and stage formation.

Element-wise chains are fused. A chain such as noise, remap, ridge, mask, and
blend should become one CPU SIMD loop or one GPU kernel, not five full-field
allocations. Simulation and neighbourhood nodes form explicit materialisation
boundaries.

### Backends

The CPU backend uses structure-of-arrays tiles and runtime-selected SIMD widths.
Jobs divide independent tiles, rows, or blocks above measured crossover points.
Small previews run inline to avoid dispatch overhead.

The GPU backend keeps coordinates, fields, masks, meshes, and indirect work
descriptions resident. A graph update uploads compact parameters and dirty
source resources. It does not upload every intermediate field each frame.
Readback happens only for an explicitly requested CPU consumer, diagnostic, or
saved artifact.

Backend parity is defined per node. Exact integer and seed operations must
match. Floating-point nodes declare either bitwise parity or a tested numeric
tolerance. A graph cannot silently switch to a backend with a weaker contract.

### Tiles, halos, and LOD

Each request names an integer tile coordinate and LOD. Shared sample boundaries
derive from world coordinates, never from independently accumulated local
floats. Neighbourhood nodes request halo samples based on their declared radius.

Simulation tiles execute with overlap. Publication crops the halo and can blend
declared transition regions where the algorithm requires it. Tests compare
adjacent edges exactly or within the node's declared tolerance.

LOD generation must preserve shared coarse and fine boundary samples. Mesh
outputs support skirts or stitched index patterns, but cracks are first prevented
at the field sampling contract.

### Scheduling across worlds

Worlds submit immutable generation requests to a shared scheduler. The scheduler
deduplicates identical signatures, sorts ready work by priority and stable
ticket, and runs one process-wide batch. Visible near tiles rank above collision
lookahead, thumbnails, and offline background work.

Generation may run on workers or GPU compute queues. Results never mutate ECS
storage there. At a deterministic world barrier, the owner thread accepts only
results whose target entity generation, request ticket, graph signature, and
expected revision still match. Destroyed, edited, stopped, or superseded work is
dropped.

Lua and JavaScript await a generation ticket. Awaiting suspends that script, not
the world, event loop, renderer, or Studio interface. A separate resume source
is drained in ticket order at the documented heartbeat barrier.

### Cache

The cache key is the full graph and request signature. Cache entries are typed
artifacts, not opaque editor snapshots. Memory and disk caches are bounded by
bytes and expose hit, miss, build, eviction, and rejected-result counters.

Graph edits invalidate only reachable downstream stages. A cached upstream field
may feed several outputs and several worlds. Device-resident entries use deferred
retirement so in-flight command buffers never observe recycled memory.

## Publication and residency

Generated object-space mesh geometry is immutable for a published signature.
The scene component refers to that geometry and carries ordinary instance
transforms separately. Render compute reads the resident object-space geometry
through stable mesh table entries and reads transforms through resident instance
slots. Moving, rotating, or scaling an instance changes only its instance slot.

For editable mesh output, generation uses the bulk transaction:

1. worker preparation validates arrays and computes the content signature;
2. the owner-thread barrier rejects a missing or stale target;
3. exact unchanged content leaves the revision untouched;
4. changed content advances one revision;
5. collision and render consumers rebuild or upload only that revision;
6. transforms never participate in the geometry signature.

Long term, terrain mesh artifacts should enter a content-addressed resident
terrain table directly. EditableMesh remains the scripting and authoring bridge,
not the only storage representation for a continent.

## Studio authoring

The existing nodegraph should grow into a view over the shared graph model. It
needs:

- typed sockets with unit and domain labels;
- searchable node catalogue grouped by the families above;
- inline constants and exposed graph parameters;
- subgraph creation, versioning, and instance overrides;
- named output and layer browser;
- per-node preview, pinned preview, probe, histogram, range, and NaN display;
- 2D field, 3D terrain, volume slice, material, and placement previews;
- cache state and invalidation overlays;
- CPU and GPU timing, allocation, resident bytes, transfer bytes, tile count,
  and queue latency per node and stage;
- tile boundary, halo, LOD, and determinism comparison modes;
- cancel, pause, single-tile rebuild, bake, and export controls;
- undo and redo over canonical graph operations, separate from cache state.

Preview work uses lower resolution and lower priority, but it executes the same
compiled semantics as a final build. A preview-only implementation would hide
the exact seam and parity failures the editor is meant to reveal.

## Customisation

Customisation is data-shaped wherever possible:

- exposed parameters and parameter collections;
- curves, gradients, masks, images, splines, and stamps;
- reusable versioned subgraphs;
- node presets;
- biome templates and material sets;
- declarative placement prototypes.

Native custom nodes register stable names, versions, socket metadata, validation,
cost estimates, CPU implementation, optional GPU kernel generation, and tests.
Script callbacks are not allowed inside per-sample loops. A script may assemble
graphs, submit requests, await results, and author data, but a million samples
must not mean a million VM crossings.

## Delivery stages

### Stage 0: current bridge

- bulk `EditableMesh:SetGeometry` in Luau and JavaScript;
- exact geometry signature and no-op publication;
- deterministic awaited completion;
- collision preparation batched across changed meshes;
- benchmarks for terrain-sized sign and prepare operations.

Exit gate: the terrain example performs one geometry call per chunk, repeated
identical geometry produces no render upload or collision rebuild, and stopping
or replacing a target cancels publication safely.

### Stage 1: graph schema and scalar fields

- stable serialisable graph schema;
- typed sockets and structured validation errors;
- scalar and 2D field values;
- constants, coordinates, arithmetic, select, curve, basic noise, fractals,
  domain warp, and heightfield output;
- canonical signatures and a bounded CPU cache;
- Studio editing and 2D previews.

Exit gate: saved graphs survive node ordering changes, adjacent tiles share exact
edges, and CPU results replay deterministically.

### Stage 2: compiled CPU execution

- terrain intermediate representation;
- dead-node removal, constant folding, common-subexpression elimination, and
  fused element-wise stages;
- runtime-selected SIMD kernels;
- process-wide request batching, priorities, cancellation, and owner-thread
  commit;
- release crossover benchmarks.

Exit gate: a representative noise and mask graph has no per-node full-field
temporary and scales across physical cores without changing results.

### Stage 3: GPU compute and residency

- kernels generated from the same intermediate representation;
- resident field and artifact cache;
- compact dirty parameter uploads;
- explicit readback requests;
- GPU timestamps, transfer counters, heap counters, and deferred retirement;
- CPU versus GPU parity tests.

Exit gate: a camera moving through a stable generated area submits only view and
instance changes, with zero terrain geometry upload bytes.

### Stage 4: simulation and layers

- named layer sets;
- slope, curvature, flow, and distance derivation;
- hydraulic and thermal erosion with secondary outputs;
- tile halos, overlap, and seam tests;
- material and biome mask composition.

Exit gate: erosion output is seam-safe across a tiled build and cached upstream
noise is reused after material-only edits.

### Stage 5: runtime terrain

- clipmap or chunk LOD selection;
- mesh, collision, water, material, and placement outputs;
- visible and lookahead priority policy;
- multi-camera request merging;
- replication-safe artifact identity and deterministic server use.

Exit gate: a multi-camera visual test crosses tile and LOD boundaries without a
crack, duplicate generation, CPU stall, or unbounded cache growth.

### Stage 6: volumes and ecosystem

- 3D scalar and signed distance fields;
- caves, overhangs, volumetric meshing, and volume LOD;
- river networks, vegetation succession, and richer placement graphs;
- offline world baking and distributed tile builds.

This stage extends the value system. It must not require replacing the graph,
scheduler, cache, or publication contracts established earlier.

## Verification matrix

Every stage adds unit, integration, visual, and performance evidence.

Correctness tests include:

- graph canonicalisation and stable signatures;
- type, unit, cycle, and resource-limit refusal;
- deterministic seed derivation and replay;
- CPU backend parity across supported SIMD widths;
- CPU and GPU tolerance contracts;
- adjacent tile, halo crop, LOD boundary, and world-origin precision;
- cancellation after entity destroy, world stop, graph edit, and request replace;
- exact no-op publication and one-revision changed publication;
- process-wide deduplication across scenes and cameras;
- bounded cache and device retirement behavior.

Benchmarks include:

- samples per second for fused noise and mask graphs;
- terrain-sized signature, validation, and mesh publication;
- compile time and incremental recompile time;
- tile latency at preview and runtime sizes;
- scaling from one worker to physical cores;
- GPU execution, transfer bytes, readback bytes, and resident bytes;
- cache hit, partial invalidation, and eviction cost;
- collision and mesh artifact production;
- end-to-end streamed camera traversal in release.

Visual tests include:

- deterministic reference tiles for representative node families;
- tile seams, LOD transitions, skirts, normals, materials, and shadows;
- erosion secondary layers and mask composition;
- multi-camera reuse;
- high coordinates, mirrored transforms, and nonuniform instance scale;
- device reset and cache rebuild.

Golden images complement numeric tests. They do not replace field hashes, edge
comparisons, profiler counters, or memory ceilings.

## Decisions deliberately left open

The following choices require measured prototypes:

- clipmaps versus independently selected chunks;
- bytecode interpretation versus generated C++ for CPU fused stages;
- shader source generation versus a fixed operation VM for GPU stages;
- exact persistent cache file format;
- erosion algorithm set and bitwise versus tolerant parity;
- dual contouring, marching cubes, surface nets, or another volume mesher;
- whether final terrain artifacts share `MeshTable` or use a specialised table.

Each prototype must use representative graphs and release builds. The selected
option is recorded with measured crossover, memory, seam, and determinism data.
