# Render hooks

This document defines the render hook system. It gives the data factory one
place to declare, connect, run, collect observations, and submit bounded
typed mutations without creating a second render graph or allowing arbitrary
callbacks in the renderer.

The first hook is data capture. The system is deliberately small so a second
real consumer can prove which parts should be shared before more hook kinds are
added.

## Purpose

`DataFactoryHookBind` owns the binding between named hooks and static render
seams. Observation hooks keep their asynchronous immutable-record contract.
The built-in `view.camera` hook accepts a bounded owned one-shot patch before a
matching view begins recording.

The flow is:

```text
register hook specs
        |
connect a session's hook set
        |
arm and admit one bounded batch
        |
graph executes each derived capture node
        |
hook freezes context and records queued GPU readback
        |
renderer submits without waiting
        |
DataFactoryHookBind::Pump collects completed readback
        |
host takes a complete bundle and saves it
```

The binder is an observation owner. It does not become a general event bus,
command dispatcher, or file writer.

## Current seam

`RenderObservationContext` in `mono.engine/render/include/engine/render/RenderObservation.hpp`
is the value record used at the current seam. The binder-owned
`DataFactoryObservation` helper copies the pipeline, authored node, world and
snapshot identity, frame, camera facts, and named graph resources from a
`graph::RunContext`. The record contains
names and values, never device handles or callbacks.

The current data capture path queues `ResourceImage` readbacks with
`ResourceImageDelivery::CopiedPixels`. `Renderer::Impl::RecordResourceImages`
records the copies at the node, and `PollDataCapture` takes the completed image
group later. `CaptureRecordValidation` checks that the returned images agree on
snapshot, frame, dimensions, camera and resource identity. The hook binder
moves ownership of this scheduling and collection state into one place while
leaving GPU copy and fence work in `render`.

## Non-goals

- Do not build a callback graph beside the render graph.
- Do not call user code, Lua, MCP, or a file system from a render thread.
- Do not let a hook mutate scene, graph, pipeline, or resource state.
- Do not let a hook invoke external code. `view.camera` is a typed owned value
  patch consumed synchronously by the renderer, never a callback or mutable
  `View` reference.
- Do not expose SDL GPU objects in a hook context or saved record.
- Do not make a hook wait for a GPU fence or for another CPU thread.
- Do not make physics or replication hooks until the render hook has two real
  consumers and its queue and polling rules have been measured.
- Do not serialize process-local enum values or slot indices.
- Do not retain a full frame of GPU images for every submitted frame.

## One owner and API

`DataFactoryHookBind` is the only owner of hook registration, session
connections, pending batches, completed bundles, generation checks, budgets,
and teardown. Its public API is intentionally boring:

```cpp
HookHandle RegisterHook(const RenderHookSpec &spec);

ConnectHooksResult ConnectHooks(
    const HookConnectionRequest &request,
    std::span<const HookHandle> hooks
);

void DisconnectHooks(std::span<const ConnectionHandle> connections);

std::optional<BatchHandle> ArmDataCapture(
    ConnectionHandle connection,
    DataCaptureRequest request
);

CallHooksResult CallHooks(
    ConnectionHandle connection,
    BatchHandle batch,
    const RenderObservationContext &context
);

void Observe(const RenderObservationContext &context);

std::optional<HookBundle> TakeCompleted(BatchHandle batch);

std::vector<RenderHookCapability> DescribeHooks() const;

void Pump();

void Cancel(BatchHandle batch);

void Shutdown();
```

`CallHooks` admits readback work before graph recording. It rechecks the
installed pipeline revision and never invokes arbitrary callback code.
`Observe` is the static graph seam. It freezes the exact context when a matching
derived capture node runs. The already queued renderer copy then records from
that node without waiting.

`ConnectHooks` validates the whole requested set before changing the session.
It rejects an unknown handle, duplicate hook or channel, unsupported channel,
missing graph node, stale pipeline revision, or connection capacity overflow.
A failed connect leaves all registry slots unchanged.

