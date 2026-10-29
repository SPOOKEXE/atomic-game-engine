# v0.26.1 implementation plan

## Status and scope

This is the approved execution plan for the v0.26.1 work listed in the
[repository roadmap](../../ROADMAP.md). The shared graph module, target layer,
module policy updates, and adapter migrations have user approval. Remaining
implementation and evidence gates are tracked below. Scoped accepted-candidate
evidence and first-pass closure are recorded below. Optimization passes 2 and 3
and the remaining milestone gates are incomplete.

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
| Domain runtime measurement coverage | Separate latency modes passed 11 audio mixing, 5 CPU image, 10 GPU image, and 8 populated grid cohorts with 101 internal trials and complete output checks. The repeated CPU image workload makes 404 individual calls; each grid cohort collects 101 distinct GPU observations over 404 measured CPU frames. Heap hooks are disabled for latency modes. The GPU image fixture's exit-time device-wait crash was reproduced under GDB and resolved by registering direct fixture teardown after renderer dependencies initialize; both the 101-trial mode and the legacy eight-case mode then exited cleanly. This validates measurement and lifecycle behavior, not an optimization gain or complete B0. |
| Renderer allocation diagnostics | The new `domain-render-memory-bench` recipe was dry-run checked; equivalent direct invocations passed all eight GPU image and four blank-grid heap reports with explicit Khronos and synchronization validation, zero validation errors, and zero dropped heap scopes. Snapshots exclude fixture setup and readback oracles. Reports distinguish tracked C++ requested payload, process-lifetime peaks, and logical GPU residency; diagnostic timings are invalid. Both GPU image cache-hit cohorts performed no tracked allocations or GPU work. |
| Actual CPU ray callers | Weather ground-height, character portal-query, and headless Studio viewport suites passed 7, 23, and 34 cohorts respectively at 101 trials and 3 warmups without heap hooks. Separate three-trial heap-enabled diagnostics passed all 64 cohorts. Checks cover real caller results, ignored and tagged geometry, movement synchronization, portal paths, picking, dragging, and placement; Studio adornment drawing is not a separate measured adapter. The recipes were dry-run checked and the equivalent binaries executed directly. |
| Complete renderer admission probe | The four pipeline installation fixtures now measure native build and compilation, scheduling, scheduled `RenderStageProjection`, and frame planning together. Empty, simple, default PBR, and 256-node fixtures passed exact native-wave, node mapping, queue, scope, metadata, and barrier-preservation oracles with heap diagnostics. Existing refusal contracts remain in the graph test suite. This is functional probe evidence; timed B0 remains pending. |
| Ray and volume probe coverage | All 50 renderer cohorts passed at 1920x1080 for each of perspective, orthographic, and oblique projections with heap hooks and explicit Khronos and synchronization validation. Eight measured frames provide allocation diagnostics, not sufficient timing tails. The 15 refraction cases verify source contrast, IOR-one identity, thickness response, and an offscreen guard contribution; fixtures use front-facing panes and an oblique guard inside the actual clip depth. These follow the shader's current incident-ray convention, without claiming physical orthographic optics. Twelve untimed volume boundary cases cover positive falloff, rotated anisotropic bounds, and translated noise sampling. No production shader changed and no optimization gain is claimed. |
| Town promotion VM memory | The heap-enabled town baseline and promotion run exited cleanly and emitted all 12 promotion VM memory reports. Each aggregates 16 batches outside the measured owner interval, excluding reference VMs, setup, warmup, and teardown. Live figures are maximum per-batch sums; Luau peaks are maximum batch sums of individual VM peaks, not a simultaneous global peak. Supported growth deltas are summed. QuickJS peak and growth remain unavailable, and allocator bases are not compared. |
| Measurement-probe validation gate | The first pre-commit run failed the server admission test because its wait accepted a character before the independently replicated player-class component arrived. Commit `5e5b7900` adds that class to the existing wait predicate, preserving the deadline and both exact player-count assertions. The corrected case passed ten consecutive runs, each with 12 assertions; the subsequent full pre-commit run passed 782 suites with zero skips or failures. Logs: `.cache/v0261-b0-benchmark-precommit.log` and `.cache/v0261-b0-benchmark-precommit-final.log`. Formatting and documentation coverage also passed. |
| C01 real-service benchmark | `just script-compute-bridge-bench` now measures actual Luau and JavaScript requests through the renderer-owned compute service, GPU completion, VM materialization and release. The heap-disabled optimized Vulkan run passed all 16 cold/retained cohorts at 101 internal trials, covering 0, 65, 65,537 and 1,048,576 elements. All 2,424 measured jobs had exact ticket-attributed GPU timestamps, zero dropped queries and full untimed every-word CPU-oracle parity. Cancellation and retirement coverage passed 15 GPU cases with 1,199 assertions. Profiling is private, opt-in and bounded; definition, submission, queue, GPU-command, materialization and cleanup costs remain separate. This is functional benchmark and sample-coverage evidence, not complete B0 or an optimization gain. |
| C06 complete-path latency benchmark | `just procedural-delivery-latency-bench` adds directly timed generation, cooking, signed publication and generator-free scene installation. Each trial builds fresh source stores and recipients; only its paired warm phase reuses the cold delivery cache. All four scene sizes passed 101 cold/warm trial pairs with full mesh/object, canonical identity and cache oracles, producing 48 stage distributions. Warm phases fetched zero source bundle payload bytes. Latency runs disable heap hooks; the existing separate allocation diagnostic schema remains intact. Results stay on stdout. A follow-up run observed no concurrent agent builds and low background CPU use, but the full milestone baseline and all three optimization passes remain incomplete. |
| Refreshed C04 execution evidence | At `ce2958ee`, ordinary optimized runs without C++ heap hooks passed 512 frames for empty and 1/2/8-view recording, serial/parallel modes and 4/8 MiB compute payloads. Every populated cohort passed the full-buffer oracle and recorded 512 completed queue measurements with zero dropped queue queries. A separate Nsight Systems 2026.5.1 capture, with heap hooks enabled, saved `.cache/build/bench/c04-current-parallel.nsys-rep` and its SQLite export. Independent source and correlation-ID analysis excluded owner-prefix work and found compute ranges overlapping actual G-buffer raster packets in 15/16, 16/16 and 15/16 measured frames for 1/2/8 views. The capture changed requested Vulkan 1.0 to 1.1; ranges do not establish simultaneous instruction occupancy or a speedup. Normal desktop GPU work remained active. This refreshes execution and overlap evidence, not the complete quiet baseline or any optimization pass. |
| C04 installed EngineGraph path | The installed renderer projects its validated native schedule through `RenderStageProjection` into `EngineGraph`, caches it in `InstalledPipeline`, and executes it at all three `ViewRecording` sites. The old `RenderGraph` runtime walker and lossy `AppendRenderStage` path are removed. Native scope and wave ordering, resource versions, and history remain unchanged; authored projection retains the legacy authored sweep. Integration state only; no performance claim. |
| C04 render-stage graph tests | Stage projection and engine graph integration passed 284 cases and 18,289 assertions. Log: `.cache/v0261-render-stage-graph-tests.log`. |
| C04 parallel recording tests | Parallel recording passed 49 cases and 2,496 assertions. Log: `.cache/v0261-render-stage-parallel-tests.log`. |
| C04 last-good replacement refusal | After the backend refused replacement, the prior output retained real red pixels and identity. 1 case, 52 assertions. Log: `.cache/v0261-render-stage-last-good-tests.log`. |
| Current headless render validation | The headless `test_render` selection excluded `[slow]` cases and passed with explicit Khronos 1.3.275 and synchronization validation through the local cache layer, without root configuration changes. Serial passed 892 cases and 420,052 assertions; parallel passed 892 cases and 420,051 assertions. Both logs contain zero VUID, `SYNC-HAZARD`, or `Validation Error` lines: `.cache/v0261-render-stage-full-validation.log` and `.cache/v0261-render-stage-full-parallel-validation.log`. These are headless non-slow default cases, not all test tiers, platforms, or private hidden WSI coverage; the hidden last-good case has separate evidence above. The UX pre-commit run passed 782 suites with zero failures: `.cache/docs-ux-precommit.log`. |
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
| Complete B0 latency baseline | At frozen benchmark revision `b416f9d4`, all 17 CPU and 15 GPU invocations exited 0, plus 101 fresh-process `RenderNodeKinds` startup observations. Graph, compiler, and admission coverage comprises 46 cohorts at 101 samples; CPU audio has 11 cohorts and CPU image 5. C06 generation/cook pipeline coverage has 8 cohorts at 128 trials, and signed delivery has 8 cold/warm cache cohorts with 101 trial pairs and every stage oracle. Actual ray callers cover portal 5, physics 17, WeatherGround 7, Character 23, and Studio 34 cohorts. Native cadence covers 14 configurations/84 owner windows and town cadence 24 configurations/144 owner windows, with 600 ticks and all promotions. The latency baseline is complete; expanded original/accepted memory diagnostics are recorded below. This is baseline evidence, not an optimization gain. Earlier incomplete and contended rows remain historical. |
| Complete B0 GPU coverage | GPU image passed 10 cohorts, populated grid 8, native compute 7, and VM compute 16. C04 serial/parallel 4 MiB and 8 MiB workloads passed 16 cohorts at 512 frames. Weather passed 4 scenes across 4 phases. Ray passed 50 cohorts across each of 3 projections at 1920x1080 for 512 frames, with 4 boundary proofs per projection: all 150 source oracles passed, GPU timing/query drops were zero, and complex-volume ordered/reversed HDR hashes matched prior functional runs across all three projections. Latency runs used heap hooks OFF; separate heap-enabled diagnostics remain recorded above. Those earlier diagnostics lacked populated grid fixtures and some first-submission boundaries; the expanded original/accepted comparison below closes those gaps. |
| B0 provenance and host conditions | The frozen `bench` build used GCC 13.3 with effective `-O3`, Ryzen 9900X, and RTX 4090 driver 580.173.02. FrameGraph instrumentation remained enabled in benchmarks that request it, so this is not a fully uninstrumented or shipping-cost claim. Session stdout records per-invocation hardware, command, revision, binary and stdout SHA hashes, UTC, and host observations. Desktop graphics remained active; no overlapping build, test, other benchmark jobs, or foreign compute were observed during the accepted runs. |
| Fresh C04 physical-overlap trace | At docs-only revision `853fd23c`, the exact same `bench_render` binary SHA `49a2c26390d2d35ce7f24c8a5e129b47c5dec63adda591d6b67c41f128cbdc33` produced `.cache/build/bench/c04-enginegraph-b0-853fd23c.nsys-rep` and its SQLite export. Independent analysis of 1,703 API/GPU correlations confirmed compute/raster and download/raster overlap in 16/16 measured frames each for 1, 2, and 8 views, excluding warmups, final drain, and owner prefixes. No upload/raster overlap was observed. Synchronization objects were not collected, so GPU semaphore stall time is unavailable; the capture upgraded Vulkan 1.0 to 1.1. This establishes execution overlap, not instruction occupancy or speedup. |
| First pass fog candidate | The `volume.frag` `DensityAt` candidate adds only an explicit positive-falloff guard in that function; quality, sampling, and noise math are unchanged. Optimized heap-OFF Vulkan runs on RTX 4090 at 1920x1080 covered 5 actual volume scenes across 3 projections for 512 frames each, with 128 GPU observations per active cohort. All 27 scene/boundary oracles passed with exact B0 RGBA8/HDR hash matches: 5 scene and 4 boundary checks per projection. Explicit validation across all 3 projections at 8 frames reported no VUIDs or synchronization hazards. Whole-path measurements improved for simple, busy, and complex scenes in every projection; inactive/empty controls retained identity-output behavior. Paired fresh-process runs of the same executable for complex perspective staged the original shader and then restored the candidate for 512 frames each, confirming a stable original baseline and repeated gain, with 5 exact hashes per run, no query drops, unchanged logical GPU residency, and zero churn. The measured-gain candidate is accepted; full first-pass closure is recorded below. |
| First pass physics mesh-ray candidate | At `9ac6fb88` plus the reviewed physics diff, the candidate uses the existing BVH with bounded local scratch, sorts original triangle IDs, retains a complete scalar fallback, and uses the scalar path for meshes of at most 8 triangles. Optimized heap-OFF measurements paired 3 fresh-process original/candidate rounds across 17 cohorts, each with 101 samples, 3 warmups, and 64 queries per batch; all full analytic oracles passed. The first candidate regressed the small 8-triangle cohort; the scalar crossover removed that median regression. Exact focused parity passed 19 cases/564 assertions. Some short control tails vary, so this does not establish blanket tail non-regression or individual-query tails. Actual caller oracles passed portal 5, WeatherGround 7, Character 23, and Studio 34 cohorts; this establishes no caller-wide speedup. Full pre-commit passed 782 suites with zero skips or failures, focused parity passed 19 cases/564 assertions, and the clang-format 21 check passed. Complex-query whole-path batches improved repeatably and the candidate is accepted; full first-pass closure is recorded below. |
| Rejected first pass C01 owned-completion candidate | The five-file experiment was restored to `79d17645`; its source patch is retained at `.cache/build/bench/experiments/compute-owned-move.patch`. Exact CPU checks passed 9 cases/533 assertions and GPU checks passed 15 cases/1,197 assertions with no VUIDs or synchronization hazards. All full oracles passed across 7 native and 16 VM cohorts. Three alternating native pairs repeated a complex whole-completion gain, but two full paired VM runs showed a repeatable empty-Luau whole-path regression; most nonzero VM paths were near unchanged. The candidate was rejected, its root cause remains unproven, and the original copy behavior and budgets are retained. Full first-pass closure is recorded below. |
| First pass C06 local road-endpoint candidate | At `79d17645` plus the reviewed C06 diff, each existing local/halo road record lazily caches its exact 16-chord endpoints per `GenerateRegion` call for house and forest queries. There is no persistent result cache; integer math, RNG, output keys, accounting, budgets, and cancellation are unchanged. Optimized heap-OFF measurements covered 3 alternating fresh-process original/candidate pairs across 8 scene/mode cohorts at 128 trials. All 4 historical hash pins and all 96 fixture comparisons matched, including work, candidates, spatial tests, output geometry, and artifact counts. Busy/complex generation and whole edit-to-publication paths improved repeatedly; simple was broadly unchanged. A small repeatable empty-cohort median increase is an acknowledged measured tradeoff, not a universal-gain claim. Separate optimized heap-ON A/B checks at 1 trial across 8 cohorts passed the pins: allocation-operation counts did not increase, extra allocated bytes were bounded per call, post-call live bytes were identical, and observed whole-pipeline peak was essentially unchanged. This does not establish a leak slope. Unit checks passed 39 cases/3,838 assertions. All 13 optimized heap-OFF benchmark targets rebuilt, full pre-commit passed 782 suites with zero skips or failures, and clang-format 21 passed. Dedicated headless `-O1` ASan+UBSan checks passed Procedural 39 cases/3,838 assertions and BakedShapes 19 cases/564 assertions with no findings; sanitizer evidence is limited to those tests. The candidate is accepted with the recorded empty-path tradeoff. Full first-pass closure is recorded below; final milestone gates remain separate. |
| Rejected first pass C02 local-track preparation | The two-file animation experiment was rejected and restored to `8ba847a7`. Its patch is retained at `.cache/build/bench/experiments/cadence-pass1/single-track-preparation.patch`, SHA `40d31b255d764756df3f7436a3803a97df8602c43bad6006b7fab7a584b19451`. Dev Animation checks passed 7 cases/20,301 assertions, and the actual paired Town case passed 1 case/18,614 assertions. Optimized heap-OFF original/candidate runs used the same `bench_client` executable path and the current accepted control: native covered 14 configurations/84 owner windows and Town 24 configurations/144 owner windows each, at 600 ticks with all promotions. All 228 matched work counts were exact, with zero owner drops and promotion latency. Native complex-full showed no improvement, authored paths had small mixed benefit, and many observed Town medians were worse. The experiment provided no convincing whole-path gain, so the added complexity was rejected; causal attribution remains unproven, and this does not establish a universal absence of benefit. The complete original Town rerun supplied the comparison evidence. Full first-pass closure is recorded below. |
| First pass CPU image Blend reader candidate | At accepted control `37cece4e` plus the reviewed diff, only Blend removes redundant bounds/offset work using admitted byte offsets and exact `uint8`-to-`double` conversion; Over, Put, other operations, matching-canvas refusals, ownership, and budgets are unchanged. Three alternating fresh-process original/candidate pairs used the same optimized heap-OFF executable path across all 5 cohorts, with 101 internal trials and 8 warmups; the repeated 256 cohort retains 4 calls per trial. All full-pixel oracles passed, including the added combined odd-width, alpha, opacity, and same-source public-graph byte oracle; the old `At` reference shares unchanged pixel helpers. Populated whole-evaluation medians improved repeatedly and the simple median was essentially unchanged; the empty refusal control varied and some busy short tails were mixed, so no universal tail claim is made. Dev and dedicated headless `-O1` ASan+UBSan checks each passed 36 cases/1,201 assertions, with no sanitizer findings; full pre-commit passed 782 suites with zero failures or skips, and clang-format 21 passed. Diagnostic Tracy artifacts `.cache/build/bench/bench/cpu-image-attribution-37cece4e.tracy` and `.cache/build/bench/bench/cpu-image-blend-candidate-37cece4e.tracy` contain 108 and 109 complete Eval roots respectively; 2 baseline orphan boundary children were excluded and the candidate had none. Source/allocation work was minor beside Transform/Blend, and paired complete zones attributed savings to Blend; attached captures are attribution diagnostics, not latency acceptance. The candidate is accepted; full first-pass closure is recorded below. |
| First pass latency matrix recheck | Accepted production at `9de02761` passed all 17 CPU and 15 GPU invocations with the B0 workloads. Each of the three ray projections matched all 109 ordered non-timing output/work records exactly, including all scene, boundary and motion hashes. Native and town cadence retained every non-timing owner-window field across 84 and 144 windows. The original final oblique timing overlapped documentation browser work and is excluded; an isolated replacement at documentation-only `32a973e4` used the identical executable and passed all 109 comparisons. All 101 fresh-process catalogue samples matched the B0 whole-row oracle, with the same executable hash and a 1.40 ms median versus 1.39 ms at B0; this is an unchanged control, not a startup optimization. GPU query drops were zero. Weather advancing phases retained 16 detailed counter events and reported 496 omitted details at both revisions; aggregate work counters remain available. All latency and startup invocations are terminal. Expanded memory and final first-pass checks are recorded below. These timings retain their original harness and binary hashes; later memory probes change outer benchmark-body overhead, but add no calls inside the retained individual timing intervals. |
| Expanded first-pass memory comparison | All 33 accepted-code and 33 original-production invocations exited 0 with heap hooks enabled. Original controls stage only the four changed production files from frozen B0, retaining the current measurement fixtures. Both variants passed 40 portal checkpoints, 84 populated/blank-grid checkpoints, 900 ray checkpoints across all three projections, 96 C04 checkpoints and 16 weather phase rows. Requested CPU byte/block/overhead and logical GPU family/allocated/released/live/peak invariants passed with zero dropped heap scopes or foreign frees in these probes. All tracked GPU residency, lifetime-peak and cumulative allocation/release fields matched exactly across grid, ray and C04 rows; all 16 weather GPU phase records also matched. Driver commitment and interval-only peaks remain unavailable. |
| Memory work and allocation decisions | Compiler heap rows, prepared CPU image/audio rows and all 40 portal checkpoints matched exactly. All 17 physics ray cohorts allocated zero tracked bytes/blocks during measured query windows and retained unchanged physics storage. Native cadence matched all 506 non-timing records exactly. Town work and C++ heap fields matched; 16 QuickJS live-byte fields varied by at most 144 bytes in either direction, with no causal optimization claim. C06's eight canonical pipeline hashes and reservation limits matched; observed whole-trial peak changes ranged from -7 to +270 bytes, post-trial live differences from -7 to +1 byte, and live-block counts were unchanged. Existing bounded road scratch and empty-path latency tradeoffs remain explicit; these diagnostics do not establish a leak slope. CPU image, audio and signed-delivery reports passed the existing domain verifiers for both variants. |
| Memory provenance and restoration | Retained accepted CPU/startup observations preserve their original seven-path fixture metadata and binary hashes. The later reporter-only change to render/benchmarks/WeatherGpu.cpp is absent from those executable inputs; it belongs solely to bench_render, separate from production render/src/WeatherGpu.cpp. The accepted Town/GPU recovery and all original controls record the actual final eight-path freeze; corresponding relevant fixture blobs and workload settings match. Earlier truncated collector output is excluded, and recovery/control captures have no truncation. Four accepted production source hashes were restored exactly after the original control, heap hooks were disabled and all 12 matrix targets rebuilt successfully. No fresh outer benchmark-body latency claim is made for the expanded harness. |
| First-pass closure checks | The frozen accepted production and expanded probes passed clang-format 21, architecture and source checks, all 93 shader contract/SPIR-V/MSL checks, and the required pre-commit tier: 782 run, zero skipped or failed. Logs: .cache/v0261-pass1-final-gates.log and .cache/v0261-memory-final-heap-off-build.log. Public API documentation coverage is checked again for this record. Existing scoped sanitizer and parity evidence above remains applicable to the unchanged accepted production sources. This closes pass 1 only; passes 2 and 3 remain required. |
| Second-pass scheduler membership candidate | At accepted control `867c6bf1` plus the reviewed diff, `CompileSchedule` replaces temporary per-node hash-set membership with a call-local bitset bounded to 8 KiB at 256 enabled nodes. It still appends outgoing edges on first insertion in the original generation order; no schedule, cache, queue, history or refusal contract changes. Three alternating original/candidate pairs used the same optimized heap-OFF executable path across all 16 catalogue/admission cohorts at 101 samples. All five complete catalogue/semantic oracle row hashes and all work rows matched in every pair. Default-PBR whole-admission medians improved 24-25% and prepared-schedule medians 37-38%; stress prepared scheduling improved 5-12%, while whole stress admission varied from 7.5% faster to 2.6% slower. Short tails were mixed, so there is no universal tail-gain claim. Separate heap-ON comparisons passed all 20 rows and the same full oracle hashes: default-PBR admission used 30,816 fewer tracked bytes and 934 fewer allocation operations, stress admission 89,168 fewer bytes and 1,536 fewer operations, with unchanged retained-byte counts in every row. Process lifetime peaks are not interval-only peaks or a leak slope. One combined 256-node public fixture covers all membership words, duplicate resource/scope prerequisites, and a real future producer; it passed against the original and candidate. Graph/core CTest passed 2/2 for both, render graph checks passed 39 cases/2,281 assertions, and dedicated headless -O1 ASan+UBSan schedule checks passed 19 cases/1,205 assertions with leak detection and no findings. Format 21, architecture, source and pre-commit checks passed: 782 run, zero skipped or failed. Logs: `.cache/v0261-schedule-pass2-gates.log` and `.cache/v0261-schedule-pass2-sanitize.log`. The first timing batch's byte-truncated capture is excluded; the complete compact recapture retains whole-row oracle SHA hashes. Accepted source is restored and the benchmark target rebuilt with heap hooks OFF. This accepts one pass-2 candidate only; its full all-workstream matrix and pass 3 remain incomplete. |
| Second-pass light-ray noise-cell reuse | The invocation-local cache keys all three `floatBitsToUint(floor(exact noisy coordinate))` lanes and retains only the 8 hash corners. Sample positions, interpolation, falloff, accumulation, quality settings and bindings are unchanged. Optimized heap-OFF runs used 1920x1080 on RTX 4090/Vulkan 580.173.02. Normal full 512-frame controls covered 5 scenes across 3 projections, plus 2 extra complex-perspective pairs. Complex whole-completion medians fell 37.8-38.1% and busy medians 27.1-27.7%; controls and simple short tails were mixed. The default-preserving `just ray-volume-noise-bench` added at `0809a3ea` runs NoiseScale 4, complex 16-volume scenes and 64/32 step bounds. This workload does not prove all cache lookups miss. Its three-projection 512-frame pairs had 20.5-23.2% lower medians, exact cross-variant RGBA8/HDR and work parity, and zero dropped marks. The earlier normal orthographic pair overlapped browser activity, so it is excluded and replaced by a clean rerun. The old normal `b11fa851` and new `0809a3ea` harness results retain separate provenance; within each pair, the executable binary and path match. No outer `BENCH` timing claim is made. Actual Khronos library mapping with synchronization validation across all three projections reported zero VUIDs or SYNC hazards. Separate heap-ON diagnostics used 12 fresh invocations, 216 checkpoints and 108 matched stage pairs. All GPU logical fields, CPU allocation operations, live bytes/blocks, overhead, process lifetime peaks and steady report fields matched. The only CPU byte increase was 2,312 bytes per measured renderer startup, matching the SPIR-V increase from 30,388 to 32,700 bytes. Translated MSL grew separately from 15,757 to 17,397 bytes (+1,640). Source, SPIR-V and MSL were pinned before and after each memory run; one heap-ON binary was used, then candidate and compiled artifacts were restored and heap-OFF rebuilt. This says nothing about driver commitment, registers, spills, occupancy, leak absence or interval peaks. Desktop GPU activity and unlocked clocks qualify performance, not universal gain. Format, architecture (55 modules), source (0 open findings), all 93 SPIR-V/MSL shader checks, public documentation coverage and pre-commit checks passed: 782 run, zero skipped or failed. Em-dash checking found no U+2014 in 3,118 first-party files. Logs: `.cache/build/bench/experiments/volume-noise-pass2/final-gates.log` and `final-docs.log`. Accepted with the measured startup-byte cost and hardware limits. Artifacts are in `.cache/build/bench/experiments/volume-noise-pass2`; raw benchmark results remain in stdout/RAM. Full pass-2 all-workstream matrix and pass 3 remain incomplete. |
| Second-pass C01 owned completion | At accepted control `3ec72f8f`, the five-file change adds a fully validated rvalue completion handoff and preserves const-copy isolation, refusal precedence, fences, cancellation, and conservative two-result admission charges. Three alternating original/candidate pairs used the same optimized heap-OFF executable path, with 7 native cohorts at 128 batches and 16 VM cohorts at 101 trials and 3 retained warmups. All output, work, reservation and retirement checks passed; poll counts vary. Runs used the optimized `bench` preset on RTX 4090/Vulkan 580.173.02; desktop activity qualifies the timings. The old empty-Luau regression did not repeat. This is not a universal latency win: native and VM tails were mixed, and retained Luau at 1,048,576 elements had 0.33-1.82% higher whole-path medians in all three pairs. Separate heap-ON diagnostics passed four fresh invocations, 216 snapshots and 46 allocation-window rows. All 23 paired allocation windows used fewer C++ bytes and operations, matching published result bytes plus per-job metadata. Native 1M serial saved 357,924,736 bytes and 384 allocation operations over 128 jobs; both VMs saved 5,592,528 bytes and 4 operations per two-request 1M window, about 33.6 MB to 28.0 MB. Tracked C++ live bytes/blocks, profiler overhead and process lifetime peaks matched at all 108 paired snapshots. Fixed semantic GPU memory boundaries, allocation windows, work and budgets matched; asynchronous pump-peak observations remain diagnostics. Separate VM observations differ: cold Luau 65,537 has +16,360 bytes/one block and cold Luau 1M has -16,360 bytes/one block at materialized release; retained Luau live bytes matched, and QuickJS live-byte deltas ranged from -32 to +16 bytes. No cause, VM-heap non-regression, leak absence, residency improvement or shipping-speed claim is made. CPU runtime checks passed 9 cases/533 assertions; 15 GPU cases/1,198 assertions passed with the Khronos library actually mapped and synchronization validation enabled, with no validation diagnostics. Format 21, architecture, source and public documentation checks passed; pre-commit completed 782 suites with zero skips or failures. The reviewed controller pinned source, 194 runtime inputs and both live example scripts, protected hardlink archives, and restored exact candidate sources/binary with heap hooks OFF. Four diagnostic stdout hashes were verified without truncation; results remain in stdout/RAM. Accepted for C++ allocation reduction with the measured latency costs. Logs and controller: `.cache/build/bench/experiments/c01-owned-pass2`. The full pass-2 matrix and pass 3 remain incomplete. |
| Remaining | First-pass latency, memory comparison and changed-source gates are complete within the scopes below. One of three optimization passes is complete. Passes 2 and 3, final-revision milestone gates, platform verification and final handoff remain open. Earlier contended timing runs remain rejected. |

