# Proposed Data Factory MCP Additions

## Status and boundary

This is a proposed engine contract for the `datafactories` repository. It is not
an implementation inventory. A feature described as complete in research notes
is not proof it exists in this engine or meets this contract.

The external data factory owns experiment plans, parameter distributions, split
policy, task descriptors, collector storage, dataset layout, scoring, and model
training. The engine owns bounded operations against a leased or temporary world:
build or edit it through Luau, reset it, step it, atomically observe it, capture
render channels, checkpoint it where supported, and return durable resources.

This extends the existing Studio MCP vocabulary and proposed `DataSceneService`.
It does not add a parallel DataFactoryService, session protocol, generic asset
registry, scene creator, or batch scheduler. Existing `instance_id` and playtest
lifecycle scope the world. Existing `operation_id` and `get_request_status`
handle long operations and retry recovery.

The engine is authoritative only for its encoded world, physics, renderer, and
audio model. A channel is renderer truth only for its named pass and configuration.
It is not a claim about the real world or a unique hidden scene behind an image.

## Principles

1. A sample is reproducible only from pinned scene and asset dependencies, script
   source hashes, configuration, seed, action log, versions, and determinism
   grade. A seed alone is not enough.
2. Stable string names cross save, script, MCP, and artifact boundaries. The MCP
   adapter can keep a lossless opaque integer mapping within one export, but no
   ECS handle, declaration-order number, or `core::Name::Id()` is serialized.
3. State, observations, semantic labels, language labels, and inferred labels
   have separate provenance. Missing, unknown, hidden, inferred, and physically
   true are different values.
4. A capture binds all channels to an explicit `snapshot_id`. Clock equality does
   not prove state and pixels agree.
5. Render-only work advances no tick, simulation RNG, physics, scripts, animation,
   or particles. Renderer-local history or RNG changes are declared.
6. Large payloads are resources with schema, shape, type, checksum, and snapshot
   ID, fetched by `get_resource`, never JSON arrays.
7. A capability is usable only when `negotiate{}` advertises it.

## Canonical MCP surface

### Reused Studio tools

Scene construction and bulk edits use `execute_luau{code,target=edit}`, with
`set_properties` for small edits and `import_rbxm` where appropriate. Runtime
scene behavior uses `eval_server_runtime` or `eval_client_runtime`. Script source
tools, selection, place inspection, Studio playtest lifecycle, logs, and edit
viewport screenshots retain their upstream names and semantics. `typecheck_luau`
is the only proposed wrapper around the Luau frontend and has no world effects.

| Need | Canonical operation |
|---|---|
| Build or modify a scene | `execute_luau`, `set_properties`, `import_rbxm` |
| Inspect or edit scripts | Existing source and line-edit tools, `grep_scripts` |
| Run runtime scripts | `eval_server_runtime`, `eval_client_runtime` |
| Check script source | `typecheck_luau` |
| Lease a world | `manage_instance`, `get_connected_instances`, playtest tools |
| Recover an operation | `get_request_status{operation_id}` |
| Fetch a large result | `get_resource{id, options}` |

### Dataset delta

| Tool | Proposed contract |
|---|---|
| `negotiate{}` | Pure capability discovery. It does not create, load, reset, or mutate a world. |
| `reset_world{instance_id, seed, scene_spec?}` | Starts a deterministic episode in an already leased world and invalidates its prior idempotency ledger. |
| `pause`, `resume` | Quiesce or continue at a completed tick boundary, guarded by `expected_tick`. |
| `step{instance_id, dt_ns, actions, expected_tick, operation_id}` | Advances one declared interval and reports tick and integer nanosecond time. |
| `snapshot{instance_id, components, limit}` | Produces a barriered atomic snapshot. Overflow fails rather than truncates. |
| `capture{instance_id, snapshot_id?, channels, ...}` | Captures negotiated truth channels bound to one snapshot. |
| `capture_render_screenshot{instance_id, camera, ...}` | Game-quality arbitrary-camera render without starting or advancing playtest. |
| `checkpoint`, `restore` | Saves and restores an explicitly declared state level. |
| `apply_intervention`, `step_and_capture` | Version-checked cause changes and atomic causal capture. |

`DataSceneService` is the Luau runtime surface for capture, rendering data,
snapshots, and causal operations. It maps to these MCP operations. It must not
add duplicate wire tools such as `run_script`, `create_scene`, `list_assets`, or
generic entity-component editing.

### Mutation, recovery, and error envelope

