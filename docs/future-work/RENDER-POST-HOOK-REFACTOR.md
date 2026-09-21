# Render refactor after hooks

## Status

This document plans an internal cleanup of `mono.engine/render` after the hook
system in [RENDER-HOOKS.md](RENDER-HOOKS.md) is implemented and accepted. It is
not a claim that either the hook system or this refactor ships today.

`RENDER-HOOKS.md` is authoritative for hook behavior. This document only
defines how the frame path should be split around that behavior.

The refactor must proceed in small steps. Every phase leaves the renderer
building, testable, and usable. Replaced paths are deleted as soon as their
replacement passes its gate. There must not be two permanent frame runners,
surface planners, hook owners, or pass lists.

## Why this follows the hook work

The hook work establishes one stable observation seam before the frame path is
split. That order matters. Moving the render code first would move capture
ownership and graph seams at the same time, making image or lifecycle failures
hard to assign to either change.

Complete the hook acceptance tests first. Freeze the current node names,
resource identity, snapshot alignment, and readback lifecycle as fixtures. The
frame cleanup may then move those responsibilities without changing their
contract.

## Current pressure

The renderer has already completed the first useful split. `Renderer::Render`
builds named `ViewRequest` data, `ViewRecording` prepares a view, `GraphRunner`
executes the compiled graph, and `src/nodes/*.cpp` owns node families. This
plan continues that shape instead of creating a new one.

The remaining large units mix several kinds of ownership:

| File | Current size | Mixed responsibilities |
|---|---:|---|
| `src/Renderer.cpp` | about 2,747 lines | Public facade, device operations, batch control, and view entry |
| `src/RendererState.hpp` | about 2,746 lines | Device, pipelines, residency, targets, history, imports, readback, and submission |
| `src/ViewRecording.cpp` | about 3,078 lines | Preparation, allocation, graph execution, presentation, capture, and submit |
| `src/ViewRecording.hpp` | about 668 lines | Inputs, derived view state, command state, and broad device access |
| `src/ScenePasses.cpp` | about 1,291 lines | Scene drawing, image drawing, resources, uploads, history, and profiling helpers |
| `src/nodes/GeometryNodes.cpp` | about 1,324 lines | Several geometry and transparency node backends |
| `src/nodes/MirrorNodes.cpp` | about 1,043 lines | Mirror planning, recursive recording, composition, and history |
| `src/nodes/ShadingNodes.cpp` | about 1,030 lines | Lighting, environment, post, and related bindings |

Line count is navigation evidence, not a completion target. A short file with
hidden ownership is worse than a long file with one clear job.

Three surface capture paths remain:

1. `SurfaceCapturePlan.cpp` recursively builds a mixed mirror and portal plan.
2. `SurfaceNodes.cpp` records that plan through its explicit postorder.
3. `MirrorNodes.cpp::fillMirror` and `PortalNodes.cpp::fillLevel` still recurse
   while recording GPU work.

The mixed plan is the path to keep. The other two paths should converge on it
after behavior parity is proven.

## Goals

- Keep the compiled render graph as the only frame scheduler.
- Give every mutable field one owner.
- Make dependencies visible through one borrowed context.
- Separate input, prepared view data, and command recording state.
- Make preparation, graph execution, presentation, and submission distinct calls.
- Replace recursive surface traversal with a bounded explicit frontier.
- Keep node backends in focused files with no parallel pass list.
- Preserve hook names, timing, caching, resource lifetime, and failure behavior.
- Keep device recording on the renderer owner thread.
- Reduce rebuild pressure by narrowing private headers where measurement supports it.

## Non-goals

- No new `mono.engine/renderer` module.
- No second graph runtime, pass enum, pass-order table, or callback graph.
- No generic backend hierarchy added only to appear portable.
- No SDL GPU type in `Renderer.hpp` or new SDL include in a public header.
- No new ECS mirror or private copy of authored world state.
- No parallel GPU command recording under the current single-device contract.
- No change to surface optics, portal transforms, clipping, or shader contracts.
- No texture allocation per recursive capture entry.
- No blanket removal of `std::function` where recursion is not involved.
- No file writing, script callback, or MCP call from an observation hook.