## Optimization pass 1 decision ledger

This round is complete only with the memory comparison and final checks recorded in the evidence table. Unchanged paths are measured decisions, not optimization-gain claims. Earlier candidate rows preserve their original test and timing scopes.

| Workstream | Decision | Evidence and limit |
|---|---|---|
| Shared graph compiler and graph admission | Measured no change | Full compiler/admission and startup controls remain covered; prepared graphcore compile has zero tracked allocations. No new shared cache or ordering change was justified in pass 1. |
| CPU image runtime | Accepted | Blend removes redundant admitted-coordinate checks; full-pixel output is preserved. Paired populated medians improved; short empty/control tails remain qualified. |
| GPU image and grid composition | Measured no change | All 10 image and 8 populated-grid latency cohorts retain exact non-timing work, samples, oracles and logical residency. No pass 1 production edit. |
| Bake schema/compiler/runtime | Measured no change | Existing schema and full build/run cohorts retain canonical-output checks; no pass 1 production edit. |
| Audio graph and mixer | Measured no change | Prepared callback diagnostics allocate and retain zero bytes; all sample/clock oracles remain intact. A larger persistent routing cache was not justified in this round. |
| Engine schedule and renderer catalogue | Measured no change | Whole admission, prepared schedule, frame planning and 101 fresh registrations were measured. No startup or scheduling gain is claimed. |
| C01 native and both script VMs | Rejected experiment | Owned-completion experiment lacked an acceptable whole-path result across native and VM controls, including the empty Luau regression. Original copy behavior and budgets retained. |
| C02 native and town cadence | Rejected experiment | Local-track preparation did not give convincing whole-path benefit across 14 native and 24 town configurations. Track timing, promotion and exact work remain unchanged. |
| C04 recording and queue overlap | Measured no change | Serial/parallel 4/8 MiB workloads, full-buffer checks and existing vendor overlap evidence remain covered. No pass 1 scheduling edit or new overlap/speedup claim. |
| C06 generation and signed delivery | Accepted | Call-local road chord reuse improves busy/complex generation and publication; exact canonical output is preserved. Bounded scratch growth and the small empty-path median cost remain explicit tradeoffs. |
| Physics rays and actual CPU callers | Accepted in mesh-ray kernel; no caller-wide gain claimed | Existing mesh hierarchy removes exact triangle work with ordered candidates and scalar fallback. Portal, weather, character and viewport callers retain their full result checks. |
| GPU ray consumers | Accepted in volume density; remaining consumers measured unchanged | Positive-falloff guard preserves all scene and boundary hashes across three projections. Trace, refraction, lens and mirror controls retain work/output coverage. |
| Weather phases | Measured no change | Four scenes and four phases retain clock, cache, resource and timestamp checks. Detailed counter-event omission is reported separately from zero dropped GPU query marks. |