Every stateful reply carries `instance_id`, `episode_id`, `world_epoch`,
`world_version`, `tick`, `time_ns`, and the accepted `operation_id` where one
was supplied. `reset_world` starts a new `episode_id`; `restore` creates a fresh
`world_epoch` and invalidates snapshots from the old epoch. A client cannot use a
snapshot acquired before restore as an intervention base after restore.

Mutations accept the expected tick and, where applicable, expected world version
and base snapshot. The engine applies a mutation transactionally: validate all
preconditions, apply all changes, advance the version once, or roll back the
operation. A debug partial-commit mode is explicitly requested and permanently
marked in the result. The result uses one of these structured outcomes:

| Code | Meaning and required client action |
|---|---|
| `version_conflict` | Re-read state. Do not retry against a new world implicitly. |
| `stale_snapshot` | Acquire a new snapshot in the current epoch. |
| `capability_unsupported` | Select a negotiated fallback or reject the sample. |
| `validation_failed` | Correct typed arguments or script output. No world mutation occurred. |
| `resource_limit` | Reduce declared bounds or defer work. |
| `cancelled_before_step` | No tick advanced. Check status, then use a new operation ID to resubmit. |
| `cancelled_after_step` | Result reports completed tick range and partial resources. Reconcile before continuing. |
| `readback_failed` | Simulation may have advanced, but no promised artifact exists. Never blindly retry the step. |
| `restore_incomplete` | Downgrade or reject according to requested determinism grade. |

The idempotency ledger is scoped by instance, episode, and world epoch. It stores
operation ID, canonical argument hash, result, and resource references. The same
ID with the same arguments returns the stored result; the same ID with different
arguments fails. A delayed request from an older epoch is rejected. A transport
timeout is recovered with `get_request_status`, not a blind second mutation.

`reset_world` is never auto-retried. `capture`, checkpoint, and resource export
can be cancelled before GPU work starts. A cancelled request remains the stored
result for its operation ID. Once simulation advances, cancellation reports the
exact completed range and resource state. A failed asynchronous readback does not
rewind a completed step or license another step under the old expected tick.

## Luau and image buffers

Add `EditableImage:ToBuffer()` and `EditableImage:FromBuffer(buffer)`. They use
copied, tightly packed RGBA8 pixels: exactly `width * height * 4` bytes for the
pre-sized image. `FromBuffer` is an instance method that replaces its image's
pixels after exact-length validation. It does not create an image from arbitrary
dimensions, variable stride, or guessed color space.

The value is a Luau binary `buffer`, not a JSON list of bytes. Its descriptor
states dimensions, RGBA order, top-left row order, straight-alpha convention, and
color encoding outside the raw bytes. Typed float captures are separate resource
formats, never silently converted through RGBA8. `WritePixels` and `FromBuffer`
modify in-memory image state only. A capture or resource write materializes a
durable artifact.

For replay, scripts must use named session RNG streams and an explicit
replayable-state protocol. Participating scripts serialize and restore declared
state and record external inputs as pinned resources. Arbitrary VM stacks,
yielding coroutines, closures, caches, network traffic, and external side effects
are not assumed serializable. Unsupported scripts may generate one-way scenes,
but an exact checkpoint fails or downgrades instead of claiming restoration.

An external factory package declares permitted scene roots, Luau module roots,
pinned source and asset hashes, typed parameters, seed streams, requested
capabilities, and output limits. The engine resolves those roots before execution
and records the resolved hashes, imported modules, module-cache policy, script
result, logs, and readiness checks. A package must declare when setup is complete:
all required assets resolved, scene services registered, required entities named,
and requested render resources ready. GPU shader compilation, texture upload,
and physics settling are separate readiness states with bounded timeout and
budget. Network, persistence, purchase, teleport, and undeclared filesystem APIs
error rather than silently no-op in a reproducible temporary world.

## Time, pause, fixed step, and rewind

`pause` completes at the next fixed tick boundary. While paused,
`step(... dt_ns=canonical_fixed_interval ...)` advances one and only one
simulation interval. It reports requested and completed tick, time, accepted
actions, world version, and state hash if available. Variable `dt_ns` is a
separately negotiated integration mode. It is not an exact-invariance test.

Negotiation distinguishes `all_systems` pause from `physics_only` pause. The
former stops scripts, physics, animation, particles, timers, audio simulation,
and tick-driven scene services. The latter is a diagnostic mode whose still-live
systems are named in the result; it cannot support an atomic simulation label.
The canonical fixed rate is represented as a rational numerator and denominator
in nanoseconds. The tick clock carries an integer remainder so repeated stepping
does not accumulate rounded floating-point time.

