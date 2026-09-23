# MCP tools: scenes, demos, and data extraction

Atomic exposes a Model Context Protocol surface for inspecting and controlling a
running engine. An MCP client and the typed Python client in the sibling
`datafactories-docs/api.py` use the same tool table, schemas, and engine
validation. There is no private Python execution path around the engine.

This document describes rows and lifecycle behavior shipped by the running
host. Historical proposals and sibling handoff documents can explain intent,
but they do not advertise a capability. `tools/list`, `resources/list`,
`prompts/list`, and `negotiate` are the contract for one host process.

The engine owns worlds, scene mutation, deterministic stepping, snapshots,
captures, and retained resources. A caller owns recipes, dataset layout,
conversion, storage, training, and evaluation.

## Start a host and discover its tools

The port is off until an executable receives `--mcp-port`. A data factory needs
the isolated-world mode as well:

```console
$ client --data-factory --mcp-port 8736
```

`mcpbridge` is a stdio MCP server that connects a client to that local port:

```python
import api

async with api.connect_stdio(
    "../atomic-game-engine/.cache/build/dev/tools/mcpbridge",
    ["--port", "8736"],
    "factory-control",
) as control:
    capabilities = await control.negotiate()
    if isinstance(capabilities, api.NegotiateUnavailable):
        raise RuntimeError(capabilities.reason)
    print(sorted(operation.name for operation in capabilities.operations))
```

The control server binds to loopback and has no authentication layer. Keep the
host and bridge on a trusted machine.

Each executable registers only the rows it can honour. A headless server can
offer lifecycle and scene observations without renderer capture. A client or
studio host can add capture rows. Conditional lifecycle features such as
`fork_world`, `seek_backward`, `apply_intervention`, and `render_only` are
present only when their host service is installed. Only `render_only` has the
paired `poll_render_only` row. Capture tickets use `poll_capture`. Always call
`negotiate` and inspect `tools/list` before building a workflow around an
optional feature.

## Hook discovery and refresh

The control kernel owns the protocol tables. Product code composes optional
providers locally, where it already owns the typed world, renderer, capture,
or factory service. The shared control module does not link renderer adapters
or load native plugins at runtime.

Each active provider has a stable hook ID, revision, state, tool names, and
declared numeric limits. `negotiate` returns these under `hooks` and includes a
monotonic `control_generation`. A successful hook activation or completed
removal advances that generation. The hook list reports active providers, while
`operations` reports the callable tool table at the time of the response.

```json
{
  "control_generation": 17,
  "hooks": [
    {
      "id": "studio.data-factory.raw-scene",
      "state": "active",
      "revision": "v1",
      "tools": ["begin_raw_scene_extract", "get_raw_scene_chunk", "release_raw_scene_extract"],
      "limits": {}
    }
  ]
}
```

A hook stages its tools, resources, and prompts as one registration. A name or
URI collision rejects the activation, leaving the existing surface unchanged.
Closing a hook first blocks new calls to its rows. Existing calls retain their
activation guard until they return; rows disappear after the hook has drained.
Dependencies keep an owner hook from completing removal until its dependent
hooks have closed.

MCP `listChanged` remains `false` for tools, resources, and prompts. The host
does not send list-change notifications. After a host acknowledges a provider
change, refresh `negotiate`, `tools/list`, `resources/list`, and `prompts/list`
before issuing another optional operation. A disabled provider is absent from
the applicable list instead of remaining listed and refusing later.

## General tool reference

The standard tool table is available to a host that enables the control surface.
Every row is described by the running program and is callable only when listed.

| Group | Tools | Purpose |
| --- | --- | --- |
| Discovery | `negotiate` | Read the exact advertised operation, capability, and limit contract before using an optional feature. |
| Engine and worlds | `engine_info`, `world_list`, `world_tree` | Identify this engine and inspect the available worlds and bounded instance tree. |
| Instances and components | `instance_get`, `instance_set`, `engine_components`, `component_list`, `entity_query`, `component_get`, `component_set` | Inspect the class-facing instance view and the engine or world component schemas. |
| Profiling | `profile_frame` | Read bounded frame-profile data. |
| Architecture | `layer_table`, `module_get`, `module_may_link` | Inspect the compiled module graph and whether a proposed edge is allowed. |
| Script surface | `class_list`, `class_get`, `script_check` | Discover class properties and type-check bounded Luau input without evaluating it. |
| Diagnostics | `log_tail`, `log_level`, `metrics_read` | Read or configure logs and read counters, gauges, and histograms. |
| Tests | `test_run`, `test_result` | Start declared suites without blocking a frame, then poll their result. |
| Prompts and resources | MCP `resources/*` and `prompts/*` | Read advertised checked-in context and use advertised prompt templates. |