## Benchmark fixture corrections and verification limit

The weather benchmark now supplies a private presentation clock independently of
the semantic weather tick. Every existing render advances a synthetic 60 Hz
ordinal; the final existing maintenance render adds one second for settling.
Before strict reuse, the fixture checks actual retained work, classification,
previous pages and transition alpha. Production uses `steady_clock` when the
private override is absent. Render counts, weather ticks, waits, quality and GPU
sampling remain unchanged. Integral work counters print without rounding, while
the two derived ratios retain full double precision.

Procedural pipeline stage reports now retain placement and road counters beside
work units, candidate cells and spatial tests. Cohort totals explicitly include
canceled jobs; their partial work remains a diagnostic that depends on worker
progress. Successful worker snapshots check all five counters against the serial
result, and direct kernel rows report their actual output counts. This changes
benchmark bookkeeping and report schema, not generator or publication behavior.

The existing FrameBatch and WeatherResidency suites include clock refusal,
reset and settled-reuse checks. Source review, formatting and whitespace checks
passed. The CI renderer test target and optimized benchmark targets compiled
with two build jobs. Focused CPU weather tests passed 63 cases and 119,645
assertions. Headless Vulkan weather tests passed 18 cases and 922,185 assertions;
the Khronos synchronization-validation library was observed in the process and
no validation errors were reported.

