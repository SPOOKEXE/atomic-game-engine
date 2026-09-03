# Procedural generation plan

## Status

This document defines future procedural generation work. It does not describe a
complete runtime that exists today.

The system provides one deterministic request, scheduling, cache, and
publication contract for generation work. Terrain, animation, VFX, navigation,
audio, materials, and gameplay remain owners of their own data, compilers, and
runtimes. A shared kernel coordinates those domains without becoming a
universal graph virtual machine.

## Product goal

An author can describe a seeded result, preview it in Studio, bake it into
ordinary engine assets, or generate it at runtime. The same recipe and inputs
produce the same canonical result regardless of worker count, traversal order,
or whether the request came from C++, Luau, JavaScript, Studio, or a headless
server.

The system must provide:

- typed recipes and bounded parameter overrides;
- stable, splittable random streams;
- global, spatial, chunk, entity, and time-range request scopes;
- deterministic placement sets with prefab references;
- offline baking and bounded runtime generation;
- authored additions, removals, and modifications over generated output;
- transitive dependency tracking and content-addressed cache keys;
- cancellation, progress, diagnostics, and fixed-barrier publication;
- save, replication, and replay contracts that do not depend on wall time;
- equal Luau and JavaScript access through one engine-owned API;
- Studio authoring and previews that consume the same compiler as runtime;
- strict limits for hostile files, scripts, and network data.

## Design decisions

1. A seed is an input, not hidden mutable state.
2. Random output never depends on worker count or evaluation order.
3. A recipe describes intent. A generated artifact is immutable output.
4. Domain modules own domain semantics and artifact formats.
5. The shared kernel owns request identity, parameters, random streams,
   dependencies, scheduling, progress, caches, and publication envelopes.
6. Runtime never evaluates Studio editor nodes.
7. A graph is optional. Simple generators remain simple typed recipes.
8. Generated gameplay state changes a world only at an authority-approved
   fixed barrier.
9. The ECS owns every saved and replicated fact. Caches own derived data only.
10. Stable names and canonical bytes cross save, process, and network
    boundaries. Process-local numeric ids do not.
11. Authored overrides are sparse operations over stable generated identities.
    They are not a copied generated world.
12. A cancelled or stale request publishes nothing.

## Current foundation

Several existing systems already establish useful rules. This work extends
those rules instead of creating competing paths.

| Existing system | Useful foundation | Boundary to preserve |
|---|---|---|
| `parallel::Jobs` | Measured fork-join range work and batch timing | It blocks until a batch finishes and has one claimed process pool slot. It is not a persistent asynchronous generation queue |
| `script::ComputeJobs` | Owned request data, bounded pending work, ticket-order polling, and nonblocking owner-thread publication | It currently serves typed noise grids. Procedural generation must not turn it into an untyped callback runner |
| `scene::EditableMesh` | Complete geometry transactions, signatures, revisions, worker preparation, and revision-checked commit | It is a publication target, not the canonical recipe or a permanent chunk database |
| `scene::EditableTexture` | Script-built image content with revision-based consumer refresh | It remains the image artifact owner |
| `scene::AnimationBuffer` | Bounded canonical bytes, revisioned mutation, save, replication, and procedural clip baking | Animation generation ends at `AnimationBuffer`; it does not add another clip runtime |
| `scene::Terrain` | One world-owned terrain recipe and stable generator name | Terrain generation remains specified by the terrain plan and terrain module |
| `bake::Graph` | Closed, bounded asset import and transform pipeline | Procedural domain nodes do not enter its closed `NodeKind` set |
| `bakegraph::Document` | Saved edit log, scriptable authoring, stable text, undo, and source resolution | It remains the asset pipeline document and does not become a general graph document |
| `graph::EngineGraph` | Engine and render work scheduling | It is not an author-facing procedural graph |
| Studio `nodegraph` | Typed ports, widgets, links, previews, and editor interaction | It stays client-only UI. Engine modules cannot link it |
| `world::Universe` | World isolation and structural changes at barriers | Generated results enter worlds through the same barrier rule |
| delivery sources | Named local or HTTP fetch and upload routes | Recipes and baked artifacts use delivery rather than adding a second content transport |

The current Studio demo nodes include seeded scalar fields, image previews,
filters, and arithmetic. They are a useful interaction prototype. Their node
types, payload variants, evaluators, and in-memory document are not a runtime
contract.

Editable mesh collision already demonstrates a suitable split. Immutable
sources are read, independent builds run as a fork-joined batch, then complete
collision tables publish once. The generation kernel generalises that shape
without forcing every domain into mesh output.

## Ownership and module boundary

### Shared kernel

A new shared engine module, provisionally `mono.engine/procedural`, owns only
the cross-domain mechanics:

- recipe references and canonical typed parameters;
- request scopes and deterministic request keys;
- random stream derivation;
- dependency manifests and cache keys;
- bounded job admission, cancellation, progress, and completion queues;
- immutable artifact envelopes;
- publication tickets and stale-result checks;
- common validation limits and diagnostics;
- the scripting-neutral service surface.

The exact tier and dependency edges must be selected from the real target graph
when implementation begins. The module must remain below all domain adapters
that use it. It cannot depend on scene, terrain, render, Studio, scripting VMs,
or product executables.

### Domain-owned adapters

Each domain owns its recipe payload, compiler, validation rules, execution
kernels, output schema, and final publication target.