`DisconnectHooks` stops new scheduling for each connection, cancels its pending
readbacks, and releases its completed records. It is safe to pass an already
disconnected handle. Disconnect does not invalidate records already copied out
by the caller.

`Pump` runs on the renderer owner thread. It checks only completed readbacks,
turns them into value records, and publishes complete bundles. It does not
wait. `TakeCompleted` transfers ownership of one published bundle to the
caller, or returns no value when that batch is still pending.

## Hook specs and names

Each built-in hook has a typed specification. There is no generic `any` bag and
no inheritance tree for contexts.

```cpp
struct RenderHookSpec {
    core::Name Name;
    RenderHookKind Kind;
    core::Name NodeKind;
    uint32_t SchemaVersion;
    bool Required;
    std::array<DataCaptureChannel, MAX_HOOK_CHANNELS> Channels;
    uint8_t ChannelCount;
};
```

The enum is useful for a process-local switch. A stable string is the contract
for capability discovery, manifests, logs, and MCP. Persist names such as
`data_capture`, `data_capture.rgb_linear_hdr`, or a future
`render.visibility`. Never persist the enum's numeric value.

Names are validated as non-empty, bounded, and unique within a registry. The
pipeline and graph node are also names. `core::Name::Id()` is an in-process
comparison aid only and must not appear in a saved bundle.

The initial built-ins are the twelve `data_capture.<channel>` hooks. Their node
kind is `capture`. A connection supplies the authored base node, and
`DataCaptureNode` derives the channel-specific node name. The typed request
declares object, semantic, and part labels, snapshot identity, pipeline, view
slot, and temporal history. Its output is the existing data capture poll shape:
one camera convention, one snapshot, and a bounded set of planes with status,
dimensions, format metadata, bytes, and hashes.

## Context and lifetime

Every hook receives an immutable, owned context. The context is copied while
the graph node is executing, so a later camera change, view reuse, or recording
destruction cannot change what the readback describes.

The context states:

- world and snapshot identity
- capture frame and view slot
- pipeline and authored node
- camera transform, projection availability, crop facts, and dimensions
- named resources read and written by the node
- fields that are unavailable for this node

It contains no pointer to a world, renderer, pipeline, texture, command buffer,
ECS row, or callback-owned object. A pointer may exist in private transient
implementation state while recording a command, but it never enters the
context, queue, bundle, or world boundary.

The renderer owner thread creates the context and enqueues the GPU work. `Pump`
and `TakeCompleted` run on the renderer owner thread unless a
future adapter explicitly copies the bundle across a message boundary.
Consumers may retain a taken bundle after the recording and connection are
gone.

## Automatic graph execution

Hooks are observer nodes or declared outputs at existing static graph seams.
They are not an additional graph of callbacks. A connected hook runs when its
declared node runs, after that node has established its named outputs and before
those outputs can be recycled.

The first seam is the existing data capture resource observation in the data
capture output path. The node passes its immutable observation context to the
binder. The binder matches it to the admitted batch. The existing resource
image path records copies for resources named by the typed data capture
request. Other render code keeps recording ordinary graph work through the same
command buffers.

If the node is skipped, the hook is not called. If a declared resource is not
available, the hook records `Unsupported` according to its typed contract. It
does not invent a value from a previous frame.

Render changes normally arrive through CPU-owned scene or graph state before
submission. The narrow exception is `view.camera`: its identity contains the
exact world name, snapshot id, pipeline name and installed revision, and view
slot. It can replace optional `View::CameraFrame`, `View::Camera`, and
`View::Projection` only. The binder validates the complete patch before it
claims capacity, rejects a second pending patch for that identity, and the
renderer copies only the matching caller view on its stack. It consumes the
patch before setting `ActiveDataCaptureSource` and before `RenderView`, forces
scene and viewport camera invalidation, and leaves the caller's `View`
unchanged. Cancellation, consumption, shutdown, and stale pipeline replacement
invalidate the generation handle.

## Readback and atomic bundles

Readback is asynchronous. A hook queues a bounded set of resource copies and
returns. Submission, fence polling, and image conversion stay in the renderer.
The binder never blocks on a fence.

