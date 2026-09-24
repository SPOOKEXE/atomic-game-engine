# v0.25 render pipeline cleanup plan

## Purpose

The render path has become capable enough that its main cost is now understanding where a decision belongs.
This plan makes graph meaning, render execution, GPU lifetime, world presentation, and Studio authoring explicit
ownership areas. It is a cleanup plan, not a proposal to replace the renderer or change the authored pipeline
format.

The first rule is to preserve a working renderer after every move. Extract private implementation units inside
`mono.engine/render` before considering a new module. The current `render` dependency closure deliberately owns
the SDL device, shaders, resources, and presentation adapters. Splitting it into libraries before stable cut
points exist would add architecture edges without reducing the work needed to understand one frame.

## Implementation status

The cleanup has delivered the private compiler, registry, and executor package, with compiler and renderer
admission parity tests. `FrameBatch` and `GraphResourceCache` now own their frame and resource lifetimes.
Presentation helpers isolate collection and damage work. The Studio document adapter and its canvas, inspector,
schedule, and profile panels are separated. Portal owner grouping and the host operation boundary make the
topology, demand, capture, composition, and residency paths explicit. Focused tests cover these extracted seams.

Remaining limitations:

- The approved `just preset=ci render-check` rerun on September 24, 2026, passed all 132 GPU cases with 1,677,900 assertions. The fixture and expectation corrections are recorded in [the roadmap audit](v025-roadmap-audit.md).
- A later dev Vulkan `test_render '[render][gpu]'` gate on September 24, 2026, passed 150 cases with 1,678,712 assertions. Seven selected host, source, import, tree, and compiler-parity cases passed 1,076 assertions. The focused producer retirement fixture passed 78 assertions across three cases, covering explicit clear, binding replacement, endpoint close followed by host-owned clear, queued capture timeout, and pending retained-shadow fit cancellation. These gates used the dev renderer with assertions enabled; they do not replace the earlier CI result.
- The private presentation-source descriptor matrix passed 1,979 assertions in one dev test case. It covers all 15 declared component-to-revision mappings, add and remove invalidation, reported writes for the 14 authorable components, quiet second frames, and unique revision slots. `scene::Rendered` is derived by `SyncRendered`, so its case checks membership and steady sync without authoring its value.
- The device-free compiler/admission parity table passed its original five rows with 39 assertions and seven additional capability rows with 43 assertions. The added cases pin compute, storage-texture, and RGBA16F requirements for a valid dispatch, RGBA16F for blit, and validation precedence when an invalid blit format coincides with missing device format support. They compare `Build`, graph compile, scheduling, and exact renderer stage, offender, and reason without changing compiler behavior.
- `PortalRenderOperations` routes portal image and tree capture, upload, composition, polling, cancellation, and release through a private operation boundary. The host call ledger observes one release attempt per composed image across six retirement causes; shadow transport mode 5 observes one composition cancellation, and modes 10 and 11 observe one preparation cancellation after a duplicate call was reproduced and fixed in `AbortBody`. A separate assert-only renderer ledger observes first resource-image cancellation, portal-image drop, image replacement, physical imported-handle release, capture-tree revocation, and tree-lease release. Focused GPU cases now cover producer clear and binding replacement, endpoint retirement followed by host-owned clear, queued capture timeout, pending retained-shadow fit cancellation, repeated and deferred camera demand while a composed image remains owned, host image retirement, source replacement and withdrawal, resident cancellation/invalidation/expiry/clear, tree leases, imported-image world retirement, and renderer shutdown. Replacing a composed image retires the old handle during resident adoption, so the later host drop is an attempted call with no second applied retirement. Direct producer `Pump` after an endpoint closes is not the host retirement transition: the host must clear its producer. The unchanged-demand case pins six subsequent retirement causes; a separate changed-demand case pins latest and revert paths. These do not constitute an exhaustive owner-state proof. Resident ownership passes through `OwnsResident`, and resource-image cancellation and revision reads use exact signatures. Generic forwarding methods and `RendererRef()` remain for editable resource refresh, shader preparation, and nested source construction.
- The strict CPU `just preset=ci check` gate passed on September 24, 2026, with 629 suites and no failures.