| Domain | Owns | Shared kernel provides |
|---|---|---|
| Terrain | Fields, volumes, edits, meshing, material layers, collision artifacts, and chunk LOD | Stable requests, chunk scopes, seeds, jobs, progress, keys, and publication envelopes |
| Animation | Pose sampling, curves, markers, simplification, and AAN1 encoding | Time-range scopes, seeds, jobs, dependencies, and artifact delivery to `AnimationBuffer` |
| VFX | Spawn distributions, curves, flipbooks, meshes, and emitter recipes | Deterministic streams, bounded batches, baked artifact keys, and preview requests |
| Navigation | Nav tiles, links, cost fields, and cover samples | Spatial requests, dependency invalidation, jobs, and tile publication |
| Materials | Generated textures and parameter tables | Image scopes, dependency keys, jobs, and delivery to editable or baked textures |
| Audio | Generated clips, impulse responses, and control curves | Time-range requests, bounded buffers, cache keys, and baked asset publication |
| General placement | Candidate generation, constraints, prefab selection, and stable placement ids | The complete shared request contract plus scene adapter publication |

The shared kernel does not understand bones, particles, triangles, voxels,
navigation polygons, samples, or shader parameters. It moves typed envelopes
whose domain is named and whose bytes are validated by that domain.

### Product and Studio adapters

The client, server, and Studio own coordination around the engine module:

- a game adapter reads ECS recipe components and submits owned requests;
- a server selects authoritative publication ticks and replication policy;
- a client consumes replicated results or builds approved visual-only results;
- Studio edits recipes, requests previews, applies authored overrides, and
  exports baked assets;
- asset tooling resolves sources and publishes baked results through delivery.

No adapter keeps a second mutable copy of authored recipe state.

## Core data model

### Recipe reference

`ProcedureRecipeRef` identifies a recipe without embedding its full payload in
every component:

```text
ProcedureRecipeRef
    Name             stable asset name
    Domain           stable domain name
    SchemaVersion    domain-owned format version
    ContentDigest    digest of canonical recipe bytes
```

The name is the durable reference. The digest proves which revision was
compiled and forms part of every cache and replay key. The schema version tells
the domain adapter which reader to use. A reader either migrates an older
version explicitly or refuses it. It never guesses.

Recipe assets contain canonical bytes plus a dependency manifest. Human-readable
source may coexist with compiled bytes, but only one canonical representation
feeds the digest.

### Typed parameters

`ProcedureParameters` is a sorted set of named values. Initial value kinds are:

- boolean;
- signed and unsigned integer;
- finite scalar;
- two, three, and four component finite vectors;
- colour;
- transform;
- stable name;
- asset reference;
- bounded byte buffer;
- bounded arrays of one declared element kind.

Every recipe publishes a `ProcedureParameterSchema` with:

- stable parameter name;
- type;
- default value;
- optional numeric range and unit;
- optional allowed stable names;
- whether the value affects topology, appearance, collision, gameplay, or only
  preview quality;
- whether scripts may override it at runtime;
- whether it is safe for client-local visual generation.

Unknown parameters are refused by default. A migration reader may preserve
unknown fields in source documents, but compiled requests never carry values a
generator does not understand.

Parameters serialize in stable text-name order. Floating-point values reject
NaN and infinity before hashing or execution. Negative zero is canonicalized
to positive zero unless a domain proves the distinction matters.

### Request

`ProcedureRequest` is immutable owned data:

```text
ProcedureRequest
    Recipe
    GeneratorKind
    RootSeed
    Scope
    Parameters
    DependencyDigests
    Quality
    Authority
    ExpectedSourceRevision
    RequestSequence
```

`GeneratorKind` is a stable name resolved through the owning domain adapter.
`Quality` is a declared profile such as preview, runtime, or bake. It cannot
silently change topology unless the recipe marks the output as visual-only.

`ExpectedSourceRevision` prevents a slow completion from replacing newer
authored state. `RequestSequence` orders observations for one owner. Neither is
used as random input.

### Scope

The shared scope is a tagged value with bounded fields:

- `Global`, for one complete output;
- `SpatialChunk`, with signed integer coordinates, level, and world-space cell
  size;
- `Bounds`, with a finite world-space box and declared sampling resolution;
- `Entity`, with a stable saved entity reference and local output name;
- `TimeRange`, with integer sample indices and sample rate;
- `Preview`, with an underlying scope plus an explicit preview budget.

Domains may define extra scope payloads inside their canonical recipe format.
They cannot reinterpret common scope fields. One request has exactly one scope.

### Artifact envelope

Successful work returns one or more immutable `ProcedureArtifact` values:

```text
ProcedureArtifact
    GenerationKey
    Domain
    OutputName
    ArtifactKind
    CanonicalBytes
    ContentDigest
    Dependencies
    Bounds
    CostFacts
    DeterminismClass
```

The domain validates and decodes `CanonicalBytes`. The shared kernel stores and
routes it without inspecting its internal layout. `CostFacts` records counts
such as samples, candidates, vertices, keys, or encoded bytes using bounded
named counters.

An artifact is complete or absent. Workers never expose partially filled
buffers to a world, renderer, script, or preview.

## Generator registration

A domain registers a `ProcedureGeneratorDescriptor` during normal engine
startup. It contains:

- stable generator and domain names;
- recipe schema versions accepted;
- parameter and output schemas;
- supported scopes and quality profiles;
- determinism class;
- declared hard limits;
- compile, execute, validate-output, and encode entry points;
- an implementation version used in generation keys.

Registration order has no meaning. Duplicate stable names are startup errors.
Descriptors are frozen before a world starts, so a live request cannot change
meaning while workers hold it.

A generator may compile a domain graph, a short operation list, or one simple
formula. The kernel sees only a validated immutable `ProcedureProgram` owned by
the adapter. It does not provide opcodes, stack values, branching, or a shared
node evaluator.

## Recipe forms

The first release supports two forms:

### Typed generator recipe

A typed generator recipe names one generator and supplies its typed parameters.
It is the default for scatter, noise fields, simple meshes, image synthesis,
and other jobs that do not need a graph.

### Domain graph recipe