Ten fresh weather benchmark processes covered inactive, empty, simple, busy and
complex scenes twice each at 1920x1080, with 512 frames in each measured reuse
and advancing-tick phase. All non-timing fields matched across the 20 paired
phase reports, including logical GPU resource counters. Strict reuse performed
no field/shadow dispatch or page upload, and recorded no logical GPU allocation,
release or history reset.
This proves harness repeatability for these inputs, not a speedup, image parity,
driver heap residency or useful shadow-timing tails. This is a new fixture epoch;
its measurements must not be mixed with the old uncontrolled clock workload.

The procedural harness passed eight one-trial serial/threaded cohorts. All five
cohort progress totals reconciled against their observed stage totals, including
canceled work; direct kernel rows remain separately accounted. Canonical output
hashes matched between modes for all four scenes. This is functional reporting
validation, not latency-tail or allocation evidence. The full CI pre-commit tier
passed all 782 suites with no skips or failures, using two runner workers:
`CMAKE_BUILD_PARALLEL_LEVEL=2 just preset=ci test-pre-commit --no-report --jobs 2`.

The full pass-2 comparison was interrupted twice after reports of device
freezing. Its partial captures do not establish full-matrix completion. The
host restart also removed the volatile capture state; the saved provenance
manifest is not a substitute for the missing raw captures. Temporary production
overlays were restored without rebuilding. Compilation, benchmarks and remote
jobs were stopped while source edits and static checks continued. The user then
reported a Discord screen-sharing collision and cleared resource use; validation
resumed with two build jobs and one GPU workload at a time. Pass 2, pass 3 and
the final acceptance gates remain incomplete.