`engine_components`, `component_list`, and `instance_get` answer different
questions. `engine_components` is the sealed process-wide storage catalogue.
`component_list` is what one world declares and how many entities use each
component. `instance_get` is the class and property projection for one entity.

`world_tree`, `entity_query`, log tails, profiles, and similar reads are bounded.
Read their advertised JSON schema instead of assuming a full-world response.
Tools run on the host thread that pumps the MCP server, so a large unbounded
walk would stall the frame or server tick.

## Factory lifecycle tools

Factory tools appear only in a data-factory host. They operate on a world owned
by the factory, rather than an interactive world selected by a user.

| Tool | Purpose | Required identity |
| --- | --- | --- |
| `world_create` | Create one local factory-owned world, initially paused. | Requested `instance_id`, seed, tick rate, operation ID. |
| `lifecycle_inspect` | Read the completed lifecycle revision without changing it. | `instance_id`. |
| `world_reset` | Replace an owned paused world with the supplied seed and rate. | Current revision and operation ID. |
| `pause`, `resume`, `step` | Control an existing factory world at a completed boundary. | Current revision, plus an operation ID when supplied or required. |
| `snapshot` | Retain an immutable world boundary for aligned reads and capture. | Current revision. |
| `checkpoint`, `restore` | Retain or restore a host-supported checkpoint. | Current revision and checkpoint ID where required. |
| `world_retire` | Remove an owned paused world and return its tombstone. | Current revision and operation ID. |
| `run_script_package` | Atomically replace a paused factory world with a validated Luau package. | Current revision, package bytes, hashes, and operation ID. |
| `data_factory_operation_audit` | Read redacted recent rows from the bounded replay ledger. | Optional bounded limit. |

The complete identity of a factory boundary is:

```text
instance_id + tick + world_epoch + world_version
```

Retain all four values. Factory lifecycle, package, capture, and revision-fenced
scene operations reject stale input with `version_conflict`. A reset or restore
can create a fresh epoch, so an earlier revision is invalid after it. A snapshot
is immutable, but does not replace the revision fence on later calls.

Factory lifecycle mutations use a bounded replay ledger. Assign one stable
`operation_id` to each intended mutation. On a timeout after a request may have
reached the host, retry the same tool with the exact same wire arguments and
operation ID. The host returns the recorded outcome. The same ID with different
wire input is a conflict, not a second request. Do not assume a general editing,
package, or capture tool has this lifecycle replay behavior unless its advertised
schema and result contract say so.

Use `pause(scope="all_systems")` before a package replacement, snapshot, or
deterministic step. `physics_only` does not provide that boundary. `step` accepts
the world's canonical rational `dt_ns`; nonempty actions remain unsupported
until a host installs an action executor. Do not wait inside a Luau task after
an all-systems pause because the scheduler is paused too. Poll external MCP work
from the client process.

## Scene, export, and capture reference

These rows are added only when the host has the backing scene or renderer
service. The typed methods in `datafactories-docs/api.py` call the chunk and
release rows automatically where appropriate.

| Group | Tools | Purpose |
| --- | --- | --- |
| Scene state | `get_scene_snapshot`, `get_camera_rendering_data` | Read the bounded stable-ID scene record and camera calibration at a revision. |
| Scene exports | `export_gltf_scene`, `get_gltf_scene_chunk`, `release_gltf_scene` | Export one fenced GLB inline or through a retained ranged resource. |
| Raw exports | `begin_raw_scene_extract`, `get_raw_scene_chunk`, `release_raw_scene_extract` | Read the raw geometry and source-texture manifest plus its retained bytes. |
| Capture discovery | `get_capture_channels`, `get_resources` | Discover capture and durable-resource capabilities without guessing a schema. |
| Capture jobs | `capture_bundle`, `poll_capture`, `get_resource`, `cancel_capture`, `release_capture` | Queue capture, poll terminal state, read planes, cancel pending work, and release retained data. |
| Scene observations | `get_temporal_sample`, `get_event_narratives`, `get_authored_affordances`, `raycast`, `overlap_aabb`, `overlap_obb`, `get_collider_occupancy`, `get_collider_bev`, `get_filled_occupancy`, `get_signed_distance_field`, `get_authored_navmesh_path`, `get_rig_export` | Read bounded temporal, authored, spatial, navigation, occupancy, and rig observations. |

