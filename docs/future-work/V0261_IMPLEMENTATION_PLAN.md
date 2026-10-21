# v0.26.1 implementation plan

## Status and scope

This is the approved execution plan for the v0.26.1 work listed in
`NEW_ROADMAP.md`. The Doxygen-published [repository roadmap](../../ROADMAP.md)
provides the main version history. The shared graph module, target layer,
module policy updates, and adapter migrations have user approval. Remaining
implementation and evidence gates are tracked below. No milestone completion
or optimization gains are claimed here.

The planned scope is:

- one shared, bounded graph compiler for common topology and dependency work;
- domain-owned graph schemas, compilers, evaluators, and runtime state;
- C01 authored typed-buffer compute from one GPU pass into another;
- C02 deterministic cadence and fidelity tiers for non-critical work;
- C04 measured CPU command recording and a real multi-queue overlap path;
- C06 bounded procedural placement, regional invalidation, and publication;
- ray and ray-marching work removal across current and newly added systems;
- three measured optimization rounds covering every milestone workstream.

The task is complete only when each item has a real consuming path, accepted evidence, and its migration and fallback contracts. Helpers, demos,
tests, or public declarations alone do not complete a feature. The scope must not be reduced to fit an implementation shortcut.

The v0.26.1 branch is based on the minimal baseline and documentation changes named in the task handoff. The archived
`imagegraph-full-archive-v0.26.0` at `27a50cd2` is reference history only. Do not import its full runtime into this work. `VERSION` is `0.26.1`
at commit `c9dbcedc`.

## Constraints that govern the work

- Put reusable engine systems under `mono.engine` and follow the target graph.
- Durable and cross-world identities use stable text names and copied messages.
- ECS owns saved or replicated facts. Caches contain derived data only.
- Work within a fixed tick joins before the tick ends. No next-tick authority result may depend on machine scheduling or wall time.
- Preserve existing image pixel output, audio summation order, animation track times and events, fixed simulation, and renderer owner-thread contracts.
- Do not invent a physics graph consumer to satisfy a shared-graph milestone.
- Do not add unused scan, sort, reduction, or generic runtime features.
- Keep script VM access symmetric through one engine-owned service.
- Add a `just` job for each new benchmark. Keep measurements in build output; print benchmark results to stdout and do not write benchmark result files.
- Do not modify protected project policy or guide documents except for the exact user-approved module invariant updates listed below. Do not add agent
  credits or generated footers. Do not use em dashes.

## Source audit basis and limits

The source facts below come from the completed repository source audit. `graph` is an L9 module linked with scene, ECS, and parallel code.
`NodeSchema.hpp` and `Execution.hpp` exist, but no external domain consumers were found. Existing `ExecuteEngineGraph` runs serially. The remaining
image graph, bake graph, audio, compute, animation, renderer, procedural, and ray-query observations are the audited facts used to shape this work.

The audit itself is not runtime verification. Its first-slice implementation
evidence is recorded below. Scoped GPU checks are listed in the current
evidence table; no full milestone completion or optimization gains are
claimed. Both baseline commands passed:

- `just imagegraph-bench 3`
- `just spatial-hashgrid-bench 3`

This is partial CPU baseline evidence only, not the complete v0.26.1 baseline
matrix or any optimization pass. Recheck source assumptions and tests before
changing behavior.

## Policy exceptions and approval status

The user approved this plan, the shared graph module and its L2 core-only
dependency, the imagegraph, bakegraph, and audio policy updates, the associated
layer and adapter migrations, and the renderer worker-recording change subject
to its profiling prerequisite. This approval does not waive implementation or
validation gates. Owner-thread profiling must prove a recording cost before
the render policy edit or implementation. The approved renderer change is
limited to isolated private worker recording after that prerequisite, with
public renderer ownership, shutdown, resource publication, joins, and final
composition remaining on the owner thread.

| Module policy | Exact current invariant | Approved change or remaining condition |
|---|---|---|
| Proposed `mono.engine/graphcore/AGENTS.md` | No module policy exists yet. | Approved: add policy for a device-free L2 compiler that links only `Engine::core`. |
| [imagegraph/AGENTS.md](../../mono.engine/imagegraph/AGENTS.md) | “This module links only `Engine::core`.” | Approved: permit `Engine::graphcore` for common bounded topology compilation. |
| [bakegraph/AGENTS.md](../../mono.engine/bakegraph/AGENTS.md) | “`Engine::core` and nothing else” | Approved: permit `Engine::graphcore` for common bounded topology compilation. |
| [audio/AGENTS.md](../../mono.engine/audio/AGENTS.md) | “Do not "unify" them until `graph` has an executor and physics uses it.” | Approved: permit shared schema and topology compilation while retaining audio's sample mixer, clock, and runtime. No physics consumer is proposed. |
| [render/AGENTS.md](../../mono.engine/render/AGENTS.md) | “A frame is recorded by one thread, and that is a contract” and “Revisit when a viewport's *record* is measurable” | Approved subject to profiling prerequisite: permit isolated private worker recording only after owner-thread profiling proves a recording cost. Public renderer ownership, shutdown, resource publication, joins, and final composition remain on the owner thread. |

The concrete proposal is `mono.engine/graphcore` at L2, linking only `Engine::core`. Existing `mono.engine/graph` remains L9 and calls graphcore;
imagegraph, bakegraph, and audio link graphcore directly. Add the proposed graphcore policy and update `mono.tools/architecture/expected_graph.json` and all
transitive closures. These dependency changes and listed module policy updates
are approved. Keep the feature under the engine layer; do not route engine code
through a product executable.

## Intended ownership and migration contracts

### Shared graph compiler

`mono.engine/graphcore` is the proposed shared module at L2. It links only
`Engine::core`; existing `mono.engine/graph` remains L9 and depends on it. Imagegraph, bakegraph, and audio also link graphcore under the policy
exceptions above. Graphcore owns pure, bounded topology compilation: stable named node and port identities, port types and cardinality, directed
dependencies, cycle checks, deterministic topological order, execution waves, immutable compiled topology, and bounded diagnostics. It has no device,
domain payload, evaluator, script VM, frame clock, audio sample clock, or domain runtime state.

`CompileGraph` validates stable names, declarations, port types and cardinality, resolves links into dense edge data, asks `CompileDependencies` for
an order and waves, then publishes one immutable plan. It rejects duplicate or missing names, bad types, unsupported connection counts, cycles, and
limit violations. Rejection preserves the previously installed plan. `CompileDependencies` checks bounded edge endpoints, deduplicates only where
the domain contract allows it, forms waves, and reports a cycle path without partial output.

Topological ordering must preserve authored-sweep behavior where current domains require it. Ready nodes use the declared domain tie-break policy;
they are never unconditionally sorted by stable identity, process-local `Name::Id`, or container insertion order. For example, with dependencies
`0<-3` and `1<-4`, plus sources 2, 3, and 4, the legacy sweep is `2,3,4,0,1`; a minimum-ready heap produces `2,3,0,4,1`. Tests must freeze
image and audio ordering before migration. Execution waves are separate metadata and must not be flattened into an order that changes serial behavior.

Domain-owned adapters retain semantic checks and translate domain errors to and from the common topology representation. They own typed payloads,
schemas, node kinds, values, evaluators, scheduling policy, clock, and runtime state. Existing image pixel behavior and audio summation order remain
unchanged. Audio compilation and workspace routing must not introduce allocation, locks, or blocking on the audio callback. Image compilation, bake
graph validation, audio connection rebuilding, and `EngineGraph` scheduling migrate one adapter at a time. Remove
each replaced walker after its callers use the shared compiler.

Studio's engine adapter stays under `mono.studio/src`. Domain registries and the Studio UI derive node kinds and port declarations from the same
common domain schema; real adapters and editor widgets must consume it. The nodegraph canvas stays client-only UI and has no engine dependency.
Procedural recipes do not enter bake graph's closed node-kind set. Common graph compilation does not create a universal graph VM, common domain
bytecode, or shared runtime clock.

| Proposed contract | Inputs and state | Output and algorithm |
|---|---|---|
| `CompileGraph` | Stable node and port declarations, typed links, bounds, domain tie-break policy. | Validate, resolve names to dense references, call dependency compiler, and publish immutable plan only on success. |
| `CompileDependencies` | Node count, bounded edges, authored ordering policy. | Check endpoints and cycle state, repeatedly sweep ready nodes in the required deterministic order, emit separate waves and cycle diagnostics. |
| Domain adapter | Domain schema and payload, compiled common topology, prior plan. | Retain semantic checks, map diagnostics, build domain plan, install atomically so refusal leaves prior plan. |
| `ValidateComputeBindings` | Reflected shader resources, typed ports, capacity and access declarations. | Refuse mismatched names, types, stride, version, access, alias, size, or workgroup limits before allocation. |
| `ResolveComputeDispatch` | Logical count, local size, device dispatch limits. | Return zero work for empty input; otherwise checked ceiling division and bounded dispatch dimensions. |
| `CadenceDue` / `CommitUpdate` | Fixed tick, tier interval and phase, last tick, max staleness, entity revision, completed result. | Decide due state deterministically; commit valid result and cadence state at the fixed barrier, discard stale output without advancing. |
| `BuildRegionKey` | Canonical recipe identity, sorted relevant source leaves, bounded-query empty proofs. | Build stable regional invalidation key independent of request order, ticket, and revision. |
| `GenerateRegion` | Owned bounded inputs, stable seed tuple, domain schema and spatial facts. | Generate stable candidate IDs with named counter streams, then sort and validate canonical output. |
| `PublishRegion` | Completed region, source revisions, capacity, current region state. | Preflight and atomically replace the region at the barrier, or retain last-good state on stale input or failure. |