## C01 combined-variant retake

The combined-variant retake is complete for this scoped experiment. The
baseline is the current fixtures with the six production sources from
`867c6bf1`; the accepted side is the current six-source production state at
`aea22e88f04e27b256d016664dca318d087d4c36`. The ignored
controller at `.cache/build/bench/experiments/c01-combined-retake/run.py` has
SHA-256 `5237954df73358df0263e3128d4d356092d0deeb2ec43b7da06943e0b575be52`;
the frozen helper has SHA-256
`743460353e49a78814fdae6c74019ff15f8d391e9313e6cef3c1418d1473ea55` and its
inputs have SHA-256
`4f083f6c0de6922b7f5df11a4862dd064d364fe3f7cfa9c7e2569f90659de538`.

Three fresh-process pairs ran in B/A, A/B, B/A order using the same optimized
`bench` preset, heap hooks off, and `bench_render` executable path. Only the
target was rebuilt, with two build jobs, for each phase. Runs used an RTX 4090
with Vulkan driver 580.173.02, unlocked clocks, and desktop activity;
timings are environment-qualified and do not support a quiet-host or universal
gain claim. Native coverage was 7 cohorts at 128 batches; Luau and JavaScript
coverage was 16 cohorts at 101 trials, excluding 3 retained warmups. All 12
fresh processes exited successfully and ran 138 cohort executions (42 native, 96 VM),
producing 1,404 distribution rows (252 native, 1,152 VM).

Every output word and selected fixed-work, materialized-byte, read-call,
reservation, and retirement check passed. Poll counts are diagnostic and are
not a parity condition. Small `u32` mean completion was 754,180 / 742,353 /
761,314 ns on baseline and 739,354 / 743,324 / 736,430 ns accepted. The earlier
approximately 2.31 ms accepted result did not reproduce; its cause is not
established and no hypothetical fix is claimed. Empty native and cold JavaScript
zero-count paths have costs, while VM mixed workloads vary. The data supports no
universal latency gain. Earlier isolated allocation claims retain their own
scope and are not combined with this latency result.

Whole-completion P50 delta is accepted versus baseline, computed as
`accepted / baseline - 1`; positive values are slower. Pair columns follow the
run order above. These per-cohort timing changes are mixed:

| Workload | Pair 1 | Pair 2 | Pair 3 |
|---|---:|---:|---:|
| Native `u32`, capacity/count 65, inflight 1 | -0.33% | -0.21% | -0.86% |
| Native `u32`, capacity/count 65,537, inflight 1 | -1.33% | -0.67% | -1.92% |
| Native `u32`, capacity/count 1,048,576, inflight 1 | -3.02% | -8.00% | -7.19% |
| Native `u32`, capacity/count 1,048,576, inflight 3 | -5.47% | -6.15% | -8.64% |
| Native `f32x4`, capacity/count 65,537, inflight 1 | +1.12% | -2.34% | -2.37% |
| Native `f32x4`, capacity/count 65,537, inflight 4 | -4.90% | -2.38% | +1.16% |
| Native `u32`, capacity 65, count 0, inflight 1 | +18.47% | +0.62% | +0.24% |
| Luau, 0, cold | +2.01% | -3.25% | +3.16% |
| Luau, 0, retained | +17.52% | -0.82% | -1.37% |
| Luau, 65, cold | +0.98% | -3.51% | -2.92% |
| Luau, 65, retained | +15.41% | -3.04% | -7.89% |
| Luau, 65,537, cold | -3.29% | -4.65% | -1.89% |
| Luau, 65,537, retained | -0.61% | -8.79% | -20.30% |
| Luau, 1,048,576, cold | +6.11% | +0.13% | -2.83% |
| Luau, 1,048,576, retained | +14.13% | -2.03% | -1.68% |
| JavaScript, 0, cold | +0.37% | +2.43% | +6.38% |
| JavaScript, 0, retained | +5.50% | -6.12% | -2.32% |
| JavaScript, 65, cold | +1.53% | -7.29% | +6.87% |
| JavaScript, 65, retained | -1.44% | -28.84% | +1.04% |
| JavaScript, 65,537, cold | +2.97% | -0.66% | +3.90% |
| JavaScript, 65,537, retained | +6.15% | -14.82% | +0.40% |
| JavaScript, 1,048,576, cold | +0.22% | -2.44% | +2.68% |
| JavaScript, 1,048,576, retained | +0.86% | -1.06% | +8.58% |

Cleanup completed with `ok=true` and zero errors. Accepted source, runtime
guards, heap hooks OFF state, and clean tracked tree checks passed; the accepted
binary SHA-256 is
`889f9be04237f6eac6d66811b238b677663a56897cac57884f2b99c3d2163df3`.
One transport field, `pair1-accepted/latency-19/after.processes`, was partially
truncated. Benchmark metric, schema, and atom records are complete, and the GPU
context was unaffected. Full process context and lossless whole-transport
capture are not claimed. Raw results remained in RAM and stdout. Production
sources are unchanged by this experiment. This closes only the combined-variant
C01 retake; the milestone, pass 2, and pass 3 remain incomplete.

## Initial evidence checklist

Approval entries below reflect completed user review. Unchecked implementation,
validation, optimization, and delivery entries require evidence from the
authorized work:

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
- [x] Functional baseline B0 complete for all milestone workstreams, including expanded memory diagnostics.
- [x] Optimization pass 1 algorithm and exact-work-removal decisions measured and recorded.
- [ ] Optimization pass 2 retained-reuse and allocation decisions measured and recorded.
- [ ] Optimization pass 3 scheduling and critical-path decisions measured and recorded.
- [ ] Speed up benchmark and verification workflows while preserving required samples, isolation, and parity.
- [ ] Required architecture, test, shader, source, platform, and checklist gates complete.
- [ ] Final concrete visual review and evidence handoff complete.

## Progress note 2026-10-22

The full pass-2 matrix on `e3c1709bed9cc938035f864e811ee7424a444d82` was
stopped at the user's request after whole-device lag. The controller and GPU
child were killed without build cleanup and exited 137. The 29 completed run
events, ending at `latency-28`, are rows from the accepted build with heap
hooks OFF. The single `latency-29` ray-consumer run was killed incomplete.
There were no startup events or base-heap comparison. These current-only rows
do not complete the matrix or establish a combined gain.