Render-only capture binds to a snapshot and steps no simulation state. Rendering
can still mutate renderer-local history or consume a renderer seed. The engine
either preserves that history, records its new state, resets it by declared
policy, or labels the capture history independent.

There is no generic reverse-physics operation. Proposed step-back behavior is
checkpoint replay: restore a compatible checkpoint and replay recorded actions
forward to the target tick. An external factory may name this `rewind_to`, but
the engine result states checkpoint used, replay range, state level, hashes, and
divergence. It never implies backward integration. Forking begins before an edit,
keeps branches isolated, and retains effects outside the edited spatial region.

## Checkpoints and state limits

Capability negotiation declares checkpoint levels and guarantees:

| Level | Required contents | Guarantee |
|---|---|---|
| `scene` | Serializable scene, stable strings, pinned asset dependencies, script sources. | Reloadable scene only. |
| `ecs` | Scene plus registered serializable ECS components. | Exact for listed components only. |
| `simulation` | ECS, physics, animation, particles, named RNG streams, clocks, queues, replayable script state, action position, ID mappings. | Exact only when all systems participate. |
| `render` | Simulation state plus camera, render options, exposure, temporal history, renderer seeds. | Comparable temporal rendering in the named backend envelope. |
| `diagnostic` | Render state plus traces and profiler data. | Debug use. Caches may rebuild. |

Every checkpoint resource has a completeness report: included and omitted
subsystems, versions, state hash, pinned assets, and fields that are exact,
quantized, rebuilt, or unsupported. Restoring a lower level invalidates omitted
state. Determinism grades are `exact`, `validated`, `best_effort`, and
`unsupported`. Exact requires the named platform, backend, dependencies, and
byte-identical state restoration. Validated declares tolerance. GPU recreation
commonly prevents a scene hash from promising pixel-identical output.

An exact `simulation` checkpoint includes solver warm-start caches when they
affect later physics, scheduled work queues, timer and clock state, script
protocol state, episode and world epoch, and all named RNG stream positions.
If a cache is deliberately rebuilt, the report says so. Exact continuation then
requires byte-equivalent replay; otherwise the level is only validated and names
its comparison tolerance.

## Atomic multi-camera observations

`snapshot` produces one barriered state. `capture` can use that `snapshot_id`
with a camera list or rig, rendering every camera from the same completed
simulation state. Each output records camera transform, intrinsics, exposure
interval, camera ID, render configuration, history policy, and artifacts.

Simulation must not advance between cameras and still be called synchronized. If
TAA, motion blur, rolling shutter, stochastic sampling, portals, mirrors, or
screen-space effects depend on per-camera history, the manifest says whether
history was restored, advanced in a declared sequence, reset, or unavailable.
Portal and mirror views include recursion and invalidation information.

Every observation names `observation_id`, `snapshot_id`, tick, `time_ns`, camera,
channels, and a completion fence. The fence marks safe GPU readback retention.
World teardown occurs only after promised readbacks fence and resource writes
commit.

A capture request explicitly names camera intrinsics and extrinsics, projection,
near/far planes, coordinate convention, units, viewport crop, lens distortion,
jitter, exposure duration, motion-blur samples, rolling-shutter policy, and
resolution. If a camera is an instance path, its resolved values are copied into
the manifest. Flow resources include forward/backward direction, source and
target times, pixel-center convention, units, validity, occlusion, disocclusion,
out-of-frame, camera-cut, and undefined-motion masks.

Pixel identity is queried by integer pixel coordinates and returns the exact ID
resource value plus coverage and validity. It is never recovered by sampling a
lossy color visualization. Capture may expose visible masks, amodal masks, and
offscreen/occluded entity bounds only when each is named separately. An amodal
mask is simulator-hidden truth, not a visible-observation label.

## Render channels and rendering data

| Family | Examples |
|---|---|
| Visible output | Display RGB, scene-linear HDR RGB, alpha, post-process result. |
| Geometry | Linear/distance depth, positions, normals, tangent frame, motion vectors, disocclusion masks. |
| Identity | Stable entity/object, instance, material, character, class, part, and bone IDs. |
| Visibility | Coverage, visible/occluded state, front contributor, occlusion order, transparency, portal/mirror identity. |
| Lighting | Direct diffuse/specular, indirect terms, emission, AO, shadow factor, light contribution and caster IDs where representable. |
| Material | Base color, roughness, metallic, emissive, opacity, BRDF, UV and texture references where supported. |
| Renderer | Pass graph, resource formats, dropped passes, backend, shader versions, timings, allocation data. |
| Environment | Lens, exposure, tone map, color transform, sky, fog, environment, post-process parameters. |