`get_resources` can explicitly report that a durable-resource owner is
unavailable. That is an availability result, not an empty resource list. A
client must not invent the missing schema or substitute a capture resource.

## Build a repeatable scene or demo

Use an `atomic.data-script.v1` package for a demo or generated scene. The host
type-checks it first, then runs the package in a fresh capability-limited Luau
sandbox and atomically replaces a paused factory world. For a tiny live patch,
use a general edit tool only after inspecting the advertised class and property
schema.

Every exported entity needs a unique stable string `DataFactoryId`. Add distinct
`DataFactorySemanticId` and `DataFactoryPartId` attributes for those labels.
Never derive any of these IDs from an ECS handle, entity index, declaration
order, or display name. IDs must survive replay, export, and another process.

`data-scene/v1` emits at most 10,000 identified entities. A stable ID is at
most 256 UTF-8 bytes. Unidentified entities are reported as omitted; the engine
does not invent a label. Duplicate and overlong IDs are explicit failures.

Here is a compact scene with one labelled part and camera:

```lua
local root = Instance.new("Folder")
root.Name = "DataFactoryPackageDemo"
root.Parent = workspace

local part = Instance.new("Part")
part.Name = "VisibleBox"
part:SetAttribute("DataFactoryId", "data-factory-package/visible")
part:SetAttribute("DataFactorySemanticId", "data-factory-package/semantic/box")
part:SetAttribute("DataFactoryPartId", "data-factory-package/part/visible")
part.Position = Vector3.new(0, 1, 0)
part.Size = Vector3.new(2, 2, 2)
part.Anchored = true
part.Parent = root

local camera = Instance.new("Camera")
camera.Name = "DataFactoryCamera"
camera:SetAttribute("DataFactoryId", "data-factory-package/camera")
camera.CFrame = CFrame.new(0, 2, 8)
camera.ImageWidth = 640
camera.ImageHeight = 360
camera.Parent = root
workspace.CurrentCamera = camera
```

The package contract has hard limits: a 64 KiB canonical manifest, 256 KiB of
source, up to 128 assets totalling 16 MiB, and at most 64 parameters. Source and
asset digests are BLAKE3-256. Package entries must be regular relative files
below the supplied package root. The typed client checks these limits before it
sends a request.

This Python helper creates an owned world, applies `demo-scene.luau`, and
retires the world if setup fails after creation:

```python
from pathlib import Path

import api
from packages import DataScriptBudget, DataScriptManifest


async def create_demo(
    control: api.EngineClient,
) -> tuple[api.EngineClient, api.ScriptPackageSupported]:
    created = await control.create_world(
        "factory-demo-001", seed=42, tick_rate=60.0, operation_id="demo-create"
    )
    world = api.EngineClient(control.transport, created.instance_id)
    try:
        source_path = Path("demo-scene.luau")
        source = source_path.read_bytes()
        checked = await world.script_check(source.decode("utf-8"))
        if not isinstance(checked, api.ScriptCheckResult) or not checked.ok:
            raise RuntimeError(f"Luau check failed: {checked}")

        manifest = DataScriptManifest(
            source_path.name,
            api._blake3_256(source),
            budget=DataScriptBudget(source_bytes=len(source)),
            seed=42,
        )
        applied = await world.run_script_package(
            source_path.parent,
            manifest,
            expected_tick=created.tick,
            expected_world_epoch=created.world_epoch,
            expected_world_version=created.world_version,
            operation_id="demo-package",
        )
        if not isinstance(applied, api.ScriptPackageSupported):
            raise RuntimeError(f"package was not applied: {applied}")
        return world, applied
    except BaseException:
        revision = await world.lifecycle_inspect()
        await world.retire_world(
            expected_tick=revision.tick,
            expected_world_epoch=revision.world_epoch,
            expected_world_version=revision.world_version,
            operation_id="demo-create-failed-retire",
        )
        raise
```

The sibling `export_raw_scene_gltf_live.py` command is the complete owned-world
example. It creates, validates, packages, extracts, converts, verifies, and
retires one scene:

```console
$ mkdir -p output
$ uv run --extra mcp python export_raw_scene_gltf_live.py \
    demo-scene.luau output/demo-001 --instance-id factory-control --seed 42
```

The destination directory must not already exist. The command rejects recorded
converter losses unless `--allow-losses` is explicit.

## Read scene state and camera data

After a package returns its new revision, snapshot the world and use the exact
snapshot revision for every aligned read:

```python
world, applied = await create_demo(control)
snapshot = await world.snapshot(
    expected_tick=applied.result["tick"],
    expected_world_epoch=applied.result["world_epoch"],
    expected_world_version=applied.result["world_version"],
    operation_id="demo-snapshot",
)

scene = await world.get_scene_snapshot(
    expected_tick=snapshot.tick,
    expected_world_epoch=snapshot.world_epoch,
    expected_world_version=snapshot.world_version,
)
camera = await world.get_camera_rendering_data(
    expected_tick=snapshot.tick,
    expected_world_epoch=snapshot.world_epoch,
    expected_world_version=snapshot.world_version,
)

for entity in scene.entities:
    pose = entity.get("world_from_object")
    size = entity.get("size_metres")
    print(entity["id"], pose if pose is not None else "pose unavailable", size)
```

`get_scene_snapshot` returns `data-scene/v1`, a bounded explicitly identified
subset rather than a hidden complete world dump. It contains time, stable entity
records, resolved-lighting provenance, coverage, and omission counts. Do not
treat an omitted entity as absent or invisible.

If the snapshot exceeds the 64 KiB MCP reply limit, the tool returns a
`data-scene-resource/v1` manifest. Read its immutable JSON bytes with
`get_scene_snapshot_chunk` using `resource_id`, `hash`, and contiguous
`byte_begin`/`byte_end` ranges no larger than `chunk_byte_limit`. Concatenate
decoded base64 chunks, verify the declared BLAKE3 hash and byte length, parse
the result as `data-scene/v1`, then call `release_scene_snapshot`. Factory reads
must pass the same revision options to each chunk call. A changed revision
refuses the read; release remains available after the revision changes.

`get_camera_rendering_data` reads calibration for a stable authored camera ID or
the active camera. Its reply states the source of intrinsics and extrinsics, axes,
units, image convention, and whether an exact resolved projection is available.
Authored visibility is not rendered visibility, and a projection must not be
inferred when the reply marks it unavailable.

Scene coordinates are right-handed, +Y up, in metres. Quaternions are XYZW.

## Extract geometry, source textures, and GLB

Use the export that matches the downstream workflow:

| Export | Typed method | Result | Use |
| --- | --- | --- | --- |
| glTF scene | `export_gltf_scene()` | Verified `gltf-scene/v1` GLB | Interchange with DCC and glTF tools. |
| Raw scene | `extract_raw_scene()` | `raw-scene/v2` manifest plus source bytes | Preserve engine geometry and texture data for a custom converter. |

A GLB through 40 KiB is returned inline. A larger GLB is retained by the host,
capped at 320 MiB, and read in ranges of at most 1 MiB. A raw-scene export is
always a retained resource with the same 320 MiB total and 1 MiB range caps. The
typed methods verify BLAKE3-256 and release retained resources in `finally`.
Manual calls to `get_gltf_scene_chunk`, `get_raw_scene_chunk`, or their release
tools must provide the same cleanup on every success and failure path.

```python
raw = await world.extract_raw_scene(
    expected_tick=snapshot.tick,
    expected_world_epoch=snapshot.world_epoch,
    expected_world_version=snapshot.world_version,
)
assert raw.manifest.coordinate_system == "right_handed_y_up"
assert raw.manifest.units == "metres"
for node in raw.manifest.nodes:
    print(node["stable_id"], node.get("mesh"), node["position"])
for unavailable in raw.unavailable:
    print(unavailable.stable_id, unavailable.feature, unavailable.reason)

gltf = await world.export_gltf_scene(
    expected_tick=snapshot.tick,
    expected_world_epoch=snapshot.world_epoch,
    expected_world_version=snapshot.world_version,
)
output = Path("output/demo.glb")
output.parent.mkdir(parents=True, exist_ok=True)
output.write_bytes(gltf.glb)
```