A domain graph recipe is owned by its domain. Its compiler resolves stable node
kind names, validates typed ports, rejects cycles where the domain forbids
them, computes dependencies, and emits the domain's immutable program.

Studio may draw all domain graph recipes through shared nodegraph widgets. That
does not make their runtime representation shared. Terrain nodes compile in
terrain, animation nodes compile in animation, and VFX nodes compile in VFX.

There is no general script node in a runtime graph. A domain may expose bounded
built-in expressions or curves when it can validate and price them before
execution.

## Determinism contract

### Stable input tuple

Canonical output is a function of:

```text
recipe digest
generator kind and implementation version
root seed
canonical scope
canonical parameter values
ordered dependency digests
declared platform determinism profile
```

Ticket numbers, entity allocation order, worker ids, thread count, wall clock,
process-local name ids, and hash-map traversal order never enter this tuple.

### Random streams

Randomness uses explicit, splittable streams. A stream key derives from:

```text
root seed
recipe digest
generator stable name
operator stable path
scope identity
stream label
stable item identity
```

The initial algorithm must be fixed, documented, and covered by golden vectors.
A later algorithm receives a new stable algorithm name and version. It does not
silently change old recipes.

Generators request values by stable counter or stable item id. They do not call
a shared mutable `Next()` from parallel loops. Adding an unrelated candidate or
changing grain size therefore cannot shift every later result.

Substreams have named purposes such as `placement.position`,
`placement.rotation`, `placement.variant`, and `material.tint`. Reusing one
stream for unrelated decisions is forbidden because adding one draw would
change all following choices.

### Numeric parity

Each generator declares one determinism class:

- `BitExact`, identical canonical bytes on supported platforms;
- `Quantized`, values quantize to a declared grid before hashing and encoding;
- `ToleranceChecked`, useful for offline visual work whose final artifact is
  canonicalized by the encoder;
- `AuthorityOnly`, built on the server or trusted baker and distributed as
  bytes rather than regenerated by clients.

Gameplay-affecting placement, collision, navigation, markers, and root motion
must be `BitExact` or `Quantized`. A `ToleranceChecked` result cannot steer
simulation.

Reductions use a stable order. Parallel work writes disjoint indexed ranges.
Merge steps sort by stable keys before accumulation or encoding.

## Spatial and chunk generation

### Chunk identity

A spatial generation key contains:

- stable world or region name;
- recipe digest;
- signed integer chunk coordinates;
- unsigned level;
- declared chunk extent in metres;
- output name;
- authored override revision.

World position is derived from integer coordinates. Adjacent chunks do not
accumulate floating-point origins independently.

### Core, halo, and ownership

Generators may sample a halo beyond the core chunk to produce continuous
filters, normals, meshes, roads, rivers, or avoidance fields. The halo size is
declared by the compiled program and included in admission cost.

Only the chunk containing a candidate's canonical anchor owns that candidate.
Halo samples may influence it but cannot publish a duplicate. Candidates on a
boundary use a documented half-open coordinate rule.

Outputs that share edges expose seam metadata or canonical boundary samples.
Seam tests compare adjacent chunks at every supported level.

### Generation waves

Spatial work forms dependency waves:

1. resolve recipe and assets;
2. compile or fetch the immutable program;
3. generate independent base samples;
4. exchange declared boundary or parent data;
5. build domain artifacts;
6. validate and encode;
7. queue a completion for owner-thread publication.

A wave is data, not one thread waiting on another. The scheduler runs ready
work and reports blocked dependencies separately from active work.

### Level changes

Level is an explicit input. Lower detail is not made by skipping arbitrary
candidates from a high-detail run. A domain defines how levels nest and how
stable identities survive a level change.

Visual-only level changes may cross-fade or overlap. Collision and gameplay
switch at one fixed barrier with no interval where two authoritative versions
are active.

## Placement sets

General procedural placement produces a `PlacementSet`, not live Instances.
Each row contains:

```text
Placement
    StableId
    Prefab
    Transform
    Scale
    Tags
    Parameters
    Bounds
    ParentPlacement
```

`StableId` derives from the recipe, scope, generator path, and stable candidate
identity. It does not derive from accepted-row order. Reordering independent
work cannot rename placements.

The initial built-in placement tools are deliberately small:

- regular grid and jittered grid;
- uniform and weighted choice;
- surface scatter;
- Poisson-style minimum-distance filtering;
- slope, height, material, tag, and bounds filters;
- stable density thinning;
- alignment to surface normal or declared axis;
- bounded transform, colour, and scale variation;
- spline sampling supplied by a domain adapter;
- parent-child clusters with declared maximum depth.

Filters operate on candidate records. They cannot reach into a live world from
worker threads. Required query facts are captured into an immutable bounded
snapshot before submission.

### Prefab integration

Placement rows reference stable prefab or package asset names. The
[prefab and package plan](prefab-package-system.md) owns prefab contents,
overrides, dependency updates, and instantiation rules.

The procedural system owns only selection, transform, tags, stable placement
identity, and parameter values. It does not copy a prefab into its cache.

Before publication, the scene adapter resolves every referenced prefab and
validates total instance, component, script, and asset limits. Publication is
atomic for one accepted placement batch. A missing prefab fails or substitutes
a recipe-declared fallback. It never creates a half-instantiated hierarchy.

## Constraints and validation

Constraints are typed, pure predicates or bounded solvers over immutable input.
They declare:

- input data required;
- maximum candidates or iterations;
- spatial neighbourhood radius;
- whether order affects the result;
- deterministic tie-breaking key;
- diagnostic name and failure counts.

The first release includes local constraints only. Examples are bounds, slope,
distance, overlap, clearance, tags, material, and maximum count.

Global optimisation, unconstrained backtracking, and arbitrary script
predicates are refused. A bounded solver that reaches its iteration limit
returns an explicit partial-policy outcome chosen by the recipe: fail, accept
the valid prefix, or emit no placements. It never silently changes policy.