Work is now steered toward deterministic per-tick physics work caps. The
change adds a default 1,000,000 weighted bounded-kernel and raw
traversal units per world tick across substeps, with separate row, geometry,
and cumulative grid-entry caps, an eight-step actual-clock guard, and a sticky
owner fault with no rollback. Budget-blocked worlds cannot network or export
through Play. Restore an earlier complete checkpoint or recreate the store to
resume. Queries and direct stages outside active context remain unlimited,
though blocked guards still apply; older complete checkpoints seed limits on
their next scheduled tick. Defaults are not calibrated milliseconds or proof
of whole-device isolation. Bounded parallel grid rebuilds use the serial route;
existing unlimited parallel builders are unchanged, and escaped bounded-BVH
sync rebuilds remain. No performance gain is claimed.

The Welds grouping fix has reviewed GO and preserves ordering and cycle
behavior under the same 1,000,000-unit default. Physics checks now pass 346
cases/142,063 assertions, and the original TornadoSim client scene passes 1
case/57 assertions. Spatial 120/2,376, focused Server 2/17, and Studio 2/20
remain passing. Server Replication ran 13 cases: 12 passed and one failed.
The case `a world too big for one datagram still streams` failed at
`REQUIRE(joined)` after its three-second join deadline. The isolated same case passed 1 case/6 assertions
with 100% of one CPU and 512 MiB. This is consistent with a quota-sensitive
deadline, but does not prove the cause.

Small checks use 50% of one aggregate CPU, 512 MiB, and at most one job;
peak cgroup-accounted memory was 79,802,368 bytes (about 76.1 MiB), with zero
high, max, or OOM events. The earlier broad stop observed about 3.92 GiB total
use with no OOM kills. Full pre-commit remains unfinished, with no full-tier
pass claim. The capped `just preset=ci docs-check` passed with exit 0. Format
(clang-format 21.1.8), em-dash, source, components, architecture (including six
fixtures), generated API site, and all-public-entity coverage checks passed.
Coverage passed after comments were added to ten adjacent spatial overloads.
Temporary `NUM_PROC_THREADS=1` and `DOT_NUM_THREADS=1` overrides were applied
after configuration; the original generated Doxyfile files were restored in
`finally`. A prior attempt hit TasksMax
32 and was addressed by a single-worker run. At 512 MiB and 50% of one CPU,
peak cgroup memory was 403,668,992 bytes (about 385 MiB); `memory.events` showed
high=632, max=0, oom=0, and oom_kill=0. The high count reflects the soft 384 MiB
throttle; no OOM occurred. All workloads are stopped. No code behavior changed
for documentation, and changes remain uncommitted. Targeted compilation uses
one job and at most 1 GiB. The `e3c1709` full-matrix controller must be
explicitly repinned and revalidated before resumption. The full matrix, pass 3,
and final gates remain incomplete.

## Progress continuation 2026-10-22

CPU graph-domain builds passed for graphcore, graph, imagegraph, bakegraph,
bake, and audio under CI with one job, 50% of one CPU, and a 1 GiB memory cap.
CPU-only batches passed suite/assertion counts of 20/8,749, 285/19,086,
36/1,201, 29/8,475, and 126/16,355. Audio passed 105 cases/150,506 assertions
with one skip; the skipped case passed in isolation with
`SDL_AUDIODRIVER=dummy` at 1 case/44 assertions. This verifies 602 unique CPU
cases across the batch and rerun, not GPU coverage. Batch peak cgroup memory
was 79,446,016 bytes; high, max, and OOM events were all zero.

A combined EngineGraph public fixture compiled, and affected EngineGraph suites
passed 10 cases/2,057 assertions under 512 MiB and 50% of one CPU. It checks an
interleaved barrier consumer sequence of 2, 1, 2, callback lifecycle, node IDs,
queues, and contexts. The final harness has GO with five prepared static
fixtures (empty, simple, busy, complex, and busy deterministic), 12 BENCH rows,
10 heap probes, compile-plus-execute records, and allocation-free oracle
callbacks. One fixture has 64 independent CPU-authored inputs grouped into one
wave, compared with 64 deterministic waves. The `bench` preset, heap-hooks-OFF
build of only `bench_graph` passed with four build edges, one job, 50% of one
CPU, and a 1 GiB cap. The untimed
`.cache/build/bench/bench/bench_graph --mono-suites` run passed with exit 0;
static Make executes all five startup oracles before CLI exit. Scope peak was
9,056,256 bytes (about 8.64 MiB), with high, max, OOM, and OOM-kill events all
zero. No samples, timing, heap diagnostics, or GPU work ran. The new
`just enginegraph-bench` recipe parsed with `just --show`, but the job itself
was not executed.

This fixture epoch strengthens the oracle and changes prior row cost, so its
timings cannot be compared directly with historical rows. The frozen matrix
controllers remain untouched; their validators need explicit review for the
new 12-BENCH/10-heap schema before reuse. No optimization gain, pass 2 or pass 3
closure, or full pre-commit pass is claimed. Changes remain uncommitted pending
the required full test tier.

## Progress note 2026-10-22: EngineGraph capacity reuse

The reviewed call-local barrier-vector capacity reuse candidate has GO for the
allocation change only. CI Debug heap probes reduced prepared complex from 63
allocations/1,008 requested bytes to 1/16, and simple from 2/32 to 1/16. The
other eight rows were unchanged, retained delta was zero, and the 16-byte scope
peak was unchanged. This does not establish a general peak-RAM gain: larger
earlier wave capacity remains until the call returns.

Optimized heap-OFF runs used Ryzen 9 9900X, three alternating B/A, A/B, B/A
pairs, 12 cohorts per phase, and 72 fresh processes. Each cohort used 101
samples and 8 warmups, totaling 7,272 measured samples. Startup oracle and
fingerprint gates passed; CPU quota throttling and memory high/max/OOM events
were zero. Prepared complex P50s were B 2,204/2,275/2,265 ns and A
1,443/1,493/1,482 ns, about 34.4% lower. Simple was 50 to 40 ns; empty and busy
were unchanged, and deterministic busy was mixed. Compile controls moved too,
so this supports no causal full compile-plus-execute, whole-engine, GPU, or
tail-wide claim. Bounded timing peak was at most 39,567,360 bytes. The exact
candidate source was restored and rebuilt. Graph CPU tests passed 286
cases/19,232 assertions. Mandatory pre-commit remains pending. The full matrix,
pass 3, native, live, and platform gates remain open; no roadmap completion is
claimed.

## Progress note 2026-10-22: runner memory

The normal CI all-target build completed its remaining 33 edges in 168.7145
seconds at 50% of one aggregate CPU, one job, and a 2 GiB cap. Peak memory was
2 GiB; `memory.max` recorded 4,840 events, with zero high or OOM events. This
counter does not establish successful reclamation. The prior 1 GiB cap was
self-imposed and too low; temporary client batch-size and GCC garbage
collector experiments also failed the cap. Unity size 8 and default GCC GC
were restored. Ignored include state was unset and deleted, and 49 owned unused
prototype files totaling 37,931,874 bytes were removed. No durable build policy
change or speed claim is made.

After the full `ComputeSignatures` statement returns, signature closures are
destroyed, followed by one `__GLIBC__`-guarded `malloc_trim(0)` call before
child workers. Freeing closures alone did not reduce RSS. In controlled
fresh `--list` processes with a 4096-byte stdout pipe blocked after hashing,
baseline anon/RSS was 429,076/435,116 KiB; lifetime-only was 429,092/435,040
KiB; final trim was 10,536/16,616 KiB, about 97.5% lower anonymous RSS. Peak
VmHWM was 489,028 KiB baseline and 489,384 KiB after trim, about 478 MiB.
This measures glibc behavior only and is best-effort, not universal; no latency
gain is claimed. All 783 list rows were byte-identical with output SHA-256
`d2f80d7ca120d7e7c87a9abfec58de5d345eb872ba1276e41356ce4826e96d4c`, though
the lists expose only 12-hex signature prefixes, not full signature parity.