## Ownership model

### Persistent dependencies

Introduce one private borrowed dependency aggregate. It is wiring, not storage:

```cpp
struct RenderContext {
	DeviceState &Device;
	PipelineState &Pipelines;
	ResidencyState &Residency;
	ViewTargetState &Targets;
	ReadbackState &Readbacks;
	SubmissionState &Submission;
	DataFactoryHookBind &Hooks;
	RenderTelemetry &Telemetry;
};
```

The exact owner names may follow types already present when extraction begins.
The important rule is that every member names a coherent existing owner.
`RenderContext` must not become another spelling of `Renderer::Impl` with every
field copied into it.

All references are required after successful renderer initialization. Optional
features remain optional data inside their owning service. Nodes must not look
up dependencies through a global locator.

### Per-view records

Split the current `ViewRecording` fields into three records:

```cpp
struct ViewInput {
	// Borrowed caller data. Valid only during synchronous recording.
};

struct PreparedView {
	// Resolved camera, ranges, lighting, targets, and capture decisions.
};

struct RecordingState {
	// Command identity, attachment state, profiling marks, and failure state.
};
```

`ViewInput` begins as the current `ViewRequest`. It carries spans and host hook
pointers whose lifetime ends with the synchronous call. It must never cross a
thread or survive the frame.

`PreparedView` owns or borrows the values derived before node recording. This
includes the resolved camera, scene plan, accepted surfaces, lighting blocks,
draw ranges, graph selection, presentation choices, and immutable capture plan.
It must not own a command buffer.

`RecordingState` contains data that changes as nodes record. This includes the
active command, load and store state, timestamp slot, open pass, actual work
flags, pending history writes, and submission outcome.

`ViewRecording` becomes a small composed facade over these records and
`RenderContext`. A field lives in exactly one record. Transitional aliases are
allowed only inside one migration phase and are deleted at that phase's gate.

### Persistent state split

Shrink `RendererState.hpp` by moving coherent state, not by creating empty
wrapper types. The intended dependency direction is:

```text
Renderer facade
  -> render context
       -> persistent state owners
       -> per-view records
            -> pass recording helpers
                 -> node adapters
```

Persistent state must never depend on a node adapter. Hook value records must
never depend on a private device record.

## Proposed private layout

All files remain inside the existing `mono.engine/render` module:

| File | Responsibility |
|---|---|
| `src/Renderer.cpp` | Public facade, owner-thread checks, and named batch entry |
| `src/RendererState.hpp` | Thin composition of persistent state owners |
| `src/state/DeviceState.hpp` | Device, formats, fallback resources, and common samplers |
| `src/state/PipelineState.hpp` | Installed graphs, schedules, shaders, and pipeline variants |
| `src/state/ResidencyState.hpp` | Mesh, texture, instance, index, skin, and particle residency |
| `src/state/ViewTargetState.hpp` | Scene slots, PBR targets, surface banks, and capture pools |
| `src/state/ReadbackState.hpp` | GPU copies, fence polling, resource tokens, and completed images |
| `src/state/SubmissionState.hpp` | Batch command ownership and pending commit state |
| `src/recording/RenderContext.hpp` | Borrowed persistent dependencies |
| `src/recording/ViewInput.hpp` | Synchronous caller input |
| `src/recording/PreparedView.hpp` | Immutable derived view data |
| `src/recording/RecordingState.hpp` | Mutable command recording data |
| `src/recording/ViewRecording.hpp` | Small composed recording facade |
| `src/recording/ViewPreparation.cpp` | Validate and prepare one view |
| `src/recording/ViewExecution.cpp` | Build handlers and execute the compiled graph |
| `src/recording/ViewPresentation.cpp` | Game output, host chrome, clear, and explicit screenshot adapter |
| `src/recording/ViewSubmission.cpp` | Submit, commit, discard, and retire |
| `src/recording/GraphResources.cpp` | Resolve graph resources and stage history writes |
| `src/recording/RecordingProfile.cpp` | Node timings, timestamp marks, and stage probes |
| `src/passes/SceneDraw.cpp` | Open scene passes and record world, blended, and effect draws |
| `src/passes/ImageDraw.cpp` | Fullscreen, image, overlay, and composition draws |
| `src/passes/ResidencyUpload.cpp` | Existing upload recording helpers |
| `src/capture/SurfaceCapturePlan.hpp` | Capture request, entry, frontier, and result records |
| `src/capture/SurfaceCapturePlan.cpp` | Pure bounded frontier planning |
| `src/capture/SurfaceCaptureRecording.hpp` | Narrow capture recording API |
| `src/capture/SurfaceCaptureRecording.cpp` | Target resolution and capture draw recording |
| `src/nodes/*.cpp` | Thin authored-node adapters grouped by family |