Each resource declares coordinate system, units, component order, type, shape,
color/alpha conventions, depth convention, invalid encoding, and lossiness. IDs
preserve exact identity. Float truth uses EXR, NPY, or a versioned tensor resource.
A false-color PNG is diagnostic only.

`get_camera_rendering_data` and `get_scene_snapshot` provide aligned structured
truth. They support compact summaries, visible-instance records, and bounded
full exports. Unavailable data is absent with a reason. Negotiated physics truth
includes transforms, bodies, mass, velocities, contacts/manifolds, sleep, forces,
and joints. Character truth includes stable bone names, keypoints, masks,
controller state, and animation. Audio truth includes waveform or buses, sample
clock mapping, source transforms, events, attenuation, and occlusion.

Lighting labels name their source. Direct light may be exact for a renderer pass,
while AO, screen-space reflections, denoised indirect light, clustered assignment,
or caster attribution can be approximate, screen-limited, or unavailable. RGB
comes from geometry, visibility, materials, transport, exposure, tone mapping,
and post-processing. It is not a simple light-times-albedo label.

Reflection and transmission records include their method and limits: ray traced,
probe, planar mirror, portal, SSR, screen-space refraction, or fallback. They
name source view, recursion depth, roughness or ray budgets, validity, staleness,
and whether the contribution is secondary-view truth, a screen-space estimate,
or unavailable. Per-light and per-caster records are bounded by declared limits;
an omitted contribution is not interpreted as zero without an explicit coverage
statement.

## Interventions and provenance

`apply_intervention` uses a base `snapshot_id`, typed old-value preconditions,
stable entity strings, and attribute paths. `step_and_capture` applies an edit
and capture plan under one tick contract. Records include baseline and branch
checkpoints, script or operation hash, cause path, values and units, requested
and actual tick, settings, changed masks, and replay grade.

One edit may change many descendants. A light change can affect shadows,
reflections, exposure, and indirect pixels across the scene. Records distinguish
requested causes, script-declared invariants, observed state changes, and render
changes. An unchanged pixel can mean occlusion, resolution, disabled rendering,
or failed capture. It does not prove no causal effect.

Targets are labeled `observed`, `simulator_hidden`, `user_specified`,
`script_declared`, `inferred`, `generated`, `unknown`, or `unavailable`, with
confidence where meaningful. A task's input-evidence manifest defines what its
model may see. Future observations, hidden objects, and archive records cannot
leak into a current-input task because the factory knows them. Retrieval misses
remain missing evidence, not ground-truth leakage.

Text records event time separately from narration time. Flashbacks, predictions,
beliefs, and narrator statements can refer to a tick other than their surrounding
observation. They retain evidence and certainty and never silently overwrite
simulator state.

The external task descriptor can request points, boxes, polygons, visible or
amodal masks, keypoints, crops, marks, temporal tracks, camera paths, sparse
depth, text instructions, and before/after edit programs. It should progress
from points to boxes to masks and use negative pointing, view transfer, camera
jitter, occlusion interventions, attribute dropout, and held-out combinations
as curriculum options. These are task configurations, not new engine APIs.

When negotiated, structured scene exports include spatial-query truth for
raycasts, AABB/OBB overlap, occupancy, signed distance fields, bird's-eye maps,
navigation, and affordances. Each query identifies its coordinate system,
resolution, collision layer, dynamic-state tick, and whether its semantics are
authored, derived, or inferred. Character knowledge, beliefs, plans, and
perception are separate records from physical state. Durable evidence records
have source IDs, supersession/staleness status, deduplication links, and an
explicit repair path; missing evidence remains missing rather than silently
filled from a later archive entry.

## Resources and external manifests

Engine results return small metadata and resource IDs. `get_resource` returns
bounded previews or ranges. Resources have MIME type, schema version, shape,
type, byte size, checksum, `snapshot_id`, producer operation, and dependency
hashes.

The external factory writes an append-only dataset manifest linking pinned scene
and assets, scripts and arguments, engine/renderer versions, seeds, action log,
checkpoints, snapshots, observations, interventions, checksums, validation, and
task inputs/targets. Resources survive temporary-world teardown because retention
is outside world lifetime. The collector releases them only after manifest commit
and its retention policy permit it.