Existing runner checks passed 84 cases/693 assertions. The exact
`just preset=ci test-pre-commit --jobs 1 --report .cache/build/ci/work-budget-precommit`
run passed all 783 suites: 8,464/8,464 cases, 4,507,123 assertions, zero
failures, and zero skips. It used `SDL_AUDIODRIVER=dummy`, one aggregate CPU,
one job, a hard 512 MiB cap, and excluded `[gpu]`-tagged and `[slow]` suites.
Runner time was
416.74 seconds; wall time was 437.475 seconds including build and discovery.
Cgroup peak was 536,870,912 bytes; `memory.events` reported high=0, max=2,022,
oom=0, and oom_kill=0. The max count records reaching the limit and reclamation
pressure, not OOM. At suite 356/783, runner-parent anonymous RSS remained
33,360 KiB. This full pre-commit tier now passes for current code. Final static
gates also passed: formatting for all 39 changed C++ files; em-dash check over
3,123 files; source check across 13 fixtures and 2,643 files, covering 606
public headers with zero open findings; components check with 267 documented
components; and architecture check across 55 modules, 6 programs, 41 layered
targets, and 6 fixtures. The static batch took 138.2686 seconds under a 50%
CPU quota and 512 MiB cap, peaked at 130,174,976 bytes, and recorded zero memory
events. Final review accepted the one-shot glibc trim boundary. The current CPU
tier and static gates are complete. The full matrix, pass 3, native, live,
platform, GPU, and slow-test verification remain open; no roadmap completion
is claimed.

## Progress note 2026-10-22: cooperative GPU benchmark pacing

The benchmark-only budget patch was reviewed and built on base
`9044f255`. It drains native queues through the SDL wait and `vkDeviceWaitIdle`;
production async and queue behavior, global clocks, and power settings are
unchanged. At
`MONO_GPU_BUDGET_PERCENT=25`, support is limited to ray consumers, recording
batch, and weather GPU. Unsupported suites, missing suite selection, and
malformed values exit 2 before device initialization. Other GPU benchmarks
refuse a requested quota. GPU unit tests are not covered; do not assume this
quota applies to them.

Short live checks used CPU 100% of one aggregate CPU, 2 GiB host RAM, swap off,
and no OOM. Each run began from idle; A is the charged active envelope, followed
by required idle 3A. All checks also passed their oracles:

| Check | A | Required idle | Actual wait | Wall time | Charged RAM peak |
|---|---:|---:|---:|---:|---:|
| Ray complex noise-scale 4, perspective 1920x1080, 16 measured frames; 26 bursts | 5.014859623 s | 15.044578869 s | 15.046196245 s | 24.418991342 s | 113,197,056 B |
| Recording parallel, 1 view, 1,024 meshes, 4 MiB, 16 frames, full-buffer oracle; 34 bursts | 0.473897464 s | 1.421692392 s | 1.423502217 s | 2.216654282 s | 118,120,448 B |
| Weather complex, 1920x1080, 16 frames per measured phase; 131 bursts including 96 maintenance, cold, setup, and flush | 2.671458568 s | 8.014375704 s | 8.021747119 s | 11.003938922 s | 55,758,848 B |

For ray, the table's A, required idle, and actual wait cover only the 26
measured bursts. The separate nine-burst oracle used A=0.987691438 s, required
idle=2.963074314 s, and actual wait=2.963575389 s. The 24.418991342 s wall time
includes both phases.

The ray check also recorded 47 GPU telemetry samples at 500 ms, maximum
202.86 W, 66 C, and mean utilization 24.21%; individual samples reached 100%.
Desktop work was present. These quota and oracle checks do not establish an
instantaneous power or SM limit, a 512-frame comparison, or a performance gain.
The frozen 9044 accepted heap-OFF 512-frame complex noise-scale-4 perspective
oracle row exactly matched the current 16-frame quota check on RGBA8 and HDR
hashes plus error checks. This establishes oracle-fixture identity only, not
timing or 512-frame parity.

At frozen HEAD `9044f255`, the original matrix has 102 CPU runs complete (20 first
batch plus 82 more) and 202 startup observations. A separate 22-run CPU timing
retake passed with qualified short-query rows; native complex owner P50 was
4.516085 ms baseline and 4.559462 ms accepted (+0.96%), with 84 exact-work
windows. The GPU batch stopped after 16 accepted heap-hooks-OFF runs; cleanup,
restoration, and relay drain were clean. No GPU baseline or paired memory
closure exists for this interrupted matrix. Fifty-six original runs remain.
Do not resume the old 72-run batch; paced work needs explicit configuration and
provenance. The full 784-suite CPU pre-commit passed 8,470/8,470 cases with no
skips or failures. It ran for 367.08 seconds (384.950861562 seconds wall time)
at one aggregate CPU, 512 MiB host RAM, and swap off. Cgroup peak reached
512 MiB; `memory.max` recorded 1,633 events, with no OOM or OOM-kill events.
This run selected non-`[gpu]` suites, but a later audit found that an untagged
Studio viewport test could initialize a real GPU device, so these counts do not
prove device-free execution.

The native workflow [39773612336](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39773612336)
completed Linux, Windows, and macOS successfully at `9044f255`. It covers the
production source at that revision, not the new GPU budget patch, so native
verification of the patch remains open. Final static gates passed: clang-format
21 on seven changed C++ files; source-check with zero open findings in the
source tree and all 13 fixtures passing; docs-pages-check; component documentation for 267 components;
architecture across 55 modules, 6 programs, 41 layered targets, and 6 fixtures;
and em-dash-check over 3,125 files. The combined static run took
104.280237364 seconds under 50% CPU, 512 MiB, and swap off; peak memory was
157,810,688 bytes, with no memory pressure or OOM. Pass 2, pass 3, remaining
matrix rows, final gates, live checks, and user review remain open. No roadmap
checks or completion are claimed.

## Progress note 2026-10-22: GPU budget suite extension

The six-C++-file quota extension was reviewed GO on base `5debef7c`; it adds
exact support for imagegraph GPU, grid composite, computegraph, and script
compute bridge suites. Two bounded builds passed with 50% of one CPU, 2 GiB,
and swap off: `bench` completed 11 edges in 77.848726640 seconds with peak
1,000,509,440 bytes; CI `test_render` completed 3 edges in 16.703002847 seconds
with peak 1,270,149,120 bytes. CPU checks passed 6 cases/1,151 assertions, and
three malformed, missing-suite, or unsupported-suite preflight requests exited
2 before device initialization. GPU unit tests do not use this quota.

Short integration checks used one aggregate CPU, 2 GiB host RAM, swap off, and
a 45-second watchdog, with no OOM or memory pressure:

| Check | Result |
|---|---|
| Native compact, 65,537 elements, serial, 128 batches | Full byte oracle passed; 4.623793482 s wall; 138 bursts, A=1.043278535 s, required idle=3.129835605 s, actual wait=3.159058910 s, maximum burst=0.255627426 s, peak=102,854,656 B. |
| Both VMs, T2/W3, 16 cold/retained cohorts | Passed; 24.928184427 s wall; 73 bursts, A=6.139683632 s, required idle=18.419050896 s, actual wait=18.442198090 s, maximum burst=0.480390252 s, peak=106,582,016 B. |
| Image, T1/W0 cold, empty refusal, and simple | Passed in 1.573323317 s; 11 bursts across 1x1 and 256x256 pixel fixtures with full-pixel oracles. Empty dispatch and allocation were zero. |
| Grid, 1,920x1,080, T1/W0, 4 measured frames per path | With `MONO_B0_TRIALS=1` and `MONO_B0_WARMUPS=0`, the eight paths covered four populations x two paths, 32 measured renders total, plus excluded flushes and parity. Existing full parity oracles passed; 1.773513775 s wall; 73 bursts, A=0.343566813 s, required idle=1.030700439 s, actual wait=1.058562023 s, maximum burst=0.166818800 s, peak=115,585,024 B. |

All four checks exited zero with no active GPU work remaining; cumulative burst
rows matched the required 3A idle. These are integration checks, not pass 2 or
pass 3 closure, a gain, tail coverage, or a memory proof. Full CPU pre-commit
completed 784 suites and 8,470/8,470 cases with zero skips or failures in
417.78 seconds runner time (442.272534755 seconds wall time). It used one CPU,
512 MiB, and swap off; cgroup peak was 536,870,912 bytes, `memory.max` recorded
1,940 events, and OOM/OOM-kill counts were zero. Log:
`.cache/build/ci/gpu-budget-extension-precommit.log`; report:
`.cache/build/ci/gpu-budget-extension-precommit/test-output.md`.