This layout is a destination, not a command to create every file at once.
Create a file only when a coherent responsibility moves into it.

The current render module policy names `ViewRecording.cpp`, `ScenePasses.cpp`,
and `src/nodes/*.cpp` as the pass layout. This plan keeps node adapters in
`src/nodes`. Any change to the documented shared-helper layout needs explicit
policy review during implementation. This planning file does not edit policy.

## Main frame flow

`Renderer::Render` remains the public entry and the only batch coordinator:

```text
require renderer owner thread
pump completed renderer and hook work
validate all views and resolve graph groups
acquire the batch command and optional swapchain image
begin batch state

for each graph group in stable order
  for each view in stable order
    build ViewInput
    PrepareView
    BuildNodeTable
    ExecuteViewGraph
    FinishViewPresentation

submit the final batch command
commit or discard submission-backed state
submit ordered download work
pump only completed readbacks
clear batch state
return FrameResult
```

The functions should have narrow results:

```cpp
ViewStart PrepareView(RenderContext &, const ViewInput &, PreparedView &);

bool ExecuteViewGraph(
	RenderContext &,
	const ViewInput &,
	PreparedView &,
	RecordingState &,
	FrameResult &
);

void FinishViewPresentation(
	RenderContext &,
	const ViewInput &,
	const PreparedView &,
	RecordingState &,
	FrameResult &
);

SubmissionResult FinishBatch(
	RenderContext &,
	RecordingState &,
	FrameResult &
);
```

Names may change to match local style. The ownership cuts may not.

Preserve these current rules:

- Frame and world scopes are reused through `PreparedScopes`.
- Isolated shadows remain isolated per required view.
- Intermediate batch views never submit the shared command buffer.
- Only the final batch view records final host presentation and submits.
- A graph node owns every ordinary game render pass.
- Host chrome remains after graph output and outside the authored graph.
- An untouched swapchain image is cleared before presentation.
- Graph history advances only for work backed by a successful submit.
- Failed acquire or submit keeps retryable damage and history pending.

## Graph and pass split

The graph remains the only description of order, resources, and demand.
`GraphRunner` remains the SDL-backed `graph::NodeRunner`. Keep its missing-node,
rejected-node, submitted-count, timing, and dropped-mark diagnostics.

Keep the current retained-node filter. Keep stage probes wrapping actual graph
execution. A stage probe may observe before and after a skipped handler, so it
must not become the hook scheduling path.

Node adapters should become small. An adapter validates the authored node and
resource contract, resolves its narrow recorder, and records the node. It does
not contain a private recursive scheduler.

Split large families by work that can be understood and tested alone:

- geometry preparation and opaque drawing
- transparent ordering and drawing
- shadow recording
- mirror and portal capture recording
- surface composition
- lighting and environment
- post processing and tonemap
- game output and capture output