A `HookBundle` is published only when all required hooks and planes for its
batch agree on:

- world name and snapshot id
- capture frame and pipeline revision
- pipeline and view slot
- camera and projection facts
- crop and image dimensions
- connection generation

Different hooks may observe different authored nodes. Each result must match
the node declared by its own hook spec. Node names are not required to match
across the bundle.

All current data capture hooks are required. A failed hook fails the whole
bundle. Partial bytes are discarded or marked failed and are never presented
as a successful capture. A future optional hook must add an explicit
unavailable record before the binder may accept it.

Resource tokens are private to the renderer and are consumed exactly once.
Every queued token is cancelled on failure, disconnect, or shutdown.
The existing resource image validation remains the final check on dimensions,
formats, frame identity, and named resources.

## Capacity and handles

All current limits are fixed constants. The first implementation retains the
renderer's twelve resource image slots, the bridge's six live capture jobs, and
a 64 MiB binder completed-byte ceiling. The script bridge separately caps the
bytes retained after collection at 64 MiB. Resource admission counts unique
capture nodes because several logical channels can
share one GPU readback. Registration and connection fail cleanly when a limit
is reached. Per-frame vectors may hold only the bounded capacity declared by
the binder.

Handles are `{slot, generation}` values. Registration, connection, and batch
handles each have their own generation. A released slot increments its
generation before reuse, so a stale handle cannot cancel, collect, or mutate a
new object. Handle values are process-local and never cross a world boundary.

Backpressure is visible. When no batch slot or readback budget is available,
admission returns `Capacity` or `Backpressured` and changes no ownership. The
caller may retry at the next eligible view. Pending work has a fixed owner-pump
limit and becomes a terminal failure when that bound is reached. The binder
never grows an unbounded queue to hide a slow consumer.

## Profiling

`just data-capture-hook-bench 5` measures 64 complete connect, arm, call,
cancel, and disconnect lifecycles through the real renderer without opening a
device. Removing the full graph snapshot from dispatch reduced this release
benchmark from 41.62 microseconds with a 3 percent spread to 20.48 microseconds
with a 1 percent spread. This benchmark measures CPU dispatch and no-device
backpressure. It does not measure GPU execution or readback throughput.

The hook path reports fixed metric names:

- `render.data_capture_hook.dispatches` and `backpressure` count admission;
- `readback_poll_calls` counts actual renderer polls;
- `readback_polls` records polls per non-cancelled terminal request;
- `readback_latency` records submission-to-terminal time for those requests;
- `package_bytes`, `retained_bytes`, and `released_bytes` report byte traffic;
- `drops` counts non-cancelled terminal failures.

The renderer's resource-image counters remain the source for GPU transfer and
host copy bytes. Heap profiling remains the source for process live and peak
allocation. The data-capture packager moves validated resource-image vectors
into result planes before hashing them, so it does not allocate and copy a
second full host plane. The Vulkan capture suite checks every moved plane's
payload and hash, but is a correctness check rather than a throughput claim.

## Sessions, teardown, and failure

Connections belong to a data factory session. A session disconnects before its
world or renderer is destroyed. Teardown follows this order:

1. Mark the connection closed, so no new graph seam can schedule work.
2. Cancel pending resource image tokens.
3. Drain or discard completed bundles owned by the connection.
4. Release labels, copied bytes, and private scratch storage.
5. Invalidate the connection generation.

World teardown and renderer shutdown perform the same cancellation. A pipeline
revision mismatch is rejected before admission and checked again at the graph
seam and on collection. A late GPU completion is ignored when its batch
generation, snapshot, or pipeline revision no longer matches. No destructor
calls into a graph node or external consumer.

Required hook failure is reported in the bundle status and diagnostic. An
unsupported channel remains explicit and does not become a cache hit. Pending
work that reaches the pump bound is reported as failed and releases its private
readback tokens.

## Capability discovery and saving