The original investigations remain investigation items unless their own focused evidence closes them.

## Baseline map

```mermaid
flowchart LR
    Author[PipelineDocument\nauthored edits] --> Graph[Engine::graph\nvalidate, compile, schedule, alias]
    Studio[Studio node editor\nload, save, inspect] <--> Author
    Graph --> Install[render pipeline admission\ncapabilities and backend availability]
    Install --> Runtime[Renderer batch runner\nviews, mutations, command buffers]
    Runtime --> Passes[built-in and custom node dispatch]
    Runtime --> Gpu[targets, resource images, meshes, shaders]
    World[scene and ECS] --> Present[WorldPresentation\ndraw rows and damage]
    Present --> Runtime
    Runtime --> Profile[profile, previews, visibility]
    Profile --> Studio
```

At the start of this cleanup, the ownership direction was sound: `mono.engine/graph` was device free and
`mono.engine/render` consumed its compiled result. The implementation did not consistently retain that boundary:

| Area | Current source | Cleanup concern |
|---|---|---|
| Graph model and static planning | `mono.engine/graph/include/engine/graph/RenderGraph.hpp`, `src/RenderGraph.cpp`, `src/Schedule.cpp`, `src/ExecutionPlan.cpp` | The graph holds validation, scheduling, alias planning, and profile types, but renderer admission repeats part of this policy. |
| Node vocabulary | `mono.engine/graph/src/PipelineCatalogue.cpp` | Catalogue metadata is separate from the renderer's backend dispatch table in `mono.engine/render/src/RenderPipelines.cpp`. |
| Pipeline admission and execution setup | `mono.engine/render/src/RenderPipelines.cpp` | One 2,450 line file compiles, checks, partitions, installs, retains, and dispatches pipelines. |
| Frame orchestration | `mono.engine/render/src/Renderer.cpp`, `src/RendererState.hpp` | One 2,896 line file owns batch setup, grouping, hook mutation, targets, history, submission, and recovery. |
| World to draw rows | `mono.engine/render/src/WorldPresentation.cpp`, `src/ViewportFrames.cpp` | Incremental cache invalidation is tightly coupled to a positional revision array and duplicated component lists. |
| Pipeline authoring UI | `mono.studio/src/RenderPipeline.cpp`, `src/RenderPipelineGraph.cpp` | Editor load/save reconstructs the document separately from graph build and shows several inspector views from one stateful file. |
| Portal rendering | `src/PortalImageRuntime.cpp`, `src/PortalImageHost.cpp`, `src/PortalExchange.cpp`, `src/PortalCaptureTree*.cpp` | This is a distinct state machine spread across transport, ownership, preparation, composition, and renderer calls. |

The line counts are a prioritisation signal only. `PortalImageRuntime.cpp` is about 3,900 lines, `Renderer.cpp`
about 2,896, `RenderPipelines.cpp` about 2,450, `WorldPresentation.cpp` about 1,997, and
`RenderPipeline.cpp` about 1,025. Moves need to follow cohesive state and tested behaviour, not file size alone.

## Target ownership

Keep public `Renderer` as the device-facing façade. Its private implementation should have these named owners:

| Private area | Responsibility | Candidate files |
|---|---|---|
| `PipelineCompiler` | Convert an installed `RenderGraph` into one immutable execution package. Validate graph, schedule, backend support, capability requirements, command-buffer plan, aliases, entity nodes, and retained-node metadata in one result. | `src/PipelineCompiler.hpp/.cpp` extracted from `RenderPipelines.cpp` |
| `PipelineRegistry` | Install, replace, resolve, snapshot, revision, and retire named execution packages. | `src/PipelineRegistry.hpp/.cpp` |
| `FrameBatch` | Own one `Renderer::Render` batch from command acquisition through submit or abort. It owns view ordering, group state, hook mutations, lighting restoration, and history commit or discard. | `src/FrameBatch.hpp/.cpp` extracted from `Renderer.cpp` |
| `RenderNodeExecutor` | Map a node kind to executable backend work. The built-in table, custom node registration, and missing-kind reporting live together. | `src/RenderNodeExecutor.hpp/.cpp` |
| `GraphResourceCache` | Own graph resource images, history generations, alias-backed allocation, previews, and release. | Extract from `RenderTargets.cpp`, `ResourceImage.cpp`, and `RendererState.hpp` only after its lifecycle is specified. |
| `PresentationCollector` | Turn ECS and scene state into presentation rows, damage, and viewport frames. | Private files around `WorldPresentation.cpp` and `ViewportFrames.cpp` |
| `PortalRenderCoordinator` | Coordinate portal demand, topology, transport, image residency, and tree composition. It calls `Renderer` through a narrow private operation interface. | Private headers around existing `Portal*.cpp` files |