Do not create a `Pass` enum, `PassOrder`, or a second table that duplicates the
graph. A new way of drawing remains a graph node with one registered backend.

## One explicit surface frontier

### Path to keep

`SurfaceCapturePlan` becomes the only camera expansion plan for mirrors and
portals. Its output remains plain CPU data. `SurfaceCaptureRecording` walks the
plan and records GPU work in the plan's proven order.

The first change replaces only the recursive `visit` in
`PlanSurfaceCaptures`. It must produce byte-equivalent entry fields, root
indices, child links, pixel counts, failure status, and postorder for existing
fixtures.

Only after planner parity should `MirrorNodes::fillMirror` and
`PortalNodes::fillLevel` be migrated to the shared plan.

### Frontier record

Use a bounded vector as a stack. Do not use a priority queue or breadth-first
queue. The comparison with A* is only that pending work is explicit data.

```cpp
enum class CaptureFrontierPhase : uint8_t {
	Enter,
	NextChild,
	Record,
	Leave,
};

struct CaptureFrontierEntry {
	CaptureFrontierPhase Phase = CaptureFrontierPhase::Enter;
	uint16_t Parent = NO_SURFACE_CAPTURE;
	uint16_t PlanEntry = NO_SURFACE_CAPTURE;
	uint16_t NextSlot = 0;
	core::CFrame Frame;
	glm::mat4 Projection{1.0f};
	uint32_t Width = 0;
	uint32_t Height = 0;
	uint32_t RemainingDepth = 0;
	int16_t Arrival = -1;
};
```

The exact fields may be reduced after the parity test exposes what each phase
needs. The stack and expanded entry count are both bounded. Depth alone does
not bound branching.

### Required traversal order

Preserve depth-first traversal and source-slot order exactly:

```text
enter parent
  descend child A
    finish descendants of A
    record A
  descend child B
    finish descendants of B
    record B
record parent while sampling completed A and B
leave parent
```

A breadth-first walk followed by reverse depth is not equivalent.

Targets are pooled by viewport, kind, recursion level, and surface slot. Two
sibling branches can reuse the same lower-level target because the first
sibling records all commands that consume its descendant before the next
sibling overwrites that target. Reordering by depth, kind, material, or target
key breaks this lifetime.

### Target rules

- Never sample the texture currently used as a render target.
- Reacquire a pooled target by stable key after descending.
- Do not retain pointers across vector growth or target allocation.
- Keep root mirror ping-pong and history behavior.
- Keep inner mirror and portal targets non-cycled unless measurement changes it.
- Keep matrices describing the image that was actually written.
- Preserve readiness and successful-write rules.
- Do not allocate one texture per capture entry.
- Add a debug generation to target keys if needed to assert legal reuse.

### Camera and visibility rules

- Derive every child camera from its parent camera.
- Mirrors keep fitted reflected lenses.
- Portals keep warped cameras and the existing oblique clip contract.
- Mirrors skip their own pane.
- Portals skip the arrival partner and external images.
- Visibility is tested from the current derived view.
- Transparent geometry is ordered from each capture camera.
- Full-scene caster inputs remain distinct from camera-culled screen inputs.
- Filters, ribbon facing, particle facing, and seam light behavior stay intact.
- Pane-less surface cameras retain their existing iterating fallback.
- Terminal depth retains the existing flat or history fallback and bounce probe.

### Budget parity

The existing mixed plan validates slots, finite matrices, dimensions, depth,
entry count, and total pixels. It fails atomically and clears partial output.
Keep those semantics during the recursion replacement.

Legacy mirror and portal recorders do not admit work at the same point. Mirror
admission occurs before descent. Portal admission occurs after descent. Do not
silently normalize these behaviors during extraction. Any unified admission
rule is a later behavior change with its own image, budget, and profile proof.

## Hook integration

`DataFactoryHookBind` is injected through `RenderContext`. Its ownership and
API remain exactly as defined by `RENDER-HOOKS.md`.

The binder owns:

- immutable hook registration
- session connections
- armed, queued, submitted, ready, and terminal batch state
- generation-checked hook, connection, and batch handles
- fixed capacity and retained-byte budgets
- cancellation, expiry, teardown, and complete bundle publication
- capability descriptions and stable hook names

The renderer keeps GPU copies, device resources, fences, submission, and image
conversion. The external data factory keeps file writing, checksums, and
manifest commit.

The first limits remain:

- 12 renderer resource image slots
- 6 live capture jobs
- 64 MiB retained-byte ceiling
- admission counted by unique capture node

Different hooks may observe different authored nodes. Each result must match
the node in its own hook spec. Node names are not required to match across a
bundle.

### Scheduling seam

Preparation arms a connection for the matching prepared view. It does not
invoke the hook.

The matching output node schedules the hook after its declared outputs exist
and before those resources can be recycled. It passes an immutable owned
`RenderObservationContext` to `DataFactoryHookBind::CallHooks`, or to the final
`ScheduleHooks` spelling if that rename is chosen.

If the node is skipped, no observation is fabricated. If admission returns
`Backpressured`, ownership does not change and the caller may retry at the next
eligible view. A required capture becomes terminal only under the expiry rules
defined by `RENDER-HOOKS.md`.

Submission changes queued hook work to submitted. Failed submission, world
destruction, pipeline replacement, disconnect, expiry, and shutdown cancel
every owned resource token. `Pump` runs on the renderer owner thread and checks
only completed work. It never waits.

`CaptureRecordValidation` remains the final bundle alignment check. Optional
unavailable channels stay explicit. A required failure rejects the bundle.

### Capture paths that remain separate

The current explicit screenshot path waits for a fence and writes a file from
`ViewRecording::Finish`. Those actions are not allowed for binder observations.

Move the explicit screenshot path into `ViewPresentation.cpp` as a named legacy
adapter. Do not route binder work through it. Whether explicit screenshots
later become asynchronous is a separate change with separate product behavior
and tests.

## Failure and cleanup

Every extracted function begins with shallow guards, but cleanup cannot depend
on reaching the end of a long function. Use one scoped batch owner or the
project's existing cleanup pattern to leave command, timing, upload, history,
download, and hook state valid on every return.

Do not invent universal abort semantics during the split. The current
non-batch incomplete-view path can submit its partial command and commit
resident or history work when that submit succeeds. Preserve and characterize
that behavior before deciding whether it should change.

Classify outcomes explicitly:

| Outcome | Required action |
|---|---|
| View abandoned before recording | Release or retain acquisition exactly as today; schedule no hook |
| Node rejected | Close active pass, name the node, and follow current incomplete-view behavior |
| Scene submit failed | Discard pending history, fail resident uploads, abandon timings, and fail queued hooks |
| Scene submit succeeded | Commit submission-backed uploads and history; mark hook copies submitted |
| Download submit failed | Keep rendered frame result, fail affected readbacks, and release tokens |
| Batch intermediate view | Keep shared command ownership; do not submit |
| Shutdown or replacement | Stop new scheduling, cancel tokens, release completed ownership, then invalidate generations |

Tests must pin each row before cleanup code moves.

## Migration phases

### P0: freeze contracts

Record current behavior for view ordering, graph scopes, retained nodes, node
errors, surface plans, hook scheduling, history commits, incomplete views,
explicit screenshots, and failed submissions.

Gate: existing tests pass, new characterization tests pass, and the hook system
meets every acceptance test in `RENDER-HOOKS.md`.

### P1: extract persistent owners

Move one coherent group at a time out of `RendererState.hpp`. Begin with
readback or pipeline state because each already has clear operations. Keep
`Renderer::Impl` as a composition root while callers migrate.

Gate: each field has one home, no copied state exists, public headers are
unchanged, and the render suite passes after every move.

### P2: separate view records

Turn `ViewRequest` into `ViewInput`. Move derived values into `PreparedView` and
command mutation into `RecordingState`. Keep `ViewRecording` as a thin facade
so node families can migrate independently.