Capability discovery enumerates the live registry as owned records with stable
hook names, schema versions, access, kind, supported channels, node kinds,
mutated fields, in-flight limits, readback-node limits, retained-byte limits,
pending-pump limits, and required flags. `view.camera` reports
`synchronous_mutation`, empty channels, and `camera_frame`, `camera`, and
`projection` fields. The Luau, MCP, and client adapters read this description.
They do not reach into binder state or receive renderer pointers.

The binder returns owned records and bytes. The external data factory saves
payloads first, computes or verifies hashes, and commits the manifest last.
Saving a manifest is outside the render thread and outside `DataFactoryHookBind`.
The manifest records stable names, schema versions, snapshot and frame identity,
camera facts, dimensions, status, and hashes.

Raw spatial export is separate from this image-observation path. After scene
generation, an external factory reads the revision-fenced `raw-scene/v1` record
through bounded MCP calls. The record carries stable IDs, transforms, geometry,
cameras, and explicit unavailable facts. `DataFactoryHookBind` does not prepare,
transform, save, or train on that data.

## State machine

```text
registered
    -> connected
    -> armed
    -> queued
    -> submitted
    -> ready
    -> collected

queued or submitted    -> failed at pump bound
connected              -> disconnected
```

`registered` describes a validated immutable spec. `connected` associates it
with a session. `armed` waits for its matching prepared view. `queued` owns an
immutable context and private resource tokens. `submitted` means the GPU work
is in a command buffer. `ready` means
all required readbacks are complete and validated. `collected` transfers the
bundle to the caller. `failed` and `disconnected` release all pending resources,
and their old handles cannot be reused.

`view.camera` has a separate one-shot lifecycle: `pending` is owned by the
bridge until the renderer owner validates its snapshot barrier and exact
installed pipeline revision, then arms the typed patch. Consumption becomes
`applied-awaiting-restore`; the next matching submission renders the original
view with camera damage forced. Only a successful submission acknowledges
`applied`. Cancellation after consumption waits for that restoration and then
reports `cancelled`. Bridge polling reads cached terminal status and releases
the ticket only after the owner has reclaimed its binder handle.

## Rollout

1. Inventory the current node output, resource lifetime, submission, and
   asynchronous readback boundaries. Keep `RenderObservationContext` as the
   value seam.
2. Add the registry, stable names, typed specs, fixed capacities, and generation
   handles to `DataFactoryHookBind`.
3. Move current data capture ticket scheduling, polling, cancellation, and
   bundle validation ownership into the binder.
4. Connect the binder at the static data capture output node. Remove the
   duplicate bridge state after the new path is proven.
5. Publish hook capabilities through the existing data factory and MCP adapters.
6. Profile a release capture for record bytes, allocations, GPU work, readback
   latency, queue depth, and dropped records.
7. Add a second real render consumer only after the first path passes the
   acceptance checks below. Consider physics and replication hooks separately,
   with their own tick and lifetime contracts.

## Acceptance tests

The first implementation is complete when these behaviors are covered:

- registering the same stable name with the same typed spec is idempotent;
- conflicting names, invalid nodes, duplicate channels, and capacity overflow
  are rejected without partial state;
- connecting an all-valid set is atomic and a failed set changes nothing;
- a connected hook runs automatically exactly once per matching node execution;
- a skipped node produces no fabricated record;
- contexts retain the original snapshot, frame, camera, crop, and resource names
  after the view is reused;
- readback polling never blocks and publishes only complete matching bundles;
- unsupported channels are refused and required failures reject the bundle;
- stale connection and batch handles are rejected after generation reuse;
- byte and in-flight limits produce a terminal result without leaking a slot;
- disconnect, pipeline replacement, world destruction, cancellation, and the pump bound
  release every resource token;
- capabilities expose stable names and limits, with no enum number in output;
- the external saver can write payloads and commit the manifest after collection;
- existing data capture tests still validate camera, snapshot, frame, dimensions,
  formats, and resource alignment.

The binder is ready for another hook when these checks pass with no render-thread
waits, no unbounded growth, and no second copy of world or renderer ownership.