### C01 typed compute

Separate logical element count from allocated capacity and storage stride. Define named typed buffer ports with declared element type, stride, logical
count, capacity, access mode, and versioned shader reflection contract. Validate shader bindings, aliases, workgroup dimensions, dispatch limits,
access modes, and byte-size arithmetic before submission. Empty logical input has count zero and a legal bounded sentinel allocation when the backend
needs one. Dispatch uses checked ceiling division.

The first end-to-end consumer is stable GPU compaction: mark or classify, hierarchical scan, scatter, then a GPU consumer that reads the result.
Compaction emits a `u32` device-count buffer in the same GPU dependency plan. The consumer dispatch is bounded by validated output capacity and
guards every write with `index < deviceCount` and `index < capacity`; overflow sets an error and prevents publication. A validated indirect dispatch
may replace this guarded dispatch when added. Write-to-read barriers cover both compacted data and the count buffer. Host logical count remains
unknown until a fence completes and an optional bounded readback returns
it. No CPU count readback may sit between GPU passes. Count zero is legal with the bounded sentinel allocation and no invalid dispatch. Validate empty, odd-sized,
multi-workgroup, multi-level, maximum accepted, and one-over-limit inputs against a CPU oracle. Keep the 64 MiB per-buffer and 256 MiB aggregate
budgets, owner/world/view lifetime checks, and `engineInstanceBuffer` readonly exception in force.

`ValidateComputeBindings` checks reflected names, versions, types, strides, access, aliases, capacity, byte arithmetic, and local sizes before any GPU
resource is created. `ResolveComputeDispatch` checks logical count and device limits, then calculates checked ceiling-divided workgroup counts; count
zero remains a legal empty result without dispatching invalid workgroups.

Add a separate `ComputeBridge` by following the ownership and completion shape of `DataCaptureBridge`. Tickets own copied inputs and identify world,
runtime, and generation. Bound pending jobs, uploads, readbacks, and result bytes. Poll fences without blocking, cancel stale generations, and tear
down safely. Expose one shared compute service to Luau and JavaScript. Provide one paired script example and preserve the fixed VFX shader contract.

### C02 deterministic update cadence

Keep `scene::AdvanceAnimationTracks` on fixed simulation so semantic track time and events continue. Throttle pose evaluation only: retain the latest
semantic sample time, preserve history and bones between due evaluations, and sample current time once when evaluation resumes. Keep player
characters, combat, and projectiles at full fidelity when gameplay requires them.

Represent cadence as explicit deterministic integer tick multiples with stable phases, maximum staleness, hysteresis, and declared owner. Stable
phases derive from persistent identity, never process-local `Name::Id` or traversal order. Keep perception, behaviour, visual pose, and cosmetic
effects as distinct domain state in ECS. Promotion to combat fidelity must refresh the required state before combat consumes it. Do not use wall-clock
adaptive authority or large catch-up steps. Replicas do not advance remote authoritative animation time.

`CadenceDue` reads the fixed tick, tier interval and phase, last committed tick, and maximum staleness to return due/not-due plus the staleness
reason. `CommitUpdate` accepts only a completed due result for the current entity revision, writes domain-owned output at the permitted fixed barrier,
and advances its saved cadence state atomically. A stale result is discarded without moving the cadence clock.

Before changing the dedicated-server or replica animation installation paths, reproduce their reported gaps in real tests. Add idempotent installers
only where the reproduced path and ownership contract call for them. Provide a real cosmetic-effect consumer that preserves age, history, and emission
semantics. Use actual NPC scripts that call `Humanoid.MoveDirection`, raycast perception, and attack deadlines with existing `Animation.luau` rigs in
a town scene. Do not imply a future AI runtime already exists.

### C04 CPU recording and physical queue overlap