Validation has two stages:

1. parse and validate the recipe into checked owned data;
2. compile checked data into a priced immutable program.

No allocation based on hostile lengths occurs before bounds checks. Execution
does not discover that a graph is cyclic, a port has the wrong type, or a
parameter is outside its declared range.

## Offline bake and runtime generation

### Offline bake

Offline baking is the default for expensive or gameplay-critical output. It:

- resolves all dependencies through the configured content sources;
- runs at declared bake quality;
- records generator, recipe, dependency, and tool versions;
- validates canonical artifacts;
- writes ordinary engine assets through the existing delivery path;
- optionally writes a manifest mapping generation keys to published assets;
- supports cancellation and resumable cache hits;
- fails the batch if a required output is absent.

Baked terrain becomes terrain chunk assets. Baked animation becomes an
`AnimationBuffer` or published animation asset. Baked images become normal
texture assets. Baked meshes use the normal mesh format. A consumer does not
need the procedural runtime to load baked output.

### Runtime generation

Runtime generation is admitted only when its descriptor declares bounded work
for the requested profile. There are three paths:

- tick-bound generation runs a measured fork-joined batch and completes within
  the tick that requested it;
- prepared gameplay generation runs outside world storage, then enters the
  world only through an authority-selected barrier after completion;
- visual-only generation may publish to a presentation cache when ready and
  never changes gameplay state.

Wall-clock completion never decides simulation. For prepared gameplay output,
the authority chooses an activation tick after the artifact is complete. That
event is replicated and recorded. A late client displays a placeholder or the
prior artifact until it receives the canonical bytes. It does not simulate a
different world.

Runtime requests have per-world and per-service limits for pending count,
resident bytes, work units, dependency bytes, output bytes, and publication
count per barrier.

### Bake or generate policy

Recipe owners declare one policy:

- `BakedOnly`;
- `RuntimeAllowed`;
- `RuntimeRequired`;
- `VisualClientAllowed`.

The policy is part of save and publish metadata. A client cannot upgrade an
authority-only recipe to local generation.

## Authored overrides

Generated output remains reproducible while authors retain final control.
Overrides are sparse records keyed by stable generated ids:

- `Remove`, a tombstone for one generated item;
- `Modify`, a typed patch over fields the output schema allows;
- `Replace`, a generated id mapped to one authored object or prefab;
- `Add`, a wholly authored item with its own stable authored id;
- `Pin`, an instruction to preserve a generated item through authoring tools.

An override set records the base recipe digest and override schema version. If
the recipe changes, regeneration reports:

- overrides whose generated ids still resolve;
- orphaned overrides whose targets disappeared;
- conflicts whose target type or patch schema changed;
- new generated items with no override.

Studio never silently deletes an orphan. It offers retarget, convert to
authored addition, or remove. Runtime ignores unresolved overrides with a
bounded diagnostic unless the recipe marks every override as required.

Generated output plus overrides composes in stable id order. The composed view
is the publication artifact. Neither the raw generated set nor the override
set is mutated by composition.

## Dependencies and cache keys

### Dependency manifest

A compiled program declares every source that can affect output:

- recipe and included recipe digests;
- prefab and package digests;
- meshes, images, materials, animations, and audio;
- domain schema and generator implementation versions;
- authored override digest;
- relevant engine format versions;
- explicit external data snapshots.

Dynamic dependency discovery during execution is allowed only through a
bounded domain resolver that appends to the manifest before an artifact can be
accepted. Hidden filesystem reads, environment variables, locale, and current
time are forbidden inputs.

### Generation key

The generation key is a digest over the canonical stable input tuple. It is
independent of display name and ticket sequence. Two requests with identical
keys are interchangeable.

Cache layers are separate:

- compiled program cache, keyed by recipe and compiler version;
- canonical artifact cache, keyed by the generation key;
- consumer derivative caches, keyed by artifact digest plus consumer version;
- Studio preview cache, keyed by generation key plus preview profile and view
  settings.

A render upload, collision shape, navigation tile, or Studio thumbnail is a
consumer derivative. It does not contaminate the canonical artifact key.

### Invalidation and retention

A changed dependency invalidates only keys whose manifest names it. Spatial
adapters additionally map changed bounds to affected scopes.

Caches are bounded by bytes and entry count. They report hits, misses,
evictions, stale completions, and rebuild reasons. Disk caches use atomic
temporary-file publication and verify digest and format before use.

Failures are not cached as successful empty artifacts. A bounded short-lived
negative cache may suppress repeated requests for the same known failure and
must expose the original diagnostic.

## Scheduling, cancellation, and progress

### Scheduler shape

The generation scheduler owns a bounded queue of immutable requests and a
fixed number of cooperative workers. Its lifecycle follows the proven
`ComputeJobs` pattern:

- submission returns a ticket or a synchronous refusal reason;
- workers hold no VM values, ECS rows, renderer handles, or borrowed pointers;
- owner code polls without waiting;
- completions publish in declared order where ordering matters;
- destroying the owner requests stop and joins or reaps every worker;
- worker failures become owned diagnostics.

Long-lived generation workers use cooperative stop tokens. They do not detach.
Domain kernels check stop at declared bounded intervals. A cancelled kernel
releases its partial buffers and returns no artifact.

`parallel::Jobs` remains suitable for measured, joined ranges inside a
tick-bound build. A persistent generation request must not occupy the one
process batch slot while waiting for dependencies or publication.

### Admission

Before entering the queue, a compiled program estimates:

- work units;
- peak temporary bytes;
- output bytes;
- dependency bytes;
- candidate or sample count;
- domain-specific hard counts;
- expected publication cost.