Negotiation publishes maximum script bytes, entities, pixels, channels, image
bytes, checkpoint bytes, readbacks in flight, render recursion, operation time,
and resource TTL. Resource replies declare expiration and whether a collector may
pin them. Pinning consumes an explicit quota. A collector fetches every required
range, verifies checksum and schema, then atomically finalizes its manifest. On
crash it resumes from the last committed manifest entry and resource checksums,
not from a guessed world state.

Offscreen GPU rendering and CPU headless simulation are distinct capabilities.
CPU headless execution may generate state labels while lacking renderer truth;
offscreen rendering may require a device, shader cache, texture residency, and
asynchronous readback budget. The factory records which path produced every
artifact and rejects a requested render channel when that path cannot supply it.

Interop is declared as defined subsets, never a universal round trip. glTF and
USD exports name supported scene, material, animation, camera, and extension
subsets. COCO, YOLO, GeoJSON, and WKT exports use sidecars for stable string
names, coordinate units, camera conventions, class labels, masks, and temporal
references. Each export writes a loss report for properties its target cannot
preserve.

## Proposed external factory workflow

This is pseudocode for a `datafactories` package. Local collector functions are
not new engine MCP APIs.

```text
spec = load_factory_package("occlusion_lighting_v1")
instance_id = lease_temporary_world_external_to_mcp()
caps = negotiate({contract_version, requested_channels=spec.channels})
typecheck_luau(spec.setup_source)
reset_world(instance_id, seed=spec.seed, scene_spec=spec.scene_spec)
execute_luau(spec.setup_source, target="edit", operation_id=setup_hash)
assert_ready_external(spec.readiness, caps)

clock = get_current_tick_external_from_last_engine_reply()
paused = pause(instance_id, expected_tick=clock.tick)
clock = paused.clock
base = checkpoint(instance_id)
snapshot_0 = snapshot(instance_id, components=spec.components, limit=spec.limit)
captures_0 = capture(instance_id, snapshot_id=snapshot_0.id,
                     cameras=spec.cameras, channels=spec.channels)

for action in spec.fixed_tick_actions:
    stepped = step(instance_id, dt_ns=spec.next_dt(clock.tick), actions=action,
                   expected_tick=clock.tick, operation_id=action.id)
    clock = stepped.clock
    aligned = snapshot(instance_id, components=spec.components, limit=spec.limit)
    capture(instance_id, snapshot_id=aligned.id,
            cameras=spec.cameras, channels=spec.channels)

for intervention in spec.counterfactuals:
    restore(instance_id, base.id)
    branch_base = snapshot(instance_id, components=spec.components, limit=spec.limit)
    branch = apply_intervention(instance_id, base_snapshot_id=branch_base.id,
                                changed_causes=intervention, operation_id=intervention.id)
    counterfactual = step_and_capture({
        step_args={instance_id, dt_ns=spec.next_dt(branch.tick), actions=[],
                   expected_tick=branch.tick, operation_id=intervention.step_id},
        capture_args=spec.capture_plan
    })

for candidate in external_inverse_model(captures_0):
    restore(instance_id, base.id)
    candidate_base = snapshot(instance_id, components=spec.components, limit=spec.limit)
    apply_intervention(instance_id, base_snapshot_id=candidate_base.id,
                       changed_causes=candidate.scene_patch, operation_id=candidate.id)
    candidate_snapshot = snapshot(instance_id, components=spec.components, limit=spec.limit)
    rerender = capture(instance_id, snapshot_id=candidate_snapshot.id,
                       cameras=spec.cameras, channels=spec.inverse_channels)
    record_candidate_and_rerender(candidate, rerender)

fetch_and_verify_resource_bytes(all_resources)
atomically_finalize_external_manifest(all_resources, input_evidence, provenance)
teardown_temporary_world(instance_id)
```

The inverse loop records scene candidates as hypotheses. It never claims a
candidate is the unique source of image, video, audio, or text evidence.

## Forward, inverse, and control task matrix