These are folders and private headers, not new public modules. For example, `src/pipeline/`, `src/frame/`,
`src/presentation/`, and `src/portal/` improve locality without widening the engine graph. Do not move generic
GPU wrappers out of `render` until a second, real device consumer needs them.

## Prioritised work

### P0: establish contracts before moving code

1. Define an immutable private `InstalledPipeline` result produced by one compiler entry point. It must contain
   the `graph::RenderGraph`, `CompiledGraph`, `ExecutionSchedule`, `ResourceAliasPlan`, planned command buffers,
   entity-node set, retained-node metadata, pipeline name, and revision.

   Extract the duplicated assembly now in `Renderer::InstallEngineDefault` and the named-pipeline installation
   paths in `mono.engine/render/src/RenderPipelines.cpp`. `Renderer::RenderGraphSnapshot` in
   `mono.engine/render/include/engine/render/Renderer.hpp` should be made from this package, rather than being a
   second manually maintained projection.

2. Make backend support a first-class compiler input. Today `CompileRenderPipeline` builds a local `NodeTable`,
   adds custom kinds, then checks missing kinds. Keep `NodeCatalogue` in `Engine::graph` as authoring metadata and
   inject a `RenderNodeExecutor` capability table from `render`. Do not add SDL or render dependencies to graph.

3. Give pipeline admission one failure type that preserves the offender, stage, and reason. Existing callers
   currently shuttle `core::Name` plus a string through multiple graph and renderer APIs. A value result makes
   Studio, MCP, logs, and tests describe the same refusal.

4. Characterise the current default PBR graph before extraction. Compare its document, compiled order, execution
   waves, command-buffer plan, alias plan, retained nodes, and profile schema. This is the baseline for every P0
   move.

### P1: separate planning from frame execution

1. Extract `PipelineCompiler` and `PipelineRegistry` without changing the public `Renderer` API. Delete the old
   helper paths after all install callers use the package. Avoid a base class or a generic pass framework.

2. Extract `FrameBatch` from `Renderer::Render`. Give it explicit terminal outcomes: skipped before acquisition,
   submitted, submitted after a recoverable view failure, and aborted. Move cleanup for `BatchActive`, command
   ownership, pending graph history, visibility snapshots, mutation restoration, and lighting restoration into
   that owner.

3. Split `RenderPipelines.cpp` by ownership: compiler and registry first, then executor and built-in pass family
   bindings. Keep pass algorithms in existing focused files such as `ScenePasses.cpp`, `InterfacePass.cpp`,
   `RenderParticles.cpp`, and `RenderLod.cpp`.

4. Rename vague internal terms when extracted. Prefer `InstalledPipeline`, `PipelineCompiler`,
   `FrameBatch`, `FrameViewGroup`, `GraphResourceCache`, and `PortalRenderCoordinator`. Retire `Impl::NamedPipeline`
   once the registry takes ownership. Do not rename public authored node kinds or pipeline document fields in this
   cleanup.

### P1: make presentation invalidation declarative

1. Replace `WorldPresentation::SourceRevisions` and its matching enum in
   `mono.engine/render/src/WorldPresentation.cpp` with named revision fields or a single declared table that owns
   the component type, revision slot, and full versus pose damage class. The present v0.25 LOD change expands the
   array from 14 to 15 and adds matching calls in several locations. It works, but future omissions compile.