Requests above a hard limit are refused. Soft service budgets determine queue
priority but never weaken hard limits. Priority classes are fixed-tick,
near-field gameplay, near-field visual, Studio interactive, offline bake, and
background prefetch. Starvation limits guarantee eventual progress for
admitted work.

### Cancellation

A cancellation reason is stable and observable:

- user cancelled;
- owner destroyed;
- recipe changed;
- dependency changed;
- scope left the interest set;
- superseded by a newer revision;
- service shutdown;
- budget revoked.

Cancellation is idempotent. Cancelling a completed ticket does not retract an
artifact already published. Superseding a request marks its completion stale
even if the worker cannot stop immediately.

### Progress

Progress is based on completed declared work units, not elapsed time. Each
domain program supplies weighted phases such as resolve, compile, sample,
solve, encode, and validate. Weights are fixed when the request is admitted.

`ProcedureProgress` contains:

- ticket;
- stable phase name;
- completed and total work units;
- completed and total outputs;
- current scope label;
- cache-hit counts;
- bytes read and written;
- cancellation state;
- latest bounded diagnostic.

Workers update atomic counters or append bounded events. Studio and scripts
poll snapshots. They do not lock a worker for progress text.

## Publication and world barriers

Workers return owned artifacts to the adapter that submitted them. The adapter
validates all of these before publication:

- ticket still belongs to the owner;
- recipe, dependency, and override digests still match;
- expected source revision is current;
- artifact kind matches the requested output schema;
- canonical byte and count limits hold;
- domain decoder accepts the artifact;
- authority permits this publication mode.

Accepted gameplay artifacts enter the ECS at one mutation barrier. All rows,
instances, resource revisions, and reverse indexes for one output become
visible together. If any part cannot commit, nothing commits.

Publication produces a stable receipt containing generation key, artifact
digest, owner, scope, accepted revision, and activation tick. Consumers use the
receipt to avoid rebuilding or reapplying the same output.

Visual-only artifacts may enter renderer-owned caches without touching ECS
gameplay facts. They still require digest and revision checks.

## Save format

Saved authored state includes:

- recipe references and canonical recipe source where the world owns it;
- root seeds;
- typed parameter overrides;
- generator policy and quality settings;
- authored override sets;
- stable scope ownership;
- references to baked artifacts and their generation manifests;
- explicit pinned generated output when an author converts it to ordinary
  world content.

Runtime cache entries, pending tickets, worker state, progress, and decoded
consumer resources are not saved.

For a runtime-generated output, the save stores the recipe tuple. For a baked
output, it stores the ordinary asset reference and enough manifest data to
detect staleness. An author may choose to freeze generated output, which
converts it into normal domain data and removes its live recipe binding.

All counts and byte lengths are bounded before allocation. Stable names serialize
as text. Scope coordinates use declared fixed-width integers. Format readers
validate into checked descriptions before constructing ECS state.

## Replication and replay

### Replication modes

The authority selects one mode per output:

- `Recipe`, replicate canonical recipe inputs and let trusted peers regenerate;
- `Artifact`, replicate canonical output bytes;
- `Asset`, replicate a published asset reference and digest;
- `Event`, replicate only a stable activation event for content peers already
  possess.

Gameplay output defaults to `Artifact` or `Asset` until cross-platform
determinism tests prove recipe replication safe. Visual-only output may use
`Recipe` when the descriptor allows it.

A recipe message includes recipe digest, generator version, seed, scope,
parameters, dependency digests, override digest, and activation revision. A
peer compares the final artifact digest. On mismatch it discards the local
artifact, reports the generator and key, and requests authority bytes.

Clients cannot choose authority generation keys, scopes, prefabs, or
activation ticks. Client requests are intent only and pass server permission,
rate, bounds, and ownership checks.

### Replay

A replay records authored input changes and accepted publication receipts. It
does not record worker completion wall time.

For deterministic recipe output, replay verifies the artifact digest. For
authority-only output, replay loads the recorded artifact or asset reference.
The activation tick is recorded so a faster or slower machine changes no
gameplay ordering.

A replay fails loudly when a required generator implementation or dependency
digest is unavailable. It does not substitute the current version.

## Script API

One engine-owned `ProceduralService` exposes equivalent Luau and JavaScript
bindings. The initial surface is deliberately typed and ticket-based:

```text
ProceduralService:GetGenerator(name)
ProceduralService:Validate(recipe, parameters)
ProceduralService:Generate(recipe, options)
ProceduralService:GetProgress(ticket)
ProceduralService:Cancel(ticket)
ProceduralService:GetResult(ticket)
ProceduralService:Bake(recipe, options)
ProceduralService:ApplyOverrides(result, overrides)
ProceduralService:GetDiagnostics(ticket)
```

`Generate` returns a promise or yieldable ticket wrapper according to the
language binding. The VM suspends without blocking the owner thread. Result
buffers are copied or transferred through the existing safe buffer boundary.
No VM object, callback, closure, table view, or pointer reaches a worker.

Scripts may also build domain recipes through domain services. Terrain graph
nodes belong to terrain bindings. Animation procedural nodes belong to
animation bindings. The shared service accepts the resulting recipe reference
and common options.

Script limits include:

- pending tickets per runtime;
- submissions per heartbeat;
- recipe and parameter bytes;
- requested scope volume;
- output and transfer bytes;
- total work units;
- progress and diagnostic event count;
- cancellation frequency.

Errors name the refused field, declared limit, and actual value without leaking
filesystem paths, secrets, process ids, or addresses.

## Studio authoring

### Procedural panel

Studio adds a `Procedural` panel with:

- recipe and generator selection;
- typed parameter inspector;
- seed editing and stable substream inspection;
- scope and bounds controls;
- dependency and cache status;
- preview quality and budget;
- generate, cancel, bake, regenerate, freeze, and clear-cache actions;
- progress phases and work counts;
- diagnostics linked to the owning recipe element;
- authored override inspection and orphan repair;
- artifact cost facts and determinism class.

