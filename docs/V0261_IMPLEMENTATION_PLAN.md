# v0.26.1 implementation plan

## Status and scope

This is the approved execution plan for the v0.26.1 work listed in
[`NEW_ROADMAP.md`](../NEW_ROADMAP.md). The shared graph module, target layer,
module policy updates, and adapter migrations have user approval. Remaining
implementation and evidence gates are tracked below. No GPU checks or
optimization gains are claimed here.

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
`v0.26.5-imagegraph-full-archive` at `27a50cd2` is reference history only. Do not import its full runtime into this work. The version file currently
says `0.26.0`; update it to `0.26.1` only as part of authorized implementation.

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
evidence is recorded below. No full milestone verification, GPU checks, or
optimization gains are claimed. Both baseline commands passed:

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
| [imagegraph/AGENTS.md](../mono.engine/imagegraph/AGENTS.md) | “This module links only `Engine::core`.” | Approved: permit `Engine::graphcore` for common bounded topology compilation. |
| [bakegraph/AGENTS.md](../mono.engine/bakegraph/AGENTS.md) | “`Engine::core` and nothing else” | Approved: permit `Engine::graphcore` for common bounded topology compilation. |
| [audio/AGENTS.md](../mono.engine/audio/AGENTS.md) | “Do not "unify" them until `graph` has an executor and physics uses it.” | Approved: permit shared schema and topology compilation while retaining audio's sample mixer, clock, and runtime. No physics consumer is proposed. |
| [render/AGENTS.md](../mono.engine/render/AGENTS.md) | “A frame is recorded by one thread, and that is a contract” and “Revisit when a viewport's *record* is measurable” | Approved subject to profiling prerequisite: permit isolated private worker recording only after owner-thread profiling proves a recording cost. Public renderer ownership, shutdown, resource publication, joins, and final composition remain on the owner thread. |

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

Measure complete owner-thread recording first. `Renderer.cpp` currently advertises `UnifiedQueue = true`; the vendored SDL path is reported to create
one Vulkan queue. Queue-family capabilities are not yet established. The available GPU is reported as an RTX 4090, while Nsight tools were not found
on PATH. Recheck hardware, drivers, backend and tools before choosing a prototype.
Read-only `/proc/driver/nvidia/params` reports `RmProfilingAdminOnly=1`; Nsight is not installed. Tool installation alone would not prove trace
access. Keep capture permission unknown until a concrete capture succeeds. See NVIDIA's [performance-counter permissions note](https://developer.nvidia.com/ERR_NVGPUCTRPERM). No root driver configuration change is planned.

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
| Coherent CI build | The selected product and test targets passed across 137 build steps. |
| Full CTest | `script`, `scripthost`, `procedural`, `bake`, `examples`: 5/5 passed in 51.98 s. |
| Latest CTest | `script`, `scripthost`: 2/2 passed in 14.84 s. The earlier procedural/assets/bake/assetc run passed 4/4 in 1.29 s. |
| Focused tests | Compute: 339 assertions/19 cases; scripting compute, cadence and catalogue: 390/6; mesh: 182/26; procedural: 3,818/38; script procedural and codec: 2,933/42; bake: 2,338/5; scripthost compute/lifetime: 100/5. All passed. |
| Latest focused validation | Native procedural: 3,596 assertions/33 cases passed, including the previously failing Generate rollback coverage. Neutral service and serial Studio/Game coexistence: 106 assertions/4 cases passed. Signed RGA-to-Cook-to-CDN-to-directory delivery and unbound generator-free install: 171 assertions/1 case passed, including stale input, download edit, misnamed mesh last-good, and retry cases. |
| Bindings and component documentation | Bindings generation/check passed for 3 artifacts. `components-check` passed for 266 components, with 0 missing purposes and no serialized padding leaks. The full 131-script Luau check passed against current bindings, and Studio TypeScript passed. The new ProceduralService has 13 methods. |
| C06 real forest | ProceduralVM Luau and JavaScript produced identical real-forest hashes, 84 assertions/3 cases. This is C06 evidence, separate from C02 service results. |
| C06 benchmark harness | Eight diagnostic scenarios and context oracles passed in a concurrent O0 CI trial. This is functional harness evidence, not timed B0 performance evidence. A dense O0 C02 harness did not finish within its 180-second deadline and had no oracle failure; optimized rerun is pending. |
| Compiler and review status | Benchmark compiler is GCC 13.3.0. The Generate rollback failure is fixed in the current procedural result. The Studio role-mode conflict is fixed; serial Studio/Game coexistence is covered by the passing service tests above. |
| C01 and startup on real Vulkan | `.cache/build/ci/compute-bounded-and-startup-validation.log`: 360 assertions/12 cases passed on RTX 4090, NVIDIA 580.173.02, with zero VUIDs or validation errors. Earlier pre-metadata poll passed 470/12; poll loops vary assertion counts. The prior 56 teardown and 3 partial-startup VUID reproductions are fixed in the current path. |
| Headless regression | The old SIGSEGV case passed after the JavaScript self-disconnect fix: 16 assertions, 1 case. |
| Isolated SDL sidecar | Opt-out/opt-in transfer-to-compute-to-graphics exact parity and synchronization validation passed cleanly. This separate sidecar result does not establish production integration or physical overlap. |
| AudioGraph heap | Constructor: 12,734,480 bytes and 17 blocks. Legal 1,024-node/1,023-input mutation/rebuild added 0 blocks/bytes. This does not prove full mixer allocation freedom. |
| C04 recording and quiet benchmark | The earlier emitter recording measured 1,000 to 100,000 emitters, each with 6 particle slots, and one changed emitter; step-record self time was 4 to 10 us, draw self about 1 us, and whole render 150 to 223 us unchanged or 323 to 359 us for 10,000 emitters with one change. No dropped spans. The approved RTX 4090/Vulkan 580.173.02 GCC 13.3.0/O3 quiet benchmark used 128² scenes with 1,024 actual meshes per view, 11 cohorts, 16 warmups and 16 measured samples. Whole Render was 0.511 to 0.567 ms for 1 view, 0.915 to 1.029 ms for 2, and 3.446 to 3.721 ms for 8. `drawSlots` self time was 0.264 to 0.287, 0.531 to 0.559, and 2.104 to 2.204 ms respectively. There were no dirty rows, uploads, allocation churn, or CPU drops. Eight-view GPU marks were 296, so GPU timing is partial. The profiling prerequisite is met and the previously approved renderer worker implementation may proceed; these results do not establish worker benefit or physical overlap. |
| C01 native GPU benchmark | Six cases passed exact CPU checks. GPU timing was unavailable and the latest mean/tail metrics are partial, so the timing matrix is incomplete. |
| Remaining | C06 native scene/bake/authored inputs, shared assets PRD1, baked loading, signed client delivery and real-forest hash parity have validation; Studio UI remains. C02 simple native rows and service/serial Runtime checks passed, while UI, queues and ray B0 work remain. The dense O0 harness timed out at 180 seconds with no oracle failure; an optimized rerun is pending. B0 and all three optimization passes remain incomplete. |

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