2. Put the drawable component collection and `scene::ApplyDrawRenderState` input list behind one private
   `PresentationSource` function. `ViewportFrames.cpp` currently has another explicit component tuple. The goal is
   one declaration that supplies both viewport copying and world presentation collection.

3. Keep `WorldPresentation` as the boundary from ECS to copied draw data. Do not push GPU resource handles into
   scene components or give graph code renderer residency responsibilities.

4. Extract damage calculation, source-row collection, skin pose collection, and final row ordering into private
   files only after tests pin each contract. The current 1,997-line implementation should become a coordinator,
   not a collection of unrelated static helpers.

### P2: remove duplicated document and editor projection rules

1. In `mono.studio/src/RenderPipelineGraph.cpp`, isolate conversion between `graph::PipelineDocument` and
   `nodegraph::Graph` in a small adapter with a round-trip contract. Keep nodegraph layout data as authoring edits,
   and keep graph validity in `Engine::graph`.

2. Split `mono.studio/src/RenderPipeline.cpp` into document session state, canvas actions, inspector, schedule
   panel, profile panel, and resource preview bridge. `DrawRenderPipeline` should coordinate these pieces and not
   also contain save policy and diagnostics formatting.

3. Consolidate label, scope, resource lifetime, resolution, and parameter conversion helpers. The Studio adapter
   should use `NodeCatalogue` names and metadata whenever possible. It must not create a second schema for a
   render node.

4. Keep `PipelineDocument` as the canonical persisted representation. Do not persist nodegraph runtime IDs,
   compiled schedules, alias placements, previews, or process-local `core::Name::Id()` values.

### P2: make portal coordination comprehensible

1. Preserve the following state and ownership boundaries before moving code. Each row has one owner and one
   terminal retirement operation; helpers may request retirement but must not free another row's state directly.

   | State | Sole owner | Retirement operation |
   |---|---|---|
   | Endpoint validity | `PortalTopologyHost` | `PortalTopologyHost::RemoveWorld` removes the source, destination, and producer endpoint records for a world. |
   | Demand | `PortalImageHost::Impl::Source::Portals` | `PortalImageSource::InvalidatePortal` revokes a portal demand before `RetireCompositions` observes and removes its dependent composition. |
   | Source capture | `PortalImageSource` | `PortalImageSource::Clear` cancels outstanding uploads and releases every preview and capture lease. |
   | Prepared body | `PortalImageHost::Impl::PreparedBodies` | `Renderer::CancelPortalCaptureTreePreparation` cancels the preparation before the host erases the body record. |
   | Shadow route | `PortalImageHost` | `ReleaseShadowRoute` releases the route and sends its cancellation before the record is discarded. |
   | Composition job | `PortalImageHost::Impl::Source::Compositions` | `RetireCompositions` releases its route and preparation, then calls `Renderer::DropPortalImage`. |
   | Resident image lifetime | `PortalResidentImages` | `PortalResidentImages::Expire` cancels the resource-image token at its deadline and erases the entry. |

   `PortalImageHost::Clear` is the shutdown path: it retires bodies, sources, producers, resident images, and
   topology in that order. `Renderer::CancelPortalCaptureTreeComposition` remains the renderer-side terminal
   operation for an in-flight composition, including its imported shadow and node images.

2. Group existing portal files by that state machine under `src/portal/`, preserving private includes and the
   render module boundary. Move a state and its tests together in small changes.

3. Narrow portal calls into `Renderer` to explicit operations for capture, composition, upload, polling,
   cancellation, and release. The coordinator should not reach through general renderer state.

4. Keep copied `world::PresentationAddress` traffic at this boundary. Do not introduce shared world or ECS
   pointers into portal work.

## Investigations required before behaviour changes

These are review findings, not confirmed defects. Reproduce or disprove each one with a focused test and a trace
before changing logic.