The panel queries engine state on changes, not every frame. It retains no second
authoritative recipe. Undo records recipe or override edits through the normal
Studio action stack.

### Graph editor

When a domain supplies a graph recipe, Studio uses its existing `nodegraph`
widgets to draw the domain document. The domain supplies node schemas, labels,
ports, widgets, diagnostics, and preview adapters. Studio owns positions,
selection, open state, and other presentation-only data.

The saved graph document belongs to the domain engine module. Studio replays
document edits into that API just as the asset pipeline editor consumes
`bakegraph::Document`. The UI never becomes the only way to construct or save a
recipe.

This work must not merge `bakegraph::Document`, render `EngineGraph`, Studio
`nodegraph::Graph`, terrain programs, animation trees, or VFX programs into one
graph type.

### Preview

Previews use the real domain compiler with an explicit preview profile. They:

- run only while the panel or relevant viewport is visible;
- debounce rapid edits and cancel superseded requests;
- use lower declared bounds, samples, candidates, frames, or particle counts;
- display stale output with a visible stale state until replacement is ready;
- show seed, scope, cache key, and quality;
- never mutate the authored world;
- reuse cache entries across identical requests;
- stop consuming work after the panel closes.

Viewport previews may render generated placement ghosts, terrain patches,
particle bounds, animation poses, or material images through domain adapters.
The shared panel does not interpret artifact bytes.

### Baking workflow

Studio baking performs a preflight before work starts:

- validate every recipe and override;
- resolve required dependencies;
- estimate output count, bytes, and work;
- detect stale or unavailable generators;
- show destination assets and replacement policy;
- require an explicit authority for filesystem or CDN writes.

Progress shows cache hits, current phase, completed outputs, and bytes. Cancel
leaves previously published assets intact and does not publish partial current
outputs. Replacement uses atomic asset publication.

## Security and resource limits

Recipe files, game saves, server artifacts, CDN content, and script requests
are hostile inputs.

Every reader and scheduler must bound:

- recipe bytes, nodes, operations, links, nesting, and included recipes;
- parameter count, array lengths, strings, and buffers;
- dependency count and total dependency bytes;
- scope extent, resolution, sample count, and halo;
- candidates, accepted placements, hierarchy depth, and prefab instances;
- temporary, output, cache, and transfer bytes;
- solver iterations and neighbourhood visits;
- pending requests, progress events, diagnostics, and publication batches;
- decompressed bytes and compression ratio;
- runtime duration through work units and cancellation checkpoints.

Parsing and validation precede compilation. Compilation precedes execution.
Execution writes into owned bounded buffers. Publication decodes and validates
again at the trust boundary.

Runtime recipes cannot execute native code, load dynamic libraries, inspect the
filesystem, access the network, read environment variables, or call arbitrary
scripts. External data arrives only through declared dependencies and trusted
resolvers.

Asset and recipe parsers receive fuzz targets. Determinism and canonicalization
code receives property tests. Diagnostics redact ingest keys, local paths, and
untrusted binary payloads.

## Diagnostics and profiling

Each request reports stable diagnostics with:

- ticket and generation key prefix;
- recipe and generator names;
- scope;
- phase;
- recipe element or parameter when known;
- refusal or failure code;
- bounded human-readable message.

Major branches log submission, refusal, cache hit, cache miss, cancellation,
stale completion, publication, digest mismatch, and fallback to authority
bytes. Repeated per-scope failures are rate limited without hiding counts.

Profiling uses existing engine instrumentation:

- one span for resolve, compile, execute, validate, encode, queue wait, and
  publication;
- reported worker spans after join rather than pretending they ran on the owner
  thread;
- busy, wall, and participant counts for parallel batches;
- input, temporary, output, cache, upload, and download bytes;
- candidate, accepted, rejected, sample, artifact, and publication counts;
- cache hit, miss, eviction, and stale-completion counters;
- queue depth and resident bytes as gauges.

Performance claims use the `release` preset and name generator, recipe, scope,
quality, worker count, cache state, and machine. Crossover thresholds for
parallel work are measured per kernel and recorded beside the dispatch.

## Domain integration

### Terrain

The [terrain plan](terrain-system.md) owns heightfields, signed-distance
volumes, caves, materials, edits, collision, LOD, and streaming. Terrain uses
spatial scopes, halos, dependency manifests, jobs, progress, and publication
receipts from this plan.

Terrain placement outputs may feed general `PlacementSet` artifacts for trees,
rocks, gameplay markers, and water anchors. Terrain geometry does not become a
generic procedural mesh graph.

### Animation

The [character plan](character-system.md) owns `PoseBuffer`, procedural pose
nodes, marker semantics, and baking. A procedural animation request uses a
time-range scope and publishes canonical bytes into the existing
`AnimationBuffer` path.

The generation kernel does not sample an `AnimationTrack`, blend a character,
or own a skeleton. It schedules an animation-owned recorder or baker and caches
its immutable result.

### VFX

The [VFX plan](vfx-system.md) owns emitters, particles, beams, trails, decals,
curves, and render budgets. VFX may use deterministic streams and generated
flipbooks, meshes, spawn distributions, or curves.

Live particle simulation remains in VFX. Procedural generation creates recipes
or baked inputs, not one artifact per simulated frame.

### Navigation and AI

The [navigation and AI plan](navigation-ai-system.md) owns navmesh building,
pathfinding, avoidance, perception, and bot goals. Navigation tiles can use
spatial generation requests and dependency invalidation. AI decisions are not
procedural artifacts and do not run through this scheduler.

### Materials and shaders

