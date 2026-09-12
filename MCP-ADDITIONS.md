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

## Luau and image buffers

Add `EditableImage:ToBuffer()` and `EditableImage:FromBuffer(buffer)`. They use
copied, tightly packed RGBA8 pixels: exactly `width * height * 4` bytes for the
pre-sized image. `FromBuffer` is an instance method that replaces its image's
pixels after exact-length validation. It does not create an image from arbitrary
dimensions, variable stride, or guessed color space.

The value is a Luau binary `buffer`, not a JSON list of bytes. Its descriptor
states dimensions, RGBA order, alpha convention, and color encoding outside the
raw bytes. `WritePixels` and `FromBuffer` modify in-memory image state only. A
capture or resource write materializes a durable artifact.

For replay, scripts must use named session RNG streams and an explicit
replayable-state protocol. Participating scripts serialize and restore declared
state and record external inputs as pinned resources. Arbitrary VM stacks,
yielding coroutines, closures, caches, network traffic, and external side effects
are not assumed serializable. Unsupported scripts may generate one-way scenes,
but an exact checkpoint fails or downgrades instead of claiming restoration.

## Time, pause, fixed step, and rewind

`pause` completes at the next fixed tick boundary. While paused,
`step(... dt_ns=canonical_fixed_interval ...)` advances one and only one
simulation interval. It reports requested and completed tick, time, accepted
actions, world version, and state hash if available. Variable `dt_ns` is a
separately negotiated integration mode. It is not an exact-invariance test.

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

## Proposed external factory workflow

This is pseudocode for a `datafactories` package. Local collector functions are
not new engine MCP APIs.

```text
spec = load_factory_package("occlusion_lighting_v1")
typecheck_luau(spec.setup_source)
execute_luau(spec.setup_source, target="edit", operation_id=setup_hash)
reset_world(instance_id, seed=spec.seed, scene_spec=spec.scene_spec)

pause(instance_id, expected_tick=0)
base = checkpoint(instance_id)
snapshot_0 = snapshot(instance_id, components=spec.components, limit=spec.limit)
captures_0 = capture(instance_id, snapshot_id=snapshot_0.id,
                     cameras=spec.cameras, channels=spec.channels)

for action in spec.fixed_tick_actions:
    step(instance_id, dt_ns=spec.fixed_dt_ns, actions=action,
         expected_tick=current_tick, operation_id=action.id)
    aligned = snapshot(instance_id, components=spec.components, limit=spec.limit)
    capture(instance_id, snapshot_id=aligned.id,
            cameras=spec.cameras, channels=spec.channels)

for intervention in spec.counterfactuals:
    restore(instance_id, base.id)
    branch = apply_intervention(instance_id, base_snapshot_id=snapshot_0.id,
                                changed_causes=intervention, operation_id=intervention.id)
    counterfactual = step_and_capture(branch, spec.capture_plan)

for candidate in external_inverse_model(captures_0):
    restore(instance_id, base.id)
    apply_intervention(instance_id, base_snapshot_id=snapshot_0.id,
                       changed_causes=candidate.scene_patch)
    rerender = capture(instance_id, snapshot_id=snapshot(...).id,
                       cameras=spec.cameras, channels=spec.inverse_channels)
    record_candidate_and_rerender(candidate, rerender)

manifest = write_external_manifest(all_resources, input_evidence, provenance)
wait_for_resource_fences(all_resources)
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

## Source coverage

This proposal incorporates all six top-level Markdown documents in
`datafactories-docs`: `README.md`, `MCP-EXTENSIONS.md`,
`ENGINE_CAPABILITIES_ANALYSIS.md`, `EXTRA-IDEAS.md`,
`LOOPED-WORLD-MODEL-EXTENSION.md`, and `UNIFIED-WORLD-MODEL-v2.md`. It follows
the source contract's division between a narrow engine dataset delta and external
collector and training ownership.