Gate: every moved field is deleted from its old home, lifetimes are documented,
and no borrowed input survives synchronous recording.

### P3: split preparation and execution

Extract `PrepareView`, `BuildNodeTable`, `ExecuteViewGraph`, presentation, and
submission from `ViewRecording.cpp`. Preserve handler precedence, retained
filtering, profiling, stage probes, batch grouping, and scope reuse.

Gate: `ViewRecording.cpp` no longer owns unrelated phases, and failure tests
match the baseline.

### P4: split shared recording helpers

Move scene drawing, image drawing, upload recording, graph resources, and
profiling into focused files. Keep node adapters as their only graph entry.

Gate: every old `ScenePasses.cpp` caller has moved and `ScenePasses.cpp` is
deleted. There is still one graph runner and one pass order.

### P5: replace planner recursion

Replace `PlanSurfaceCaptures::visit` with the explicit bounded DFS stack. Keep
the plan type and all current ordering and budget semantics.

Gate: exact CPU plan fixtures pass, the recursive planner is deleted, and
frontier high-water metrics remain bounded.

### P6: converge mirror and portal recording

Move `fillMirror` and `fillLevel` to the shared plan and recording path. Retain
kind-specific camera, clipping, filtering, target, history, and fallback rules.

Gate: recursive GPU recorders are deleted. CPU trace tests prove target reuse,
and GPU fixtures match the accepted baseline.

### P7: narrow node families

Split only node files that still combine independently testable jobs. Preserve
authored kind names and backend registration. Delete each old handler when its
replacement becomes the sole registered backend.

Gate: every supported graph kind has exactly one backend. Unknown and rejected
kinds retain current diagnostics.

### P8: shrink the facade

Remove transitional aliases, broad includes, and unused friend access. Keep
`Renderer.cpp` focused on public calls, thread ownership, and batch flow.

Gate: public ABI and include boundaries remain valid, server and CDN presets
remain free of the graphics stack, and architecture checks pass.

### P9: final proof

Run the complete render and graph suites, approved GPU checks, release profiles,
format checks, architecture checks, and a clean-build comparison.

Gate: every acceptance item below has evidence and no replaced path remains.

## Test plan

### CPU and device-free tests

Extend the existing focused suites rather than creating endless smoke tests:

- `SurfaceCapturePlan.cpp`: exact entry, child, root, and postorder parity
- frontier trace: enter, descend, record, and leave sequence
- sibling target reuse with generation checks
- stable input-slot tie ordering
- derived parent camera and projection
- mirror self-skip and portal arrival-skip
- external portal exclusion
- depth, entry, and pixel exhaustion
- invalid slots, dimensions, matrices, and duplicate sources
- atomic plan failure with no partial output
- fallback behavior at terminal depth
- retained and skipped node hook behavior
- incomplete-view and partial-submit cleanup
- graph history commit and retry rules
- stale handle and cancellation behavior from `RENDER-HOOKS.md`

Reuse `GraphRunner`, `Passes`, `FramePreparation`, `GraphHistory`,
`PresentationDamage`, `ResourceImage`, `DataCapture`, and `Readback` suites.

### GPU tests

Use existing render fixtures for focused image and lifetime checks:

- mirror inside mirror
- portal in both directions
- mixed mirror and portal chains
- sibling branches reusing deeper targets
- external portal images
- pane-less surface cameras
- transparent geometry and ribbons from capture eyes
- resize and viewport slot reuse
- retained mirror history and failed-submit retry
- hooks observing named outputs before resource reuse
- optional unavailable and required failed bundle behavior

GPU work is final verification under repository policy. Ask before running live
or real-device checks. If approval is declined, record the exact skipped gate.

### Failure injection

Add narrow seams only where needed to force:

- target allocation refusal
- command acquisition failure
- graph node rejection
- scene submit failure
- download submit failure
- readback expiry
- hook backpressure
- pipeline replacement during pending capture
- world and renderer teardown during pending capture