The [materials and shaders plan](materials-and-shaders.md) owns material
instances, shader variants, and GPU rules. Procedural image and parameter
generation may publish ordinary material dependencies. Runtime shader
compilation and render graph execution remain outside this system.

## Migration from current experiments

Migration keeps one working path at every step.

1. Keep `script::ComputeJobs` noise grids working while extracting shared owned
   ticket, limit, progress, and cancellation concepts into lower primitives.
2. Add the procedural request and artifact value types with no world consumer.
3. Implement one tiny typed generator with golden deterministic output.
4. Add bounded asynchronous scheduling, polling, cancellation, and cache keys.
5. Route a new script API through that generator in both VMs.
6. Add a general placement adapter that emits data but creates no Instances.
7. Add atomic scene publication for placement sets and sparse overrides.
8. Connect one domain-owned bake path, preferably animation or a simple image,
   to an existing canonical asset type.
9. Connect terrain chunk requests after the terrain compiler exists.
10. Replace Studio demo generation with a domain recipe preview. Keep the demo
    panel until the production panel covers its useful tests, then remove it.
11. Add offline asset publication and manifest validation.
12. Remove any temporary generator path once all callers and saved fixtures
    migrate.

The current `math.noise`-compatible grid API may remain as a convenience if it
is implemented by one typed generator. It must not retain a separate worker
queue after the shared scheduler proves equivalent behaviour and limits.

## Delivery phases and gates

### Phase 0: contracts and measurements

- settle module tier and target graph edges;
- inventory existing async job, revision, digest, asset, save, and replication
  contracts;
- define canonical parameter encoding and generation key fixtures;
- measure representative noise, placement, mesh, image, and animation jobs in
  `release`;
- set first hard limits from measured memory and runtime;
- choose and document the versioned random stream algorithm.

Gate: architecture checks pass, canonical encodings round trip, and random
golden vectors match on every supported platform.

### Phase 1: deterministic kernel

- add recipe references, parameters, scopes, descriptors, requests, artifacts,
  and diagnostics;
- add canonical hashing and random substreams;
- add parse, validation, and compile separation;
- implement one simple typed generator;
- test worker-order and thread-count independence.

Gate: the same request produces byte-identical output under shuffled work
order, inline execution, and every supported worker count.

### Phase 2: scheduler and cache

- add bounded admission and ticket ownership;
- add cooperative cancellation and shutdown;
- add progress snapshots;
- add compiled-program and artifact caches;
- add stale-revision rejection and owner-thread polling;
- instrument queue, work, bytes, and cache behaviour.

Gate: cancellation leaks no thread or buffer, stale work never publishes, and
cache hits perform no domain execution.

### Phase 3: script and placement path

- bind `ProceduralService` equally to Luau and JavaScript;
- add placement records, local constraints, prefab references, and stable ids;
- add authored override composition;
- add atomic scene publication at a mutation barrier;
- add script limits and authority checks.

Gate: a headless test creates, regenerates, overrides, saves, reloads, and
replays one placement set identically through both scripting languages.

### Phase 4: Studio authoring

- add the Procedural panel and typed inspector;
- add visible-only previews, debounce, cancellation, and stale display;
- add domain graph editing through existing nodegraph widgets;
- add undo, seed inspection, override repair, and cost facts;
- add bake preflight and progress.

Gate: headless document tests cover every edit and error path. With user
approval, live Studio inspection verifies layout, cancellation, previews, and
undo without continuous hidden-panel work.

### Phase 5: domain adoption

- connect animation baking;
- connect generated images or VFX inputs;
- connect terrain chunks and placement outputs;
- connect navigation tiles when their domain compiler exists;
- remove superseded experimental worker and preview paths.

Gate: every adopted domain publishes only its existing canonical artifact type,
and disabling the procedural service leaves baked content fully usable.

### Phase 6: delivery, replication, and hardening

- publish baked assets and manifests through delivery;
- add recipe, artifact, asset, and event replication modes;
- add replay receipts and digest verification;
- add parser fuzzing and adversarial limit tests;
- run cross-platform determinism and cache compatibility suites;
- run long cancellation, eviction, streaming, and shutdown soaks.

Gate: hostile inputs remain within declared limits, gameplay replay is
independent of worker completion time, and digest mismatch falls back safely.

## Focused test plan

### Recipe and parameter tests

- every value kind round trips canonically;
- parameter order cannot change a generation key;
- unknown, duplicate, missing, non-finite, and out-of-range values fail with
  the correct field;
- old schema versions migrate explicitly or fail;
- recipe and dependency name ids never enter saved bytes;
- semantically equal canonical recipes have equal digests.

### Random and determinism tests

- golden vectors cover the root and every split operation;
- stream labels isolate unrelated random choices;
- candidate insertion cannot shift existing stable candidates;
- inline, one-worker, and many-worker output is identical;
- shuffled candidate and dependency input order canonicalizes identically;
- adjacent chunks agree at boundaries and halos;
- supported platforms produce the declared determinism class.

### Scheduler tests

- zero work, one unit, below crossover, and many ranges;
- full queue refusal and later admission;
- cancellation before start, during each phase, after completion, and during
  owner destruction;
- worker failure becomes one completion diagnostic;
- stale recipe, dependency, scope, and override revisions publish nothing;
- shutdown joins every worker;
- progress is monotonic and never exceeds total units;
- competing requests obey priority and starvation bounds;
- nested domain work cannot deadlock the process pool.

### Cache tests

- equal keys hit across sessions;
- every key field invalidates when changed;
- unrelated dependency changes do not invalidate;
- corrupt disk entries are refused and rebuilt;
- eviction releases bytes and consumer derivatives;
- a failed build is never returned as an empty success;
- cache hits perform no generator work or output allocation;
- preview and bake quality never collide unless their canonical output is
  declared identical.

### Placement and override tests