| Product | Inputs | Targets or controls | Provenance rule |
|---|---|---|---|
| Geometry | RGB, video, sparse points | Depth, normals, masks, flow, occlusion | Mark screen-limited and hidden geometry separately. |
| Lighting and PBR | RGB plus controls | Shadows, AO, terms, BRDF, material, environment | Separate exact pass terms from approximations. |
| Camera | Image/video/text control | Pose, intrinsics, exposure, path, crop, BEV | Preserve units and camera convention. |
| Physics and action | Text/image/video/state | Contact, force, trajectory, action, affordance | Fixed and variable step labels differ. |
| Multimodal generation | Text, image, video, audio, state | Text, image, video, audio, structured state | Map spans, frames, samples, points, entities, and time. |
| Control following | Text, points, masks, edit programs, controls | Render, video, state patch, action | Retain requested control and observed result. |
| Inverse scene | Image, video, text, audio, mixed evidence | Scene hypotheses, camera/light/material/pose/relations | Include ambiguity and hidden-state uncertainty. |
| Counterfactual | Baseline plus intervention | Changed and invariant state/observations | Compare branches from one checkpoint. |

External manifests keep alignment maps: text span to entity/event, point or mask
to stable ID, video frame to tick, audio interval to tick, and control token to
scene patch. This supports multimodal tasks without requiring one universal model
or claiming exact cross-modal inversion.

## Acceptance phases

### Phase 1: bounded reproducible stills

Deliver pure negotiation, reset, pause, fixed single step, atomic snapshot,
game-quality render-only screenshot, RGB/depth/normal/entity-ID capture, camera
metadata, `EditableImage` RGBA8 buffers, resources, and source hashes. Accept a
script-built scene reproduced from a pinned manifest on the declared platform.

### Phase 2: replayable counterfactuals

Deliver checkpoint grades, replayable scripts, restore, forked worlds, action
logs, interventions, and divergence reports. Accept a paused scene advanced one
tick, restored, replayed, and compared at the stated grade.

### Phase 3: rich aligned truth

Deliver multi-camera same-snapshot capture, renderer-history reports, lighting
and material provenance, visibility/motion channels, contacts, animation, audio,
and resource validation. Accept a multi-camera lighting or occlusion intervention.

### Phase 4: factory integration

The external collector adds task configs, split rules, retention, leakage checks,
metrics, and training interfaces. Accept a held-out replayable sample group with
complete source provenance and explicit unsupported capabilities.

| Acceptance check | Evidence required |
|---|---|
| Fixed-step replay | Pause, one step, checkpoint restore, and action replay agree at the requested state grade. |
| No-time-advance capture | Tick, simulation RNG, script clock, and physics state are unchanged before and after render-only capture. |
| Multi-camera alignment | All camera resources reference one snapshot and report their temporal-history policy. |
| Label alignment | Projection, depth, IDs, masks, flow validity, and requested structured state agree within declared tolerances. |
| Retry isolation | A repeated operation ID with identical arguments returns stored output; changed arguments fail. |
| Failure recovery | Readback failure reports completed simulation progress and cannot induce a blind duplicate step. |
| Artifact durability | Standard flow finalizes a verified manifest before teardown; a separate retention test proves pinned resources remain fetchable after teardown. |
| Leakage prevention | A held-out task has an input-evidence list proving hidden, future, and archived-only facts were excluded. |

Profile the supported release configuration, not only development builds. Record
capture latency, readback latency, simulation and renderer CPU/GPU time, bytes,
allocations, peak host/device memory, checkpoint size, artifact throughput, and
output validation rate with scene, backend, resolution, channel set, and worker
count. A performance claim requires this evidence rather than a feature list.

## Source coverage

This proposal incorporates all six top-level Markdown documents:

| Source | Contribution to this proposal |
|---|---|
| [README](../datafactories-docs/README.md) | Repository scope. |
| [MCP extensions](../datafactories-docs/MCP-EXTENSIONS.md) | Canonical MCP reuse, Dataset delta, DataSceneService, resource transport. |
| [Engine capabilities analysis](../datafactories-docs/ENGINE_CAPABILITIES_ANALYSIS.md) | Candidate channels and explicit current capability gaps. |
| [Extra ideas](../datafactories-docs/EXTRA-IDEAS.md) | Controlled tasks, curriculum, interventions, and evaluation products. |
| [Looped world model extension](../datafactories-docs/LOOPED-WORLD-MODEL-EXTENSION.md) | Replay, evidence, bounded state, missingness, and counterfactual requirements. |
| [Unified world model v2](../datafactories-docs/UNIFIED-WORLD-MODEL-v2.md) | Provenance, uncertainty, multimodal state, forward/inverse limits, and validation. |

It follows the source contract's division between a narrow engine dataset delta
and external collector and training ownership.