| Investigation | Evidence and risk | Required evidence |
|---|---|---|
| Scheduler producer choice follows authored node order | `mono.engine/graph/tests/Schedule.cpp` pins the last authored writer, disabled writers, history, multiple writers, reader-before-next-writer ordering, and world/view/frame scope boundaries. `mono.engine/render/tests/GraphRunner.cpp` executes a compiled two-view, two-writer graph through backend dispatch: readers see the first writer before overwrite and the second afterward, or the first again when the second is disabled (28 assertions). Its history case dispatches a prior-generation read before two same-frame writes across two invocations (12 assertions), and its scope case observes two worlds across three views before one final frame node (22 assertions). The history store is modeled in the test; this is scheduling and dispatch evidence, not GPU aliasing evidence. | Verify actual GPU history storage/aliasing where resource-backed lifetime behavior matters, and extend only if a concrete scope/output gap appears. |
| Graph compiler and renderer compiler can drift | `RenderGraph::Compile`, `CompileSchedule`, and renderer admission each partition or validate different aspects. The focused parity table pins graph read-before-write, invalid schedule hint, missing backend, renderer-only shadow dependency, compute/storage/format capability failures, and validation precedence. `Build` collapses graph and schedule failures to `Invalid`; its missing-producer offender is the resource, while renderer graph admission names the node. | Extend the parity table when backend kinds or capability rules change, preserving shared diagnostics and explicitly naming renderer-only stages. |
| Catalogue metadata and backend dispatch are separate registries | `PipelineCatalogue.cpp` and the executor remain distinct, but `mono.engine/render/tests/GraphRunner.cpp` now checks every built-in catalogue kind against backend support and the reverse direction, with private test kinds exempted. | Keep the exhaustive parity test current when a node kind is added or retired. |
| Pipeline document round trip can lose intent | `mono.studio/tests/RenderPipelineGraph.cpp` has golden round trips for resources, unknown data, binding order, group, comment, mute, preview, partial records, signed numbers, and a schedulable world document. | Keep those golden cases current when editor save policy changes. |
| Frame early exits may leave batch-owned state asymmetric | `mono.engine/render/tests/FrameBatch.cpp` now injects refusal before acquisition, failure before the first and second groups, and final submit failure. The GPU cases check submission attempts, active state, history, visibility, lighting, mutations, and recovery. | Extend the fixture when a new group transition or acquisition outcome is introduced. |
| Presentation cache tracking is index coupled | Named `DrawList::SourceRevisions` fields and the private `SOURCE_TABLE` bind all 15 component types to revision slots and damage classes. `mono.engine/render/tests/PresentationSourceDamage.cpp` drives add, remove, reported write, first observation, and steady second frame from that same descriptor table, with the `SyncRendered` derived-value exception. | Keep the descriptor matrix and world-level cache tests current when a source component or revision policy changes. |
| LOD GPU layout and shader stay lockstep | `GpuLodSelection` is 96 bytes and carries `MinimumDistances` consumed by `lod-select.comp`. C++ assertions and the staged SPIR-V reflection check the layout. `mono.engine/render/tests/LodGpu.cpp` compares captured output for inherited zeros, valid and invalid overrides, and a non-uniform rotated part. | Keep the reflected layout and captured-output fixture current with shader changes. |
| Portal retirement paths are complete | The host ledger pins image-release and job-cancel calls, including the fixed duplicate preparation cancellation. An assert-only renderer ledger pins first terminal transitions in focused host, source, resident-table, tree-lease, producer clear and binding replacement, endpoint close with host clear, queued capture timeout, pending retained-shadow fit cancellation, import-world-retirement, and renderer-shutdown GPU cases. The real-host unchanged-demand fixture asserts no premature release while one layered demand coalesces, followed by one effective image drop in each of six retirement branches (292 assertions in one Catch case). A separate changed-camera fixture asserts that a deferred latest demand or a revert retains the composed image through capture acceptance and drops it once on explicit removal (74 assertions in one Catch case). It distinguishes physical imported-handle release during adoption from a later ineffective host drop. Three more real-host Vulkan cases retain the last composed image through immediate same-owner supersession (28 assertions), cancel three staged imported handles exactly once while retaining that image until explicit removal (34 assertions), and retire old image and composition once on an incompatible binding index before accepting the replacement (28 assertions). The nested-tree transport integration adds a viewport-removal branch that cancels a pending body job once and applies one tree drop; all 15 generated modes pass (2,283 assertions in one Catch case). | Audit actual staged upload failure and accepted changed-eye cancellation while a body job remains pending before claiming exhaustive terminal accounting; retain the one-effective-release rule for every owned image and job. |