- stable ids survive worker count, traversal order, and unrelated candidates;
- boundary candidates have exactly one owning chunk;
- prefab choices, transforms, tags, and parameters round trip;
- overlap and distance constraints use stable tie breaking;
- remove, modify, replace, add, and pin compose in stable order;
- orphan and schema-conflict overrides remain visible after regeneration;
- missing or oversized prefabs cause atomic refusal;
- one failed placement cannot leave a partial hierarchy.

### Save, replication, and replay tests

- authored state survives save and reload without derived cache data;
- frozen output loads without a generator;
- baked manifests detect changed dependencies;
- recipe and artifact replication converge to one digest;
- a client digest mismatch requests authority bytes;
- untrusted clients cannot publish or expand scope;
- replay activation ticks do not change with worker speed;
- missing historical generator versions fail with an exact diagnostic.

### Scripting tests

- Luau and JavaScript expose the same names, defaults, limits, and failures;
- promises or yields never block the owner heartbeat;
- buffers crossing worker boundaries are owned;
- runtime destruction cancels pending tickets;
- rate and byte limits recover after completions clear;
- callbacks, VM objects, and borrowed views cannot enter a request.

### Security tests

- oversized counts fail before allocation;
- recursive includes, graph cycles, and deep placement parents stop at limits;
- decompression bombs stop at declared output bytes;
- malformed artifact bytes fail before scene construction;
- diagnostics do not expose paths, ingest keys, addresses, or binary payloads;
- recipe, artifact, and manifest readers have fuzz targets.

Tests remain focused on contracts. Do not add broad smoke suites whose only
assertion is that generation did not crash.

## Performance gates

Each production generator records release measurements for:

- compile wall time and peak bytes;
- cold and warm generation time;
- worker busy time, wall time, and participants;
- cache lookup and artifact decode time;
- candidate or sample throughput;
- canonical and consumer artifact bytes;
- publication time at the world barrier;
- cancellation latency;
- cache hit rate and eviction churn under a representative movement trace.

Required gates are:

- no parallel dispatch below its measured crossover;
- no world-store access from workers;
- no allocation or execution on a canonical artifact cache hit;
- no hidden Studio preview work while its panel and viewport are invisible;
- no unbounded queue, cache, diagnostic, candidate, or output collection;
- no gameplay outcome based on wall-clock completion;
- no full-world invalidation for a local spatial dependency change;
- no repeated decoding merely to discover artifact facts already retained;
- publication remains within its declared fixed-tick budget.

## Open decisions

These choices need measurements or adjacent system contracts that do not exist
yet. They are decisions for the named delivery phase, not reasons to weaken the
rest of this plan.

| Decision | Current direction | Settle by |
|---|---|---|
| Exact module tier and dependency edges | Put the pure kernel below every domain adapter and add no lateral edge unless the target graph proves it necessary | Phase 0 architecture review |
| Async worker ownership | Generalise a lower parallel primitive only if another engine system needs the same bounded ticket queue. Otherwise keep the queue private to `procedural` | Phase 0 job inventory |
| Random algorithm | Use a counter-based, splittable algorithm with named streams and frozen golden vectors | Phase 0 cross-platform benchmark |
| Canonical digest primitive | Reuse the engine's existing content digest when its byte and text rules satisfy canonical keys | Phase 0 encoding fixtures |
| Persistent cache location and quota | Use the existing engine cache root with per-domain byte quotas and one shared eviction index | Phase 2 cache design |
| First production domain adapters | Prefer animation baking plus generated images because both end in existing bounded artifact types | Phase 1 performance results |
| General scene instance | Add `ProceduralGenerator` only if placement and at least one other scene use need the same inspectable lifecycle. Domain components remain the default | Phase 3 scene adapter design |
| Prepared gameplay activation policy | Authority waits for a complete artifact, selects a future fixed tick, and records it. Define lead time and missing-client behaviour with replication | Phase 6 replication design |
| Historical generator retention | Published games pin generator versions or bake artifacts. Decide release retention length from package and replay storage costs | Phase 6 delivery review |

No open choice permits process-local ids in formats, arbitrary worker callbacks,
unbounded work, partial publication, or gameplay ordered by wall-clock
completion.

## Non-goals

The first complete system does not include:

- a universal graph VM;
- a general-purpose scripting language for generator nodes;
- arbitrary native generator plugins loaded from untrusted games;
- one data model for terrain, animation, VFX, navigation, audio, and materials;
- distributed generation across untrusted player clients;
- nondeterministic machine-learning generation in authoritative simulation;
- infinite unbounded worlds with no residency or work policy;
- global constraint solving or unbounded backtracking;
- automatic conversion of every authored object into a live recipe;
- saving worker queues or cache internals;
- replacing `bakegraph`, render graphs, Studio nodegraph, editable assets, or
  domain-specific runtimes;
- guaranteeing old output after a generator version changes without retaining
  the old implementation or baked artifact.

## Completion definition

The procedural generation foundation is complete when:

- one shared kernel owns stable request, random, dependency, scheduler, cache,
  progress, cancellation, and publication contracts;
- no shared code interprets domain artifact payloads;
- one general placement generator and at least two domain adapters ship through
  the same kernel;
- Luau, JavaScript, Studio, offline tools, and headless runtime produce the same
  canonical outputs;
- seeded output is independent of worker count and traversal order;
- runtime gameplay output publishes only at recorded fixed barriers;
- authored overrides survive regeneration and expose conflicts;
- baked output loads as ordinary engine assets without the generator runtime;
- save, replication, and replay use stable names and verified digests;
- cancellation, shutdown, stale completion, and hostile input paths are tested;
- every queue and cache is bounded and profiled;
- superseded experimental paths are removed;
- architecture, formatting, focused tests, fuzz targets, and release profiling
  gates pass.