Measure complete owner-thread recording first. The initial source audit found
`Renderer.cpp` advertising `UnifiedQueue = true` and reported the vendored SDL
path creating one Vulkan queue. That was the audit state, not a current
capability claim. Queue-family capabilities still need to be established for
the opt-in path. The audit reported an RTX 4090 and
`/proc/driver/nvidia/params` with `RmProfilingAdminOnly=1`; it also recorded
Nsight unavailable at that time. Later capture attempts and their limits are
in the current evidence table below. Tool installation alone does not prove
trace access. See NVIDIA's [performance-counter permissions
note](https://developer.nvidia.com/ERR_NVGPUCTRPERM). No root driver
configuration change is planned.

The C04 goal requires a working private parallel-recording path and measured, vendor-traced physical overlap. Profiling can reject an individual
optimization or justify leaving overlap disabled by default, but serial fallback alone does not fulfill C04. Unavailable queues or trace permissions
leave C04 incomplete or unverified; only an explicit user scope change can waive that evidence. Workers receive immutable copied plans and private
command contexts. The owner still controls public renderer lifecycle, shutdown, shared resource publication, joins, and final ordered composition.
Serial recording remains a working fallback for normal unsupported backends and nodes. SDL documents that an acquired GPU command buffer is used and submitted on
the acquiring thread, which informs the private-context boundary ([SDL API](https://wiki.libsdl.org/SDL3/SDL_AcquireGPUCommandBuffer)). Use existing
`SDL_GetGPUDeviceProperties` and `SDL_GetPointerProperty` access for the private size/version-tagged callback table.

The multi-queue prototype must be a complete engine-private integration on an archived copy of the existing SDL source tree. Leave the vendor
submodule pristine; do not submit an upstream patch. Prefer a project-private, size-and-version-tagged callback table obtained through existing SDL
GPU property calls, with acquire-queue, dependency-token, capability, and timestamp callbacks. Reuse existing bind, draw, and submit entry points. Do
not add exported dynamic API symbols if the callback table serves the contract.

The opt-in SDL Vulkan patch must request queues, key command pools by thread and family, enable concurrent sharing for non-swapchain resources across
the unique queue families, and use queue-aware barrier stages while retaining layout and data-hazard correctness. Select queues at device creation.
Before worker queues use owner-created resources such as textures, gate their graphics preparation with a semaphore. Use bounded binary dependency
tokens with separate fan-out; size wait stages for the actual wait count. Join at final graphics submission and retire resources behind a fence.
Freeze resource generations and forbid cycling or defragmentation while workers record. Public
renderer ownership, `Render` and `Shutdown` affinity, shared resource publication, joins, and final composition stay on the owner thread. Submit only
after every worker records successfully. If a partial submit fails, drain it and fail the frame; do not rerun mutating work serially. If this route
cannot satisfy the contracts, document and review a native engine Vulkan backend design before implementation. A sidecar path alone does not fulfill
C04.

Exercise independent bounded compute and raster work, a transfer path, and a valid dependent consumer through serial and parallel modes. Warm both
modes and compare identical inputs. CPU worker intervals, join time, queue waits, and per-queue device time are separate measures. A GPU vendor trace
showing overlapping device activity is required to claim physical overlap. Backend support, device availability, feature enablement, and observed
overlap are separate results. Vulkan requires explicit semaphore synchronization between queues and leaves hardware queue mapping
implementation-defined ([Vulkan queues guide](https://docs.vulkan.org/guide/latest/queues.html)). A vendor GPU trace is required to claim observed
overlap ([Nsight GPU Trace overview](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-overview.html)).

### C06 procedural placement slice

This milestone implements the forest and village road placement slice from the larger [procedural generation
plan](future-work/procedural-generation.md). It does not claim the entire future procedural foundation is complete. Add only the shared pieces this
slice needs: bounded typed recipes and parameters, canonical bytes, fixed-width seeded streams, bounded work and queries, copied requests, stale
completion rejection, regional dependency keys, and atomic publication. Keep road and placement schemas and compilers domain-owned.

Canonical names and bytes sort by stable name. Reject non-finite values and canonicalize negative zero. Fix RNG algorithms with golden vectors and
named counter streams. Candidate IDs derive from stable global cells, not traversal, ticket, revision, allocation, or worker identity. Use distinct
named streams for position, rotation, variant, and material choices. Sort final results by stable IDs. Use deterministic minimum-distance priority and
a declared half-open ownership rule with a sufficient halo.

Separate canonical recipe inputs from derived digests and spatial source leaves. The low-level kernel emits canonical bytes and accepts validated
opaque digests. The host adapter uses existing `assets::Hasher`, recomputes and checks canonical input digests before enqueue and publication, and
owns digest identity. This avoids moving the full crypto/asset dependency into the low layer and avoids a second hash implementation. Each region key
records the relevant leaf manifest, including proof that a bounded query found no matching leaves. Moving an entrance or spline invalidates the old
and new expanded bounds. An unrelated region must keep byte-identical output and identity. Do not use accepted index, request ticket, or revision as a
seed.

Capture bounded physics and spatial facts on the owner thread. Refuse query or work overflow. ECS owns recipe, source, and override facts. Workers
receive owned copies. Bound admission and completion, cancel stale work, and join or drain safely at teardown. Preflight regional publication and roll
back the whole region on failure so the last good region remains active. Existing `CommitEditableMesh` remains atomic for one mesh; it is not a
whole-batch transaction.

`BuildRegionKey` combines canonical recipe identity with the sorted relevant source-leaf manifest and empty-query proofs. `GenerateRegion` evaluates
copied bounded inputs with stable candidate IDs and named counter streams, then sorts and validates output. `PublishRegion` preflights capacity and
source revisions, then commits one whole region at the fixed barrier or retains the last good region. Stale or overflowed work publishes nothing.

Build the slice through the real Studio editor: preview, visible cancellation, and explicit bake or publish. Both script VMs use the same engine
service. Confine file handling to ordinary mesh artifacts and placement descriptions. Deliver published assets through existing signed publisher and
delivery paths. Load baked artifacts without the generator and compare canonical baked and runtime output. Do not claim public runtime generation
before a real server or client runtime consumer uses it.

### Ray and ray-marching work inventory

Establish a before-and-after inventory and benchmark matrix for each current path. Add any new generation ray paths to the same inventory as they
appear.

The inventory also includes existing callers and helpers, not assumed optimization targets. Profile each complete caller path before changing it.

| Path | Candidate work to measure | Required preservation |
|---|---|---|
| `raytrace.comp` | Hoist projection work and compare squared distances instead of lengths where equivalent. | Screen-depth reflection, linear and hardware depth, orthographic and oblique projection fences. |
| `global-illumination.comp`, `pathtrace.comp` | Remove proven invariant arithmetic. | Preserve RNG sequence and output parity contract. |
| `volume.frag` | Avoid repeated interval work across volume steps where semantics allow. | Existing volume and depth behavior. |
| `weather-clouds.comp`, `weather-shadow.comp`, `environment-clouds.comp` | Profile and remove proven redundant ray work. | Existing weather behavior and certificate paths. |
| `weather-fallback.frag` | Measure fallback cost and retain equivalent sampling. | Fallback correctness. |
| `WeatherCapture.cpp`, `god-rays.frag`, `gravitational-lens.frag` | Measure portal capture and effect ray paths independently. | Capture inputs, projection, and visible result. |
| `mono.engine/spatial/src/Query.cpp`, `DynamicBvh.cpp` | Measure internal-node segment/AABB rejection on long diagonal rays. | CPU query results and BVH traversal correctness. |
| `mono.engine/physics/src/Query.cpp`, `RaycastThroughPortals` | Measure complete portal ray paths. | Portal crossing and query result semantics. |
| `mono.engine/physics/src/ShapeRay.cpp`, `MeshRay` | Measure triangle-bounds rejection and exact triangle intersections. | Nearest hit distance and transformed normal. |
| `mono.engine/physics/src/Characters.cpp` | Profile character ground and camera-obstruction raycasts, including portal traversal where called. | Grounding, camera placement, and ray query semantics. |
| `mono.engine/game/src/WeatherGround.cpp` | Measure weather ground-height queries and index preparation under active weather workloads. | Ground heights and weather impact decisions. |
| `mono.studio/src/Overlay.cpp`, `Projection.cpp` | Profile rays used by picking, drag-plane interaction, and gizmos. | Panel projection, selection, and drag results. |
| `mono.engine/resources/shaders/weather-temporal.comp` | Measure ray reconstruction and reprojection work. | Temporal history and reprojection behavior. |
| `mono.engine/resources/shaders/grid.glsl` | Measure ground-intersection ray work. | Grid intersection and placement behavior. |
| `mono.engine/resources/shaders/surface-shading.glsl` | Measure the refraction ray path. | Surface shading and refraction output. |
| `TownCadence.luau`, `TownCadence.js`, `DataSceneService.cpp` | Measure the new authored perception caller through ordinary `physics::Raycast`, including admission, copied observation and VM work. | Due-only queries beyond three metres, exact target identity, visibility, attack deadlines and paired-VM parity. This caller does not use portal traversal. |
| `mono.client/benchmarks/UpdateCadence.cpp` | Measure the native cadence caller with one or eight perception rays and actual query counts. | Actor exclusion, first-hit visibility, cadence decisions and unchanged query results. |

The shipped C01 classify, scan, scatter and doubling kernels add no ray algorithm. C06 source capture selects copied anchors inside bounded region
queries; no new procedural raycast or ray-march path was found in the shipped compiler, host or editor. Generated geometry uses the existing renderer
paths above. This inventory describes shipped consumers, not arbitrary user-authored compute kernels.

Zero-strength volume noise and segment/AABB rejection are candidates, not accepted improvements. Validate every proposed ray change against projection
contracts, weather certificates, and output or tolerance requirements. Do not accept work removal unless its measured whole-path result improves
without quality or behavior loss.

## Ten execution phases

Each phase produces reviewable evidence. No phase authorizes a policy exception
by itself. User review and the shared graph, module, layer, and migration
approvals are recorded above. The renderer change remains conditional on its
owner-thread profiling prerequisite.

### 1. Classify

- Map each requested item to the owner module, layer, real consumer, and visible or measurable completion proof.
- Mark existing foundations separately from gaps and proposed new contracts.
- Identify which C04 capability or trace checks are unavailable; record those as incomplete evidence, not as a scope waiver.
- Keep C06's full future plan distinct from this milestone's useful slice.

### 2. Discover

- Re-read each touched module's `AGENTS.md`, architecture map, format, documenting, quality, bad-practice, roadmap, and running instructions.
- Confirm CodeGraph and source paths, callers, public headers, tests, script bindings, build targets, and current benchmark jobs.
- Reproduce claimed C02 installation gaps and measure renderer recording before proposing fixes or concurrency.
- Record supported backends, shader reflection limits, queue families, GPU tools, and platform build availability.

### 3. Specify

- Freeze bounded schemas, error behavior, ownership, clocks, and lifecycle for graph topology, typed buffers, cadence, queue work, and region
  generation.
- Define stable serialization and version migration where data crosses disk, process, network, or rename boundaries.
- Define old-path compatibility and exact removal point for every migration.
- Define test scenes, CPU oracle, parity tolerances, profile workloads, and acceptance thresholds before adding optimizations.
- Keep canonical digest inputs and opaque digest bytes in the low-level kernel; use host-side `assets::Hasher` validation. Apply the approved L2 graphcore edges.

### 4. Localize

- Draft the module and target graph changes without broadening domain runtimes.
- Build an ownership and migration table from current producers and consumers.
- Identify protected files and write required module policy proposals separately for user review. Never edit policy files in this phase.
- Define API contracts for Luau and JavaScript from the same engine service.

### 5. Prototype

- Prototype graph compilation against representative image, bake, audio, and engine scheduling topologies, including deterministic failure behavior.
- Prototype one typed GPU buffer producer, compaction chain, and consumer.
- Prototype cadence promotion and demotion in the actual town scenario.
- Prototype recording cost and the smallest complete private multi-queue path on supported Vulkan hardware before committing to its complexity.
- Prototype one seeded forest and village region with edited entrance and road inputs, stale cancellation, and last-good publication.
- Prototype each ray candidate in its real consuming path.

### 6. Stubs and contracts

- Add approved module/build declarations, narrow public types, diagnostics, and focused suites before migrating callers.
- Add shader reflection declarations and bounded buffer contracts.
- Add script bindings and parity coverage for the shared service.
- Add observability for cadence decisions, queue mode, queue waits, generation invalidation, cache hits, and refusal reasons.
- Keep every intermediate commit buildable and retain a usable fallback.

### 7. Implement and migrate

- Implement the shared graph compiler, then move each selected domain adapter one at a time. Preserve domain behavior and delete replaced walkers.
- Implement the end-to-end C01 path and asynchronous bridge.
- Implement C02 state ownership, integer cadence, real consumer, and promotion.
- Implement the complete C04 path after measuring CPU recording cost; leave overlap disabled by default if measurements warrant it.
- Implement C06 region generation, invalidation, Studio workflow, and ordinary artifact delivery.
- Implement accepted ray changes in their owners and remove dead paths.
- Update `VERSION` to 0.26.1 during authorized implementation.

### 8. Validate

- Run focused suites, public-header suites, script binding checks, shader validation, architecture checks, source checks, and applicable sanitizers.
- Verify save/load, replica ownership, stale cancellation, capacity refusal, teardown, and rollback on the relevant system boundaries.
- Build server and headless targets where required. Exercise Windows and macOS compile, package, and serial fallback paths when available.
- Run the real Studio town, compute, procedural, and visual demonstrations.
- Record unavailable hardware or runtime checks as skipped, never passed.

### 9. Establish baseline and optimize in three measured passes

After functional integration, run a separate complete baseline, B0. It is not one of the required three optimization passes. B0 and every pass cover
all milestone workstreams across clock time, latency, memory, and compute. A pass may conclude that a candidate regresses or has no safe gain; record
that result and reject the candidate rather than forcing a change. Keep workloads, inputs, build settings, and parity gates fixed.

**B0, functional baseline.** Measure empty, simple, busy, and complex complete paths. Capture source revision, preset, compiler, hardware, backend,
resolution, scene, and settings. Record samples and p50, p95, p99, and max where enough samples make tails meaningful. Capture CPU and GPU time,
allocation bytes and operations, live and peak bytes, churn, uploads, dispatches, iterations, cache hits, queue waits, and network bytes where relevant.

**Pass 1, algorithm and exact work removal.** Change algorithms or remove proven redundant work one candidate at a time. Re-run actual before/after
workloads and parity gates for every workstream. Accept or reject each candidate from whole-path results; preserve RNG sequence, image output, audio
order, timing, and authoritative behavior.

**Pass 2, retained reuse and allocations.** Measure and change reusable plans, buffers, caches, and allocations. Cover cold and warm paths, memory
residency, churn, cache identity, invalidation, and all workstreams. Re-run parity and complete before/after profiles; reject stale or memory-heavy
reuse even when it lowers a narrow timing.

**Pass 3, scheduling and critical path.** Measure scheduling, overlap, and dependency critical paths. Cover serial and parallel work, queue waits,
worker joins, upload/readback, region publication, and cadence promotion across all workstreams. Re-run parity and the full matrix. Do not infer
physical overlap from command buffer counts or a shorter CPU interval.

Use release builds for shipped-cost claims. Current volume GPU means at 65x65, eight-frame weather means, and BVH runs without rays are insufficient
evidence for 1080p or tail claims. Add benchmarks and `just` jobs for actual ray workloads and each new measured path. Keep results in build output
and print them to stdout. Reject parity or quality regressions unless the user explicitly approves a bound before implementation.

### 10. Review and hand off

- Review architecture edges, public surface area, ownership, bounds, stable names, lifecycle, comments, generated docs, and replacement-path deletion.
- Confirm every new public header has its focused suite, except for documented repository exceptions.
- Run the repository's prescribed checklist and format and documentation jobs.
- Prepare real demo scenes and concrete final visual evidence for user review.
- Report code changes, tests and profiles run, skipped checks, accepted and rejected optimizations, and remaining limitations plainly.
- Update planning documents only where completion evidence makes that edit accurate. Do not claim all of C06's future foundation is complete.

## Acceptance evidence matrix

Every row is required. The named proof is evidence to collect after implementation, not a current result.

| Work item | Real consumer and behavior | Required correctness evidence | Required performance or delivery evidence |
|---|---|---|---|
| Shared graph compiler | Image, bake, audio, and engine scheduling adapters use common bounded topology while retaining domain execution. | Deterministic order and waves; cycles, bad ports, duplicates, cardinality, bounds, failed replacement, and old-plan preservation. | Adapter parity before and after; compile cost and memory for simple and complex graphs. |
| Domain schemas and registries | Image, bake, audio, engine adapters, and Studio widgets consume node and port declarations derived from the same common domain schema. | Registry validation, stable names, type/cardinality rules, source diagnostics, and parity between authoring UI and runtime adapters. | Registration and schema compilation cost; no checks-only or UI-only fulfillment. |
| Domain graph runtimes | Each domain retains its payload, evaluator, clock, and state behind its adapter. | Image output parity; audio sample-order parity; fixed schedule behavior; no Studio dependency in engine. | Whole-domain compile and runtime measurements. |
| C01 typed compute | Authored named typed-buffer compaction feeds a second GPU pass and a GPU consumer. | Reflection, type, access, alias, capacity, dispatch, empty, odd, multi-level, maximum, overflow, and CPU-oracle checks. | End-to-end GPU and CPU cost, dispatch and upload counts, bounded asynchronous readback, and no inter-pass CPU count readback. |
| C02 cadence | Real town NPC perception, behaviour, visual pose, and cosmetic effect run at declared tiers. | Deterministic integer ticks, maximum staleness, promotion before combat, current-time pose refresh, event continuity, replica authority, and reproduced installation gaps. | Tick cost and promotion spike distribution for empty, simple, and crowded scenes. |
| C04 recording and queues | Actual renderer records private worker contexts and overlaps independent compute with raster and transfer work on supported hardware. | Ownership, resource dependencies, preparation and final-join ordering, teardown, working serial fallback, and identical results. | Owner and worker CPU spans, joins, waits, per-queue GPU time, and vendor trace proving physical overlap; unsupported capability or unavailable trace leaves the goal incomplete. |
| C06 procedural | Studio previews, edits, bakes, and publishes forest, roads, village, entrances, and ordinary mesh artifacts through the same service. | RNG goldens, canonical bytes, stable IDs, seam and halo ownership, relevant-leaf invalidation, unchanged-region identity, stale cancellation, rollback, VM parity, and load without generator. | Region build and rebuild latency, memory, work/query counts, cache hits, admission refusal, and full artifact delivery path. |
| Ray and ray-marching optimization | Each changed shader or CPU path runs in its real renderer, weather, portal, or query consumer. | Projection fences, weather certificates, output or tolerance parity, portal correctness, and query correctness. | Simple, busy, empty, and complex scene benchmarks with three rounds and useful tail samples. |
| Integration and portability | Engine, client, server, Studio, and supported fallback targets consume only their owned interfaces. | Architecture graph, public headers, scripts, shaders, lifecycle, save/load, and platform compile/package checks. | Reproducible build and profile metadata; actual runtime checks identified by backend and device. |
| User-facing proof | Reviewable Studio and runtime demonstrations show the finished paths. | Concrete scenes, diagnostics, rollback, and visible or audible correctness reviewed by the user. | Final profile bundle and three-round decisions linked to the exact workload and build. |

## Current baseline checks

These commands record the existing tree's partial CPU baseline before the
implementation slice. They do not validate the full proposed feature.

| Command | Result | Limit |
|---|---|---|
| `just imagegraph-bench 3` | Passed. | CPU baseline only; no GPU checks or optimization gain claimed. |
| `just spatial-hashgrid-bench 3` | Passed. | CPU baseline only; no GPU checks or optimization gain claimed. |
| `cmake --build --preset ci --target test_graph test_imagegraph test_bakegraph test_audio` | No work to do. | Existing build targets only. |
| `ctest --test-dir .cache/build/ci --output-on-failure -R '^(graph|imagegraph|bakegraph|audio)$'` | 4/4 passed in 1.20 s. | Existing behavior only; no new feature tests or GPU/runtime verification. |

## First-slice implementation evidence

- The CI build of `graphcore`, `imagegraph`, and `graph` passed after correcting
  an aggregate default warning.
- Focused tests passed: 3/3 in 0.75 s.
- `just test-architecture` passed with 52 modules, 6 programs, and 38 layered
  modules.
- Server configure and architecture checks passed with 41 modules, 1 program,
  and 32 layered modules.
- The source check ran with no foundational findings. Its report still lists
  unconsumed compute and cadence headers, so the source gate is not complete.
- The Studio adapter unity object compiles in the CI preset. Focused server
  graphcore and imagegraph tests passed: 2/2 in 0.30 s.

## Implemented increments and current validation

These commits and results record partial work. They do not establish milestone
completion or optimization gains.

Earlier source-audit observations in this plan describe the tree at audit time.
The rows below record later, scoped evidence and retain their stated limits.

| Commits | Implemented increment |
|---|---|
| `750761a6`, `9b1dc61f`, `76fb2cc4`, `f06ef94f` | Graph foundation; cadence and pose; typed compute CPU contracts; scalar FIFO and schema adapter. |
| `a96f7618`, `543a3056`, `e47fbc10` | Graph ordering and allocation-free audio routing; bake schemas/runtime; bounded forest/village/roads kernel and canonical named-tag decoder. |
| `03d6da25`, `2deb6af2`, `a257a3c8` | Canonical artifacts and cancellation; portable SDL hooks; strict patch whitespace. |
| `6660000f`, `da1da6d5`, `ac58e4a8`, `c108ee68` | Reflected GPU host; beam cadence; ray baseline; geometry. |
| `fe43101a`, `5842d73d`, `e13582e5`, `e0f43820`, `47209fff` | Canonical procedural job host; runtime identity; both-VM NPC/town; neutral stock compaction; atomic mesh batch. |
| `e7fd7955` | Host animation fixes for server time/weight and replica unposed bones/palette. |
| `8d710d7e`, `7c0b32d5` | JavaScript self-disconnect fix; startup resource ownership. |
| `dd5ec7e1`, `0059e2ae`, `aa32cc9f` | Shared geometry bounds/palette; pure mesh bake; authored splines/overrides and atomic scene update. |
| `87179aae`, `2f924933` | Bounded C01 bridge for both VMs; client/Studio endpoint injection. |
| `368b1d58` | Shared assets PRD1 codec and bounded RGA-to-ordinary `.amesh`/`.aproc` assetc cooker and publisher with exact closure. |
| `d9adfa52`, `ba3fca63`, `47ee63aa` | Atomic native baked scene/capture/provision; neutral service and both-VM baked-reference revision/types/docs; client coordinator and signed unified round trip. |

| Validation | Result and limit |
|---|---|
| C01 real-service benchmark | `just script-compute-bridge-bench` now measures actual Luau and JavaScript requests through the renderer-owned compute service, GPU completion, VM materialization and release. The heap-disabled optimized Vulkan run passed all 16 cold/retained cohorts at 101 internal trials, covering 0, 65, 65,537 and 1,048,576 elements. All 2,424 measured jobs had exact ticket-attributed GPU timestamps, zero dropped queries and full untimed every-word CPU-oracle parity. Cancellation and retirement coverage passed 15 GPU cases with 1,199 assertions. Profiling is private, opt-in and bounded; definition, submission, queue, GPU-command, materialization and cleanup costs remain separate. This is functional benchmark and sample-coverage evidence, not complete B0 or an optimization gain. |
| C06 complete-path latency benchmark | `just procedural-delivery-latency-bench` adds directly timed generation, cooking, signed publication and generator-free scene installation. Each trial builds fresh source stores and recipients; only its paired warm phase reuses the cold delivery cache. All four scene sizes passed 101 cold/warm trial pairs with full mesh/object, canonical identity and cache oracles, producing 48 stage distributions. Warm phases fetched zero source bundle payload bytes. Latency runs disable heap hooks; the existing separate allocation diagnostic schema remains intact. Results stay on stdout. A follow-up run observed no concurrent agent builds and low background CPU use, but the full milestone baseline and all three optimization passes remain incomplete. |
| Refreshed C04 execution evidence | At `ce2958ee`, ordinary optimized runs without C++ heap hooks passed 512 frames for empty and 1/2/8-view recording, serial/parallel modes and 4/8 MiB compute payloads. Every populated cohort passed the full-buffer oracle and recorded 512 completed queue measurements with zero dropped queue queries. A separate Nsight Systems 2026.5.1 capture, with heap hooks enabled, saved `.cache/build/bench/c04-current-parallel.nsys-rep` and its SQLite export. Independent source and correlation-ID analysis excluded owner-prefix work and found compute ranges overlapping actual G-buffer raster packets in 15/16, 16/16 and 15/16 measured frames for 1/2/8 views. The capture changed requested Vulkan 1.0 to 1.1; ranges do not establish simultaneous instruction occupancy or a speedup. Normal desktop GPU work remained active. This refreshes execution and overlap evidence, not the complete quiet baseline or any optimization pass. |
| C04 installed EngineGraph path | The installed renderer projects its validated native schedule through `RenderStageProjection` into `EngineGraph`, caches it in `InstalledPipeline`, and executes it at all three `ViewRecording` sites. The old `RenderGraph` runtime walker and lossy `AppendRenderStage` path are removed. Native scope and wave ordering, resource versions, and history remain unchanged; authored projection retains the legacy authored sweep. Integration state only; no performance claim. |
| C04 render-stage graph tests | Stage projection and engine graph integration passed 284 cases and 18,289 assertions. Log: `.cache/v0261-render-stage-graph-tests.log`. |
| C04 parallel recording tests | Parallel recording passed 49 cases and 2,496 assertions. Log: `.cache/v0261-render-stage-parallel-tests.log`. |
| C04 last-good replacement refusal | After the backend refused replacement, the prior output retained real red pixels and identity. 1 case, 52 assertions. Log: `.cache/v0261-render-stage-last-good-tests.log`. |
| Current headless render validation | `test_render '~[slow]'` passed with explicit Khronos 1.3.275 and synchronization validation through the local cache layer, without root configuration changes. Serial passed 892 cases and 420,052 assertions; parallel passed 892 cases and 420,051 assertions. Both logs contain zero VUID, `SYNC-HAZARD`, or `Validation Error` lines: `.cache/v0261-render-stage-full-validation.log` and `.cache/v0261-render-stage-full-parallel-validation.log`. These are headless non-slow default cases, not all test tiers, platforms, or private hidden WSI coverage; the hidden last-good case has separate evidence above. The UX pre-commit run passed 782 suites with zero failures: `.cache/docs-ux-precommit.log`. |
| Earlier coherent CI build | The selected product and test targets passed across 137 build steps. |
| Earlier scoped CTest | `script`, `scripthost`, `procedural`, `bake`, `examples`: 5/5 passed in 51.98 s. |
| Earlier focused CTest | `script`, `scripthost`: 2/2 passed in 14.84 s. The earlier procedural/assets/bake/assetc run passed 4/4 in 1.29 s. |
| Focused tests | Compute: 339 assertions/19 cases; scripting compute, cadence and catalogue: 390/6; mesh: 182/26; procedural: 3,818/38; script procedural and codec: 2,933/42; bake: 2,338/5; scripthost compute/lifetime: 100/5. All passed. |
| Latest focused validation | Native procedural: 3,596 assertions/33 cases passed, including the previously failing Generate rollback coverage. Neutral service and serial Studio/Game coexistence: 106 assertions/4 cases passed. Signed RGA-to-Cook-to-CDN-to-directory delivery and unbound generator-free install: 171 assertions/1 case passed, including stale input, download edit, misnamed mesh last-good, and retry cases. |
| Bindings and component documentation | Bindings generation/check passed for 3 artifacts. `components-check` passed for 266 components, with 0 missing purposes and no serialized padding leaks. The full 131-script Luau check passed against current bindings, and Studio TypeScript passed. The latest schema data reports 257 components, 20 services, and 120 methods. |
| C06 real forest | The Luau and JavaScript applications each completed 600 headless frames with zero engine or Vulkan messages. Captures were pixel-identical at 960x540 RGBA (SHA-256 `477602fac0de046b93bf8fa6f7703e9f459ca1f2eaddc21a43608689d4a5d4be`, 224,440 nonblack pixels); each run recorded 3 dropped ticks while the host was contended. Region hashes remained `d0f9f5180bdb58fef1ac43c707e77bf578504682b2551fad3a5996c36e2fd418` and `01421dfb1e4684c04fe939d63daaa50da9d2f462d29a2d9e066b9a6c71746cd3`. The existing suite passed 84 assertions/3 cases. This is C06 evidence, separate from C02 service results. |
| C06 benchmark harness | Eight diagnostic scenarios and context oracles passed in a concurrent O0 CI trial. This is functional harness evidence, not timed B0 performance evidence. The earlier dense O0 C02 harness did not finish within its 180-second deadline and had no oracle failure; later optimized 120-tick results are recorded below. |
| Current functional benchmarks | Commits `e9d4890e` and `26f7520b` fixed benchmark validity and added the render catalogue/planning job. The optimized benchmark build passed 511 steps in 51.082 seconds; log: `.cache/build/bench/v0261-functional-b0-build.log`. `just render-catalogue-bench 1 1 bench` passed startup, warmup, schema, and schedule oracles. Image Evaluate (5 rows), bake (5 rows), and audio mixing (15 rows) one-sample, zero-warmup smoke runs exited successfully. Runs were contended and sample counts are too small for latency acceptance. Results go to stdout and function stores, with no output files. Image reference independently composes the graph but shares production Sample/Over/Put helpers, so pixel arithmetic is not independently validated; bake reference shares primitive math and checks orchestration and canonical bytes. |
| Compiler and review status | Benchmark compiler is GCC 13.3.0. The Generate rollback failure is fixed in the current procedural result. The Studio role-mode conflict is fixed; serial Studio/Game coexistence is covered by the passing service tests above. |
| C01 and startup on real Vulkan | With explicit Khronos and synchronization validation, ordinary C01 passed 10 cases/297 assertions with zero validation errors; log: `.cache/build/ci/v0261-scatter-portability-explicit-layer-validation.log`. Native GPU startup passed 2 cases/47 assertions with zero errors; log: `.cache/build/ci/v0261-current-native-startup-validation.log`. Earlier scoped polls are historical. |
| Headless regression | The old SIGSEGV case passed after the JavaScript self-disconnect fix: 16 assertions, 1 case. |
| Isolated SDL sidecar | Opt-out/opt-in transfer-to-compute-to-graphics exact parity and synchronization validation passed cleanly. This separate sidecar result does not establish production integration or physical overlap. |
| AudioGraph heap | Constructor: 12,734,480 bytes and 17 blocks. Legal 1,024-node/1,023-input mutation/rebuild added 0 blocks/bytes. This does not prove full mixer allocation freedom. |
| C04 recording and quiet benchmark | The earlier emitter recording measured 1,000 to 100,000 emitters, each with 6 particle slots, and one changed emitter; step-record self time was 4 to 10 us, draw self about 1 us, and whole render 150 to 223 us unchanged or 323 to 359 us for 10,000 emitters with one change. No dropped spans. The approved RTX 4090/Vulkan 580.173.02 GCC 13.3.0/O3 quiet benchmark used 128² scenes with 1,024 actual meshes per view, 11 cohorts, 16 warmups and 16 measured samples. Whole Render was 0.511 to 0.567 ms for 1 view, 0.915 to 1.029 ms for 2, and 3.446 to 3.721 ms for 8. `drawSlots` self time was 0.264 to 0.287, 0.531 to 0.559, and 2.104 to 2.204 ms respectively. There were no dirty rows, uploads, allocation churn, or CPU drops. Eight-view GPU marks were 296, so GPU timing is partial. The profiling prerequisite is met and the previously approved renderer worker implementation may proceed; these results do not establish worker benefit or physical overlap. |
| Earlier C01 native GPU benchmark | Six cases passed exact CPU checks. GPU timing was unavailable in that run and its mean/tail metrics were partial; this is historical evidence, not the current trace result below. |
| Current strict builds | The corrected CI build passed in 172 steps (44.239 seconds); log: `.cache/build/ci/v0261-thread-portability-build-corrected.log`. Formatting checked 2,632 files with no changes. Earlier full dev (1,212 steps), strict CI (598 steps), and headless server (238 steps) builds passed, but predate later source changes. The Windows runner cross build passed in 122 steps; its initial target-list attempt stopped before compilation because `benchrunner` is absent from that preset. A later strict incremental build passed in 3 steps (6.624 seconds); log: `.cache/build/ci/v0261-gate-corrections-build-corrected.log`. The bounded client startup-helper fixture at `8445301e` built in 192 steps/31.974 seconds and passed its targeted suite, recorded below. These are scoped build results, not a completed platform gate. |
| Earlier full dev CTest | Full dev CTest passed 57/57 suites with zero failures in 483.20 seconds before the runner fix; log: `.cache/build/dev/v0261-checklist-final-ctest.log`. No full dev CTest rerun after that fix is claimed. Focused post-fix `testrunner` CTest passed 1/1 in 0.04 seconds; log: `.cache/build/dev/v0261-runner-isolation-ctest.log`. The corrected full pre-push gate later passed all 767 suites; see the runner process isolation row. |
| Earlier full server CTest | Full server-preset CTest passed 46/46 in 526.90 seconds before the runner fix; log: `.cache/build/server/v0261-checklist-ctest-corrections.log`. Strict-CI `test_server` passed with 3,848 assertions in 107 cases before that fix; log: `.cache/build/ci/v0261-full-server-corrections-test.log`. Focused post-fix `testrunner` CTest passed 1/1 in 0.05 seconds; log: `.cache/build/server/v0261-runner-isolation-ctest.log`. The later full pre-push result is recorded separately. |
| Server determinism | The server produced byte-identical results for 512 entities over 200 ticks. Replay over 256 entities and 120 barriers was also byte-identical. |
| C01 and C04 serial/parallel suites | The original headless selection passed 17 serial cases/1,521 assertions and 18 parallel cases/1,520 assertions, with zero validation messages or skips after retirement fixes `02d6e013` and `ca424e27`. Real X11 WSI results are recorded separately below. |
| Current focused CTest | `graphcore`, `graph`, `examples`, and `audio` each passed the latest focused CTest run (4/4 in 44.98 seconds); log: `.cache/build/ci/v0261-current-schema-domain-ctest.log`. Startup and identity tests passed 12 cases/96 assertions. The earlier Studio MCP fixture failures were corrected by `825ad0bb`, whose focused normal and factory suites passed 505 assertions/2 cases. |
| Studio headless interaction | Procedural and command suites passed with 11,933 assertions/45 cases, including a real ImGui click. Commits `9693ddf3` (undo) and `2c6a281b` (panel) are present. Current approved live scenarios are recorded below with their stated scope. |
| Later focused source fixes | `81a0ff07` feedback passed 47 cases/10,594 assertions; `5dffb6bb` zero-byte audit guard passed 15 cases/21 assertions; `397df3ac` exact-default codecs passed 39 cases/3,685 assertions. `baba825d` replica exclusions and MCP fixtures passed client classification (48 cases/845 assertions), client MCP (13/1,281), server MCP (4/265), launcher MCP (2/32), and CDN MCP (1/106). `973c6d41` corrected compositor fixture channels and copy counts. Later commits `4f2d73ab`, `55d88e9e`, and `649780c3` added the acyclic prefix diagnostic, real render schedule/schema and Studio common-declaration integration, and audio routing schemas; full-capacity callback routing retains zero-allocation coverage. `dcf3827d` replaced `jthread` with joined C++11 workers. `879224c3` kept private headers byte-identical; source check reported 603 public headers with zero open findings. Log: `.cache/build/ci/v0261-private-header-sourcecheck.log`. |
| Current startup and crowd evidence | Commit `3b1cd3a2` fixed Town physics startup; both actual Town VMs passed 600-frame headless runs with zero errors. A separate focused startup test passed 2 cases/171 assertions. Commit `33d34edf` includes the crowded source, benchmark, and paired-VM test; the 64-NPC case passed with one store per VM and 693,311 assertions/1 case. All 12 TownCadence workloads passed functionally: 24 configuration rows, 144 window rows, zero owner-frame drops, heap drops, or promotion-latency ticks, and 128 ticks per promotion trigger. Four crowded workloads with 64 NPCs per VM passed both VMs exact oracles. Runs were contended, so no quiet timing acceptance is claimed. |
| Selected checklist sequence | Architecture passed with 54 modules, 6 programs, 40 layered modules, and 6 fixtures. Source check passed with 13 fixtures plus scan; bindings passed for 3 artifacts; component check passed for 266 components with no padding leaks; 131 Luau scripts and Studio TypeScript 7.0.2 passed; editor Luau LSP, 50 docs pages, clang-format 21.1.8, and em-dash checks passed. Other required gates remain open. |
| Build-profile and test-runner tools | Ten build-profile fixtures and the linker, compiler-cache, and nodegraph checks passed. The Ninja orphan check reported zero dead outputs. `testrunner --list` reported 767 suites including graphcore, computegraph, procedural, cadence, beam, windowed-queue, and `engine.render.texturecancellationgpu` suites. |
| SDL revision metadata | Commit `e422a953` derives the prepared SDL cache revision from the pinned source and patch fingerprint. After an engine-only HEAD change, SDL built no targets and generated-header bytes and mtime remained unchanged. Proof: `.cache/build/ci/v0261-sdl-revision-proof.json`. This is a no-op rebuild result, not a broad build-time gain claim. |
| Runner process isolation | Source commit `cdfc578c` and workflow commit `88ee20dd` add a release-only native process check using `test_testrunner` and `testrunner` from `release-tests`; that preset disables benchmarks. The dedicated process-worker fixture passed 20 repeats, each with 6 cases/149 assertions. An earlier full pre-push gate for `ea34b822` passed all 767 enabled suites; the default gate excludes `[gpu]` and hidden tests, which have separate evidence. A later full-hook run is recorded below. |
| Windows runner process isolation | The corrected target-list Windows cross build passed in 122 steps; Wine 11.0 headless process tests passed 6 cases/149 assertions; log: `.cache/build/windows-cross/v0261-runner-isolation-wine.log`. Native MSVC and macOS await another full push gate and manual workflow. The latest native workflow result is recorded below. |
| Manual client startup | The headless client opened its listener in 14.435 seconds and exited cleanly at 22.764 seconds. Log: `.cache/build/ci/v0261-client-manual-startup.log`. This is a startup correctness observation, not a performance claim. |
| Earlier full-hook record | Gate at `ca68a47e` passed 769/770 suites; the single failure was `client.mcpcontract`, where all 3 actual MCP cases failed to connect with `asio.system:111`. Physics passed in 338.69 seconds. Total gate time was 471.363 seconds. Log: `.cache/build/ci/v0261-live-final-pre-push.log`. The final full-gate result is reported separately in its build log. |
| Client MCP startup fixture | Commit `8445301e` bounds startup-helper polling so children that exit early are observed. The targeted test passed 3 cases/695 assertions in 6.258 seconds; log: `.cache/build/ci/v0261-client-mcp-startup-tests.log`. Build log: `.cache/build/ci/v0261-client-mcp-startup-build.log`. |
| Native platform workflow | Earlier workflow `38956827990` at head `e5c2419673ca2357a956404598fec69d3645e75d` passed Linux, Windows, and macOS release build, package, and runner process-isolation jobs; publish was skipped. The final native workflow result is reported separately in GitHub Actions. |
| Shader vendor revision environment | A hook-inherited `GIT_DIR` caused `git describe` to report the engine revision instead of vendor revisions for shaderc, SPIRV-Tools, and glslang. Commit `39095d6d` removes that environment from the shader check. Controlled script verification with a temporary checking stub showed the old hook returned 1 with incorrect vendor identities; the fixed hook returned 0, reported each vendor's own pinned commit, and removed `GIT_DIR`. The deletion-only hook also returned 0 without running the checker. An ordinary engine HEAD-only change left `glslc` a no-op in 0.267 seconds with identical generated metadata, compiler bytes, and mtime; incorrect metadata occurred only under the hook Git environment. This verifies the hook behavior, not the full gate. |
| Corrected regression fixtures | Commits `365c920d`, `5170ba4e`, and `a5be9da3` fixed the Magic fixture tick preparation, made Forest cameras optional for generator-only VMs, and replaced the remaining `jthread` use. Focused validation passed: SceneTick 29 cases/18,441 assertions, ProceduralService 4/106, camera companions 1/358, and `testrunner` 76/496. Logs: `.cache/build/ci/v0261-gate-corrected-{client-scene,procedural-service,camera-companions,testrunner}.log`. |
| Shader gate | The initial gate failed on one of 93 shaders because the compiler emits `_packed` for the reserved MSL identifier `packed`. Commit `2e259c5f` renamed only the GLSL local `packed` to `compacted`; block/port `Packed` and stock-buffer `packed` names remain unchanged. A 4-step rebuild passed, then all 93 shader checks passed. This is not native macOS runtime proof. |
| Canonical ordinary Vulkan and ray/weather diagnostics | The ordinary-path ray, weather, volume, and hard-render selection passed 22 cases with 922,465 assertions and zero validation messages or skips after the sampler-barrier correctness patch and HardRender set-2 and mirror BGRA fixture corrections. The prior reflected-cloud and hard-render failures are resolved in this tested selection. Actual WSI results are recorded separately below. Weather passed all four scenes at 1080p for 512 frames with strict reuse. Empty recorded its maintenance event at frame 14, tick 0, then passed 512 strict frames with zero rebuilds, clock writes, or page uploads. Ray passed 36 cohorts at 1080p for 512 frames with all correctness oracles passing. Its run started at 00:32:52 ACST while unrelated GPU work was active, so its timings are rejected for quiet B0 acceptance. |
| SDL cancellation source | Commit `69d1a5f1` contains the 8-file source fix; metadata commit `23743b83` adds the exact patch-context whitespace exception. Strict `git apply --whitespace=error` remains enforced. |
| Current texture-cancellation and render validation | Commit `c06c7b67` fixed SDL defragmentation through the real allocator fixture. Before the fix, 123 assertions passed with 7 `InvalidImageLayout` errors; after it, 123 assertions passed with zero errors in both ON and OFF configurations. Logs: `.cache/build/ci/v0261-texture-defrag-{before,after}-validation.log` and `.cache/build/ci/v0261-current-off-defrag-only-validation.log`. The current full render binary reported 883 cases: 875 passed and 8 expected private-capability skips, with 417,138 assertions and zero Vulkan messages; log: `.cache/build/ci/v0261-current-all-render-binary-validation.log`. Shared-build headless private filters remain valid: 25 cases total, 18 passed and 7 windowed opt-in skips; serial/parallel runs had 1,566/1,565 assertions and zero errors. Only the earlier shared-build real-WSI attempt had invalid parallel skips after an external branch switch. See the isolated X11 WSI results below. |
| Isolated real X11 WSI and preview lifetime | In the dedicated validation worktree, real X11 serial WSI passed 7 cases/651 assertions in 90.959 seconds, and parallel WSI passed 7 cases/651 assertions in 62.189 seconds. Both had zero skips and zero Vulkan messages. Preview-lifetime fix `b61b3a68` built in 36 steps (21.981 seconds), log: `.cache/build/ci/v0261-live-lifetime-thread-fixes-build.log`; a later fixture-only incremental build passed in 3 steps (5.024 seconds). After correcting a test-only font-atlas `GetTexID` inspection to use raw image IDs, the X11 fixture passed 6 cases/244 assertions with zero Vulkan messages in 2.711 seconds. That fixture correction did not change production code. An initial dummy-driver invocation failed because that driver lacks Vulkan. The original checkout remains on `v0.25.0-fixes`; the isolated worktree protects this validation branch. |
| Text, wheel, and control checks | The text/wheel fix passed 1 case/77 assertions. Tools/control passed 17 cases/189 assertions. Live scrolling was observed; no one-notch pixel oracle is claimed. |
| Authored world and script fixes | Commits `44c7a3d3`, `b05fd52c`, `d8b54669`, and `93a4f55f` add PWH2 serialization/preflight and atomic failed-save behavior, homogeneous VM/world JS or Luau selection with mixed-language refusal, click hover/down/release automation, and bounded JS stack entry with worker handoff. Focused game tests passed 9 cases/431 assertions; Studio language/automation passed 4/193 before test-config isolation, so the current full gate must rerun. The old click-route test failed 14 assertions including a missed Tab; the corrected route passed 2 cases/130 assertions. JS thread handoff passed 2 cases/60 assertions with zero skips; tests cover two pinned-worker jobs and two distinct async workers while preserving the recursion bound. |
| Live Studio edit and bake checks | Commits `5d2a33e0`, `51d10bdd`, and `406bd9f2` carry the WSI sentinel, schema, and text/wheel updates. In content isolated with bubblewrap `--dev-bind /dev /dev`, seed 42 activated near/far previews at 34/19 placements; repeating the far preview kept all 17 files byte-identical. SourceEntrance westgate Z8 to Z9 refused a stale bake, as shown in screenshot 26. Activation changed near `.aproc` SHA-256 from `893849ab71a5c3790989fb6685e9fbd08323c78939b126eea1257f4f464d3540` to `5073c24793b5ad0a63b70b50f928c3e6b10a3226bed11dac522eb0e80b4496bf`; undo restored baseline, redo restored the changed output, and far files stayed at baseline. Roadwest road-control changed ready placements from 34 to 36; undo and rebake restored baseline exactly. |
| Preview cancellation and signed delivery | Canceling the visible preview produced matching 480x480 RGBA crops at `(8,439,488,919)` before (screenshot 56) and after (screenshot 57), SHA-256 `98489e4f1869c107cb955a579dc831610c4975d4dbacd3a515d3968569d16b9b`. Actual bake and signed publish produced 44 assets in one bundle under root prefix `1d74b34d`; scratch-catalogue delivery verified and offered 41 meshes. Manifest SHA-256: `8b26f9e9347d3ff35576ad5ece9588009298f98dfa9370933032ef72f1854b7b`. |
| Latest live feature checks | Both Luau and JavaScript BufferCompute VMs reached `BufferComputeComplete`. Both Forest VMs produced 2 EditableMesh and 2 MeshPart outputs with matching 36/19 canonical regions. Both Town VMs reached 40 entities with North/South guards; separate animation-track polls matched at 23.016487 and 29.166393. All six VMs stopped and restored. The full live run had zero engine errors and zero Vulkan messages. Log: `.cache/build/ci/v0261-live-studio/studio-lifetime-worker-final-validation.log`; screenshots 69-81 are in the same directory. |
| Save-as and File Open | The 125-byte MCP text document used Save As to write exactly `verification-source.agame`, with no duplicate. The 1,193 bytes refer to the decoded `ProceduralAuthority` block, not the whole saved world; its hash is `c2db60237fa3c8f929aa1ef183a716955d841f97b9190013a3078bbbdf11797b`. The saved world contains 9 worlds. After startup and two live File Opens, all 9 worlds loaded. The main File Open survived with an active preview. ProceduralGeneration window 31458943 was detached; its orbit value refreshed from 0.600 to 2.579, then File Open from its own modal window 31458945 succeeded. Re-preview after reopen produced pixel-identical 480x480 RGBA crops at `(8,439,488,919)` in screenshots 72 and 75, SHA-256 `98489e4f1869c107cb955a579dc831610c4975d4dbacd3a515d3968569d16b9b`. The live run exited cleanly with zero engine errors or Vulkan messages. |
| Signed catalogue delivery | The signed catalogue load produced 44 assets, and 41 meshes were offered. |
| Ordinary compute fallback | The OFF build passed in 2,006 steps. The ordinary compute fallback passed 10 cases/293 assertions with zero Vulkan messages or skips. |
| C01 explicit-layer validation | With explicit Khronos and synchronization validation enabled, ordinary C01 passed 10 cases/297 assertions with zero validation errors. Log: `.cache/build/ci/v0261-scatter-portability-explicit-layer-validation.log`. |
| OFF queue fallback | Requested serial fallback passed 13 cases/426 assertions with zero validation errors or skips. Log: `.cache/build/ci-sdl-queues-off/v0261-texture-cancel-final-fallback-validation.log`. |
| Earlier renderer submit-refusal validation | Before the SDL cancellation fix, the direct test passed 2 cases/106 assertions but reported two `InvalidImageLayout` validation failures. Log: `.cache/build/ci/v0261-texture-cancel-before-explicit-layer-validation.log`. The broader earlier render selection reported 878 cases: 871 passed, 7 expected private-capability skips, and one `InvalidImageLayout` in forwarded-images submit-refusal recovery. Log: `.cache/build/ci/v0261-full-render-named-validation.log`. |
| SDL cancellation failure-path evidence | First-party tests force an owner-observed `Cancel` refusal after native cancellation succeeds, then cover successful drain and refusal. They do not exercise native `SDL_Cancel` returning false. Native begin, fence, OOM, and timeout failure branches were source-reviewed only, not executed. Repair discards user commands and replays only fresh-backing barriers. Faulted, unproven commands refuse future blocking wait, acquire, or submit; teardown abandons unsafe objects. Waits entered before a fault are not retroactively bounded. |
| ASan and UBSan suites | The refreshed custom headless build passed in 29 steps, then `graphcore`, `graph`, and `testrunner` CTest passed 3/3 in 2.06 seconds (11.519 seconds total build and test time). Logs: `.cache/build/asan-v0261/v0261-current-graph-schema-{build,ctest}.log`. This cache did not run client, audio, or GPU sanitizer paths. Earlier scoped runs passed `graphcore`, `computegraph`, and `procedural` 3/3, and after the ECS zero-length `memcmp` correction passed `ecs`, `scene`, and `script` 3/3 in 37.86 seconds; log: `.cache/build/asan-v0261/v0261-corrections-ctest.log`. |
| C02 native and town timing | Seven native cohorts and eight town cohorts each passed 600 ticks with 120 internal warmup ticks. The runner used `--warmup-samples 0` and reported that actual CLI setting; cohorts were not repeated for harness warmup. The native run started at 00:26:48 ACST and provides 497 steady measured ticks per cohort with useful tails. Critical windows have only 30 samples and remain insufficient for firm tail claims. Native classes cover empty, simple (16 actors, 4 bones, 8 keys, 128 obstacles), busy (128 actors, 4 bones, 8 keys, 1,024 obstacles), and complex (128 actors, 64 bones, 32 keys, 2,048 obstacles), with full and authored variants for populated classes. Town simple is one world with 2 NPCs; complex is 32 independent worlds with 64 NPCs total per VM, not one crowded town. All town cohorts ran from 00:28:56.202 through 00:29:03.262 ACST, before the model processes started at 00:29:54. Native complex full p50 was 4.739151 ms and authored p50 was 2.827348 ms, a feature-baseline comparison rather than an optimization-pass claim. Luau allocator figures use requested bytes/blocks and individual-window peaks summed; QuickJS reports charged malloc live bytes/count and has no peak or growth measure. These allocator bases are not comparable. Later TownCadence scripts changed the per-NPC fields, so quiet C02 comparisons must be refreshed before accepting pass-to-pass comparisons. |
| C02 promotion workloads | Seven native and eight paired-town cases passed with 128 entries per trigger and zero promotion latency. Town physics startup passed in both actual VMs. Timings are not accepted as quiet performance evidence. |
| C02 heap-mode and workload validation | `CadenceMeasurements` supports timing with C++ heap hooks disabled; its seven heap fields are reported unavailable in that mode. Four recipes use `memory=0` by default and support `memory=1`. Native and town regular matrices passed in both modes across 7/12 configurations and 42/72 windows. Promotion-enabled baseline and promotion matrices passed in both modes across 14/24 configurations and 84/144 windows. The final heap-enabled wrapper run exited 0 after all regular and promotion-enabled source assertions; counts, numeric fields, and oracles were verified. Functional and report-validation evidence only; no performance gain claimed. |
| Physics raycast recipe | A direct `bench_physics` invocation with `MONO_PHYSICS_RAYCAST_TRIALS=101`, `MONO_PHYSICS_RAYCAST_WARMUPS=3`, suite `engine.physics.bench.raycast`, `--samples 1`, and `--warmup-samples 0` matched the recipe defaults; the `just` recipe was dry-run verified only. Across 17 cohorts and 64 calls per trial, it measured 109,888 public `Raycast` queries. Owner, distance, world-position, and unit-normal analytic oracles passed. Workloads covered empty and base-collider counts 6, 1,024, and 4,096, with mesh sizes of 8, 512, and 131,072 triangles; six actual shapes; a transformed mesh hole and far competitor; and one complex `SyncBroadphase` after movement. The shared mesh geometry was not copied per body. `heap_compiled=0`, `timing_valid=1`. Batch p50/p95/p99/max values are always printed; 101 samples provide sufficient tail coverage. Per-query mean is derived and does not represent individual-query tails. A prior heap-enabled `memory=1` trial passed all 17 rows and its query and allocation oracles. The fixture's `ColliderExtent` was corrected to `{4, 0.5, 4}`; production code did not change. This is workload evidence only; no performance gain or full quiet B0 claim. |
| C01 workload oracles | All seven workloads passed their latest-cohort oracles for job counts 128, 384, and 512. There were nine body invocations total: eight harness warmups and one measured invocation. The report, oracles, and timing ring retain only the latest measured cohort and exclude previous warmup cohorts. Timing acceptance is withheld because the GPU environment was contended. |
| Partial quiet B0 rows | Graph/domain: 30 rows at 101 samples; portal: 5 rows at 101 samples; procedural: 8 rows at 128 trials. Timed runs and exact oracles passed. The whole B0 matrix and all three optimization passes remain incomplete. |
| Procedural fuzzing | Commit `43b84397` ran all three libFuzzer targets for 10,000 runs each with no findings. The raw input cap was 1 MiB + 1 byte; this does not establish full 16 MiB PWP1 corpus coverage. |
| Documentation gate | Commit `1cd5324a` passed the documentation site and public-API coverage checks with zero coverage gaps. |
| C04 capture evidence | Nsight Systems 2026.5.1 on RTX 4090 with driver 580.173.02 recorded an affirmative overlap between the C01 compute and G-buffer raster GPU execution ranges. For 16 measured frames after 16 warmups, overlap was observed in 9/16 one-view frames (sum 166,504 ns, max 27,393 ns), 8/16 two-view frames (sum 160,359 ns, max 27,201 ns), and 12/16 eight-view frames (sum 193,899 ns, max 26,945 ns). This capture at 00:21:24 ACST predates the later model processes and remains valid evidence of physical overlap, not a speedup. Native queue handles, source packet order, and GPU context/correlation IDs identify the raster work; the counts were independently reproduced. Exact full-oracle output passed for the unchanged two-pass kernel over 524,288 `f32x4` elements, with 8 MiB input and output. This benchmark size respects the bridge's 16 MiB aggregate budget for its two result copies; the native executor's separate allowance was not changed. A later set of 12 cohorts (4 MiB/8 MiB, 1/2/8 views, serial/parallel, 256 frames) also passed every exact full-buffer oracle with zero drops, but its timings are rejected for quiet B0 acceptance: two external Ollama `llama-server` processes (PIDs 3275794 and 3275845, started 00:29:54 and 00:29:56 ACST) used the same RTX 4090, with `nvidia-smi pmon` later showing 21% to 70% model use; a .NET build was also active. Local v2 capture artifacts are `.cache/build/bench/c04-nsys-parallel-v2.nsys-rep` and `.sqlite`. GPU ranges do not report instruction occupancy; compute and transfer ranges may appear early per [Nsight Systems Vulkan trace notes](https://docs.nvidia.com/nsight-systems/UserGuide/index.html#vulkan-gpu-trace-notes). At the time of this earlier capture row, Nsight Graphics 2026.3.1 counter capture remained blocked by permissions. A later saved privileged trace with GPU dependency waits is documented in the [follow-up correctness audit](V0261_COMPLETION_AUDIT_2026-10-17.md#c04-saved-nsight-graphics-capture); it does not claim the final 512-frame oracle, accepted benchmark rows, quiet timing, or optimization gain. |
| Earlier Windows cross package | The earlier cross configure/build passed in 414 steps and its package validated at 107,508,739 bytes/1,590 entries. This is historical artifact evidence, not current platform proof. Windows product runtime remains unverified. |
| Raytrace ABI and follow-up checks | Shader ABI commit `0d207e61`, lens fix `237268fe`, and fixture fix `1767c0bd` are covered by the named-member SPIR-V test and final 135-oracle ray matrix across perspective, orthographic, and oblique projections, with zero VUIDs. Timing is rejected under shared workloads. Headless server CTest passed 46/46. [Follow-up correctness audit](V0261_COMPLETION_AUDIT_2026-10-17.md) has the exact scope and logs. |
| Final code-head validation | At `9784ad17`, strict pre-push passed 772 suites with 0 skipped and 0 failed; server full-file testing passed 36 cases/2,011 assertions. Native workflow [38987915616](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/38987915616) passed Linux x86_64, Windows x86_64, macOS ARM64, Build, Process Isolation, Package, and Upload, with 6 artifact records; publish was skipped as a manual dry run. Logs: `.cache/build/ci/v0261-selector-lifetime-pre-push.log` and `.cache/build/server/v0261-server-lifetime-tests.log`. The saved privileged C04 GPU-wait evidence and its limits are in the [follow-up correctness audit](V0261_COMPLETION_AUDIT_2026-10-17.md#c04-saved-nsight-graphics-capture). |
| Remaining | Current approved live Studio scenarios completed with the stated scope. C01 quiet timing remains pending; Ray and C04 timings from contended runs are rejected for quiet B0 acceptance. The complete B0 matrix remains incomplete, and zero of three optimization passes is complete. Consult final-revision full-gate and native workflow results in CI logs and GitHub Actions before claiming completion. |

## Initial evidence checklist

Approval entries below reflect completed user review. All implementation,
validation, optimization, and delivery evidence remains unchecked until
obtained from the authorized work:

- [x] User reviewed this plan and approved the listed module policy changes
  and renderer worker recording subject to the profiling prerequisite.
- [ ] Source assumptions and module invariants rechecked, including the
  in-progress compute and cadence consumers.
- [x] Shared graph module, target layer, closures, and adapter migrations approved.
- [ ] C01 chain, ownership, reflection, limits, and CPU oracle complete.
- [ ] C02 cadence and real consumer complete with installation gaps reproduced.
- [ ] C04 CPU baseline and approved complete backend path demonstrated.
- [ ] C06 real forest and village slice, editor, bake, delivery, and runtime proof complete.
- [ ] Ray inventory complete, including newly introduced ray paths.
- [ ] Functional baseline B0 complete for all milestone workstreams.
- [ ] Optimization pass 1 algorithm and exact-work-removal decisions measured and recorded.
- [ ] Optimization pass 2 retained-reuse and allocation decisions measured and recorded.
- [ ] Optimization pass 3 scheduling and critical-path decisions measured and recorded.
- [ ] Required architecture, test, shader, source, platform, and checklist gates complete.
- [ ] Final concrete visual review and evidence handoff complete.