## Test plan and gates

Keep focused tests beside their owners. The repository already has useful seams:

| Contract | Existing starting suites | Add or strengthen |
|---|---|---|
| Graph validity, schedule, aliases, documents, diagnostics, profiles, layout | `mono.engine/graph/tests/RenderGraph.cpp`, `PipelineDocument.cpp`, `PipelineCatalogue.cpp`, `EngineGraph.cpp`, `PipelineDiagnostics.cpp`, `PipelineProfile.cpp`, `PipelineView.cpp` | Compiler result snapshots, catalogue-executor parity, multi-writer scheduling, document canonical round trips. |
| Renderer pipeline admission and graph history | `mono.engine/render/tests/GraphRunner.cpp`, `GraphHistory.cpp`, `Passes.cpp`, `CustomNodes.cpp`, `Capabilities.cpp`, `ResourceImage.cpp` | Install replacement, failed admission preservation, revision and snapshot consistency, alias and retained-resource retirement. |
| Frame cleanup and visible output | `HardRender.cpp`, `HardRenderGpu.cpp`, `RenderStageProbe.cpp`, `RenderImageComparison.cpp`, `VisibilityObservation.cpp` | Failure-injection state machine test and a small golden default PBR image fixture. Keep GPU checks opt-in and deterministic. |
| Presentation and viewports | `WorldPresentation.cpp`, `PresentationDamage.cpp`, `PresentationSchedule.cpp`, `ViewportFrames.cpp` | Declaration-table invalidation cases and copy parity between viewport and collector. |
| Portals | `PortalImageHost.cpp`, `PortalCaptureTree*.cpp`, `PortalExchange.cpp`, `PortalTopologyHost.cpp`, `PortalResidentImages.cpp` | State-transition ledger tests and full terminal-release sequences. |
| Studio | `mono.studio/tests/RenderPipelineGraph.cpp`, `NodeGraph.cpp`, `Presentation.cpp` | Document round-trip golden tests, inspector status rendering, schedule and profile display snapshots. |

Every extraction change must pass its narrow suite first, then the normal repository gates in `docs/CODE_QUALITY.md`:

1. `just format-check`
2. `cmake --preset dev` and `cmake --build .cache/build/dev -j`
3. `cmake --preset server` and `cmake --build .cache/build/server -j`
4. `ctest --output-on-failure` in both build directories
5. `just check-server-is-headless`, `just test-architecture`, and `just components-check` when components change
6. `cmake --preset ci` and `cmake --build .cache/build/ci -j`

For rendering changes, run a headless GPU suite only at the final verification stage with user approval, as the
repository instructions require. Record the preset, backend, fixture, dimensions, and tolerance. Profile a
release build before claiming a runtime improvement; extraction alone is not a performance change.

## Delivery order

1. Land characterisation tests and the immutable pipeline package with no output change.
2. Extract compiler, registry, and executor. Delete old installation helpers in the same changes.
3. Extract `FrameBatch` behind identical `Renderer::Render` behaviour and failure-injection tests.
4. Replace positional presentation invalidation with the declared source table, then split collector helpers.
5. Separate the Studio adapter and panels with document round-trip tests.
6. Map portal state, then move one lifecycle owner at a time with terminal-release tests.
7. Reassess whether any private area has earned its own engine module. Add one only if it has a stable public API,
   at least two consumers, and an allowed layer edge recorded in `mono.tools/architecture/expected_graph.json`.

## Explicit non-goals

- Do not rewrite the linear `PipelineDocument` format in v0.25. First prove canonical round trips and isolate its adapter.
- Do not replace the graph with a generic render-graph framework.
- Do not move SDL, shader compiler, or GPU types into public headers.
- Do not merge graph planning into renderer. The existing device-free `Engine::graph` layer is valuable and should become clearer.
- Do not mix this cleanup with new visual features, pipeline schema changes, or a portal protocol redesign.