Prefer existing device-free planners and real integration fixtures over a mock
renderer.

## Profiling gates

Profile the release preset. Name the backend, scene, view count, surface depth,
resolution, pipeline, and hook channels beside every result.

Measure:

- CPU preparation, graph execution, recording, and submit time
- capture frontier entries and stack high-water mark
- surface pixels admitted and refused
- node and draw counts
- transfer and command-buffer bytes
- GPU resource creations, live bytes, and peak bytes
- retained target reuse and history writes
- hook context bytes and allocations
- readback bytes, latency, queue depth, expiry, and backpressure
- cache hit behavior with upload and allocation counters
- clean build wall time and total CPU seconds

Splitting headers can improve parallel wall time while increasing total frontend
CPU work. Report both. Do not call the split faster unless the measured build or
frame path is faster.

The frontier conversion is accepted for safety and visible control flow first.
Do not claim a frame-time win unless the release profile shows one.

## Risks and controls

| Risk | Control |
|---|---|
| A second scheduler appears in pass files | Graph remains sole order source; adapters register one backend each |
| Context becomes a service locator | Required concrete references only; no global lookup or generic registry |
| State is copied during extraction | Delete old field in the same phase; assert one owner |
| DFS becomes breadth-first by accident | Exact frontier trace and pooled-target generation tests |
| Target overwritten before parent samples it | Preserve sibling subtree completion and command order |
| Camera derives from the root instead of parent | Per-entry camera fixtures for mirror and portal chains |
| Budget behavior changes during cleanup | Exact status, count, pixel, and partial-output tests |
| Hook schedules for a skipped node | Schedule only in the executed output adapter |
| Hook inherits screenshot waits | Separate binder readback from legacy screenshot adapter |
| Failed submission publishes a bundle | Move queued work to submitted only with successful device submit |
| Split changes partial-submit behavior | Characterize and preserve before any policy change |
| Too many tiny files hurt locality | Split by coherent job, not by function count |
| Private SDL types leak outward | Keep all new records under `src/` |
| Build gets slower overall | Measure wall time and total CPU seconds |

## Policy dependencies

`mono.engine/render/AGENTS.md` still names `RENDER_PIPELINE.md` as authority,
prescribes the current `ViewRecording` and `ScenePasses` layout, and describes
`fillMirror` recursion. It also contains an older stage statement that conflicts
with its later graph sections.

These are policy dependencies to resolve with the required human review during
implementation. Do not silently edit protected policy files as part of this
plan.

Keep `graph` shared at L9 and `render` client-only at L12. No new module or tier
escape is needed. If a proposed extraction creates a new dependency edge, stop
and resolve the design instead of widening the layer rule.

## Completion checklist

- `Renderer::Render` is the only batch coordinator.
- The compiled graph is the only render scheduler.
- `GraphRunner` remains the only graph-to-device adapter.
- Every mutable field has one owner.
- View input, prepared data, and command state have explicit lifetimes.
- Node adapters depend on narrow recording helpers.
- No recursive surface planner or recorder remains.
- One bounded DFS frontier preserves exact child-before-parent order.
- Pooled targets are not overwritten before their consumers record.
- Mirror, portal, and mixed capture behavior passes focused GPU fixtures.
- `DataFactoryHookBind` remains the only hook owner.
- Hook scheduling occurs only at declared executed node seams.
- Hook polling never waits and never writes files.
- Legacy screenshots remain separate and honestly documented.
- Submission, history, residency, download, and hook failures are covered.
- Replaced files, aliases, handlers, and state homes are deleted.
- Public headers gain no SDL dependency.
- Architecture, format, render, graph, and approved GPU checks pass.
- Release profiling records frame cost, memory, traffic, and hook backpressure.

The desired end state is boring: one graph, one batch runner, one explicit
surface frontier, one hook binder, and one owner for every piece of state.