The first combined static-doc run generated the site, and source, docs-pages,
and em-dash checks passed, but public-entity coverage found 17 missing
comments in private `BenchGpuPacing.hpp`, which is part of Doxygen input. A
reviewed comment-only header fix added the missing documentation without
changing methods, expressions, API, field order, types, or initialization. The
final `just preset=ci docs-check` regenerated the site and passed with “every
public entity is documented.” It took 372.374944471 seconds under 50% CPU,
512 MiB, and swap off; peak was 536,870,912 bytes, `memory.max` recorded 279
events, and OOM/OOM-kill counts were zero. Log:
`.cache/build/ci/gpu-budget-extension-docs-final.log`. Prior CPU/GPU checks
remain applicable to the unchanged behavior. Broad sanitizer coverage remains
incomplete, and GPU unit tests do not use the pacing setting.

A six-target global ASan+UBSan `-O1` build hit the 2 GiB cgroup limit in the
vendored HarfBuzz amalgamation, compiled at `-O2`; the build exited 143 with
one leaf OOM and one OOM-kill event. No sanitizer tests ran. A separate
five-target core build without server reached 71/248 steps, then was stopped
intentionally with SIGINT to serialize quota checks; peak was 959,541,248 bytes
with no OOM. `MONO_OPTIMISE_VENDOR=OFF` was not applied in those two earlier
attempts. Sanitizer validation will resume incrementally.

A later low-memory sanitizer build completed at
`2026-10-22T14:49:14.195658+00:00`: six targets (`test_graph`, `test_spatial`,
`test_physics`, `test_server`, `test_testrunner`, and `testrunner`), 1,634 steps,
and 4,370.502938317 seconds under one job, 2 GiB `memory.high`/`memory.max`,
and swap off. CPU quota started at 50%, then moved to 100% of one aggregate CPU;
there were also brief frozen intervals, so this duration is not a speed
comparison. Peak memory was 2 GiB
including file cache; the leaf `memory.max` counter recorded 44,751 events, with
zero OOM or OOM-kill events. This retry used `MONO_OPTIMISE_VENDOR=OFF` and
completed HarfBuzz within the cap. Scoped ASan/UBSan suites passed: Spatial 96 cases/2,270 assertions,
Graph 18/2,437, Physics 56/1,911, Server `[server][work-budget]` 1/5, and runner
`[cascade]` 15/38, totaling 186 cases/6,661 assertions. These are scoped checks,
not full sanitizer coverage. Production was at `497ee80a` with unrelated
Viewport registration still pending.

Runner `--list --no-report --jobs 1` failed at 512 MiB with exit 137/-9 and one
leaf OOM/OOM-kill event. Repeating at 1 GiB, one CPU, and swap off passed with
133 rows in 3.267084757 seconds; peak was 1 GiB including file cache,
`memory.max` recorded 1,133 events, and OOM counts were zero. ASan and UBSan
libraries were verified. Logs are under
`.cache/build/procedural-sanitize/`: `v0261-lowmem-server-test.log`,
`v0261-lowmem-cascade-test.log`, `v0261-lowmem-runner-list.log`,
`v0261-lowmem-runner-list-1g.log`, and `v0261-lowmem-build.log`. No goal row is
complete from this note.

## Progress note 2026-10-23: test discovery and device classification

The source-reviewed discovery repair adds an actual-registry guard, propagates
`ReadSuites` failure, and registers seven missing suites. CI built all six
targets in 27 steps and 22.005073903 seconds, peaking at 1,912,139,776 bytes
with no OOM. The full CI build passed 55 steps, mostly relinking, in
41.227234696 seconds,
peaking at 2 GiB with 5,521 `memory.max` events and no OOM. Both used one CPU,
2 GiB, swap off, and one job. Focused CPU checks passed 57 cases/17,083
assertions: discovery/execution 13/414 plus 44 new registered cases/16,669.
`test_studio` and `test_ui` built in five steps. Logs:
`.cache/build/ci/v0261-discovery-{build-tests,build-all,focused-c}.log`.

The first 792-suite pre-commit was interrupted with exit 130 after source audit
found untagged `studio.viewports` initialized a real GPU through
`Editor::Initialise` to `Renderer::Initialise`, even headless; that suite had
already completed in 6.74 seconds. This run is not claimed GPU-free or complete.
The owned scope was terminated and confirmed inactive. Sixteen minimal `[gpu]`
tags were then added: 13 Viewports, 2 DataFactoryHost, and 1 hidden UIInterface.
No test bodies or prior tags were removed, and no cases were newly budgeted.
The associated `Suite.hpp` comment was updated after compiled bodies, with no
C++ logic changes.
`PlayLink` was unchanged: source-init matching had attributed a later helper to
that CPU case, which does not initialize a device.

The no-driver proof used temporary nonexistent driver manifests with
`VK_DRIVER_FILES` and `VK_ICD_FILENAMES`, plus
`VK_LOADER_DRIVERS_DISABLE=*` and `VK_LOADER_LAYERS_DISABLE=~all~`. The system
loader returned null and -9 from `vkCreateInstance`; a known Studio GPU case
failed as expected because initialization returned false, with SDL reporting
unsupported Vulkan before device creation. Logs:
`.cache/build/ci/v0261-no-driver-{probe,engine-proof}.log`.

The next guarded 792-suite pre-commit exited 1 after all suites ran: 791 were
green. The report records 8,500/8,504 cases passed, 3 failed, and 1 skipped;
runner time was 356.84 seconds and outer wall time 374.790606544 seconds. It
used one CPU, 512 MiB, and swap off, peaked at 512 MiB, and recorded 2,422
`memory.max` events with no OOM. The only failure was three untagged
`client.mcpcontract` cases that spawned a headless real-GPU client. SDL's
unsupported-Vulkan error confirms that the no-driver guard blocked device
initialization. Those three cases now have `[gpu]` tags, preserving their test
bodies and bringing the total tagged cases to 19. One audio case was skipped
because it requires `SDL_AUDIODRIVER=dummy`; the passing rerun enabled that
opt-in rather than removing or suppressing the case.

After the prior build was stopped at 436/857 steps, the resumed CI all-target
build passed 422 steps in 548.070988919 seconds under one CPU, 2 GiB memory
limits, swap off, 256 tasks, and one job. Peak memory was 2 GiB including file
cache; `memory.max` recorded 7,922 events, with no OOM or OOM-kill. This is not
a speed baseline. Log: `.cache/build/ci/v0261-commit-build.log`.

The protected 792-suite CPU pre-commit then passed 8,501 cases with no skips or
failures in 354.84 seconds (372.802112379 seconds outer wall time), under one
CPU, 512 MiB memory limits, swap off, and one job. It used
`SDL_AUDIODRIVER=dummy` and retained the Vulkan driver/layer overrides. Peak
memory was 512 MiB including file cache; `memory.max` recorded 1,344 events,
with no OOM or OOM-kill. Log: `.cache/build/ci/v0261-commit-precommit.log`.
Static checks also passed: clang-format 21, em-dash check over 3,127 files,
source-check across 13 fixtures, 2,647 files, 275 components, 9 crossings, and
606 headers, plus docs-pages 51/51. The selected bench-preset runner build
passed four steps in 9.983500669 seconds under one CPU, 2 GiB, swap off, and
one job; peak memory was 520,228,864 bytes. Both CLI entry points passed the
negative-discovery check using temporary fake children that exited 19: each
returned 1 with empty stdout and stderr naming the binary, exit code, and
original diagnostic, without a partial registry. This check took 0.067411248
seconds under one CPU and 512 MiB, peaking at 17,776,640 bytes; its raw evidence
was kept in RAM. The final commit checks launched no benchmarks or GPU
workloads. The earlier 8,470-case counts remain historical and do not prove
device-free execution. The GPU unit-budget draft remains unapplied. GPU unit
tests remain outside this quota, and the broader GPU matrix and milestone are
incomplete.