The raw-scene manifest names mesh vertex and index byte sections, source texture
byte sections, nodes, cameras, lights, units, and axes. Material facts belong to
submeshes and nodes. The extraction reply carries unavailable features beside the
manifest. Vertex records are 48-byte little-endian values: position `float32x3`,
normal `float32x3`, UV `float32x2`, joints `uint16x4`, and normalized weights
`uint16x4`. Indices are little-endian `uint32`. Source textures are
`rgba8_unorm` or `r8_unorm` with their declared colour space.

The raw-scene manifest is capped at 256 meshes, 256 nodes, 64 textures, and 256
lights. Each mesh accepts at most 262,144 vertices and 786,432 indices. Each
texture is capped at 16 MiB, with 256 MiB across source textures. These are
export limits, not a claim that every scene has every feature. Record
`raw.unavailable` beside downstream data.

`raw_scene_gltf.py` converts a `RawSceneExtract` into the documented
`gltf2_scene/raw-scene-v2-subset` bundle. It writes `scene.glb`, coordinates,
stable-ID entity rows, an explicit loss report, and a SHA-256 manifest. A
converter loss is data to retain or explicitly refuse, not a warning to discard.

## Capture image planes and labels

Capture is optional and asynchronous. First call `get_capture_channels` at the
revision you intend to use. Submit only advertised channels, dimensions, camera
selection modes, queue counts, and capture profiles. A queued capture ticket
means the request was accepted, not that GPU readback bytes are ready.

The core capture lifecycle is:

```text
snapshot -> capture_bundle -> poll_capture until terminal
         -> read declared resources and verify each one -> release_capture
```

Keep a world paused while polling a snapshot-aligned capture. On cancellation or
deadline expiry, call `cancel_capture`, poll to a terminal state, then call
`release_capture`. Terminal tickets also need release, including a rejected or
malformed terminal reply that already allocated a ticket.

Each ready plane names its channel, status, retained and source resource, byte
size, digest, shape, scalar type, row stride, colour space, image origin,
packing, and provenance. Match snapshot, capture frame, camera, crop, extent,
and storage profile before joining it to scene data. Object-ID pixels are not
semantic IDs, and an authored entity does not imply renderer visibility.

`DataSceneOptions` with `submit_capture_options` is the typed one-camera route.
`capture_multi_camera` requires advertised same-frame multi-camera support and
accepts two through six distinct named cameras. The client-side coordinator in
`multicamera_capture.py` can collect bounded tickets, but separately completed
tickets alone do not prove cross-camera atomicity.

Spatial and authored tools such as `raycast`, `overlap_aabb`, `overlap_obb`,
`get_collider_occupancy`, `get_collider_bev`, `get_authored_affordances`, and
`get_event_narratives` are also optional scene observations. Use only rows the
host advertises, retain their revision and snapshot identity, and preserve an
explicit unavailable response instead of fabricating a label.

## Errors and cleanup

`EngineOperationError` reports an engine refusal or a transport failure after a
tool call starts. Its `details` usually contains the engine error code.
`EngineTimeoutError` is its timeout subclass and means the caller stopped
waiting locally. Retry the same replay-supported mutation ID before deciding
whether the mutation happened.

`EngineProtocolError` means a typed reply, identity, range, or digest failed
client validation. It can surface directly from a typed method. Generic
`EngineClient.call()` wraps its own normalization failure as an
`EngineOperationError` with `protocol_error` details. `ValueError` means the
caller supplied invalid local input before a tool call.

Common engine refusal codes are `validation_failed`, `version_conflict`,
`resource_limit`, `identity_conflict`, `resource_unavailable`, and
`capability_unsupported`. The tool description and JSON schema returned by the
running host remain authoritative for a particular executable and build.

Always retire an owned world, and never retire a user-selected interactive one:

```python
world, _ = await create_demo(control)
try:
    # Snapshot, observe, export, or capture here.
    pass
finally:
    revision = await world.lifecycle_inspect()
    await world.retire_world(
        expected_tick=revision.tick,
        expected_world_epoch=revision.world_epoch,
        expected_world_version=revision.world_version,
        operation_id="demo-retire",
    )
```

For any active capture ticket, cancel if it is pending, observe a terminal state,
release it, then retire the world. Persist the world revision, operation IDs,
returned hashes, unavailable facts, and converter losses with the extracted
dataset so an interrupted workflow can be diagnosed and replayed safely.
