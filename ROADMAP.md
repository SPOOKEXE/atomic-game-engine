
# ROADMAP

## Editing

Deferred items are for items that need significant systems we cannot do now.
If you can do the item now, do NOT add to the deferred list.

Do NOT add new deferred work as a roadmap item. Place it in
`docs/DEFERRED.md`. If a TODO item is not FULLY completed, split the
TODO item, keeping the concise short dash point list with block infront and
split it as another item under the same version.

For example, if we complete A and B but not C and D:
```
### v0.5
- [x] do: A1, A3, A5
- [_] do: A, B, C, D
- [x] do: G, H, I
- [x] do: K, K2, K3
```

becomes:
```
### v0.5
- [x] do: A1, A3, A5
- [x] do: A, B
- [x] do: G, H, I
- [x] do: K, K2, K3
- [_] do: C, D
- [_] deferred `D0001` for later.
```

or defer to another version.

## VERSIONS

The milestone headings below are development labels. Not in line with project versioning.

### v0.24

Rendering (docs/RENDER-REFACTOR.md) including the consolidated materials, shaders and rendering optimization work:
- [x] Add opt-in render-stage image snapshots, raw pixels, metadata and a visual index. GPU overwrite checks pass; capture stalls affect timing.
- [x] Fix topology renewal after cache expiry and consume ready topology replies before camera routing.
- [x] Add portal startup readiness and Humanoid camera routing fixes. All 16 product crossing variants pass: 30/60 Hz, first/third person, explicit/automatic subject, held/released movement.
- [x] Capture a missing-eye-image black frame at its first render stage; retain useful images and remove bulk captures.
- [x] Reduce editable collision BVH build work and scratch storage. Full Terrain worker profiling remains below.
- [x] Add per-mesh, global-lighting and camera render capability fields to GPU-resident rows so compute passes can branch without CPU readback. Instance policy occupies the existing 64-byte resident row, world and camera policy occupy the shared view uniform, and authored compute nodes can request both without CPU readback.
- [x] Expose the existing render passes as render-pipeline nodes. Every native render pass has a graph node and backend handler; the remaining work below extends that graph with new features.
- [x] Add selectable antialiasing choices as render-pipeline nodes. FXAA, TAA and all three SMAA stages have graph node kinds, default shaders and multi-target authored-raster support. A Vulkan graph fixture runs every choice against a hard diagonal, verifies softened output pixels, and verifies TAA's paired history output.
- [x] Add four authored mesh LOD levels with GPU-side per-instance selection beside occlusion culling. `CustomMeshLOD` overrides `AutoMeshLOD` per level, while nil custom slots fall back to the matching automatic artifact. Each resolved level has its own resident instance row and indirect draw command; a per-view compute pass selects one from projected bounds without returning the result to the CPU. The Vulkan fixture proves near views draw the detailed quad and distant views draw the coarse triangle while preserving the part's authored bounds.
- [x] Allow visual items to attach compute and post-processing shader nodes, resident only while their pipeline is active. Scene records, serialization, BasePart script properties and policy-aware demand checks select lazy graph shaders. Inactive post and compute nodes pass their input through; a Vulkan fixture proves independent activation, output changes, and target retirement when the pipeline is removed.
- [x] Add a demo pipeline that exercises capability toggles, antialiasing, post-processing and authored LOD selection. `RenderFeatures.pipeline` extends the production PBR graph with lazy visual compute and post nodes plus FXAA; `RenderFeaturesDemo.luau` authors world, camera and instance policies, then demonstrates `CustomMeshLOD` overriding and falling back per level to `AutoMeshLOD`. The example fixture checks the staged document against its C++ recipe and executes the scene through Luau.
- [x] Define the visual-compositor graph contract using the Unity Scriptable Render Pipeline and Visual Compositor as references. Resources now enforce access, colour space, alpha interpretation, sample and mip shape, array depth, lifetime, history ownership and byte sizing. Authored graphs round-trip the same contract through Studio: https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html and https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html.
- [x] Move residency and delta upload into nodes, then remove each replaced legacy rendering path. `mesh-residency` owns mesh-table admission and `delta-upload` owns changed instance, skin, indirect, ribbon and overlay transfers. Draw nodes no longer perform hidden upload fallbacks.
- [x] Add the product-side active-scene collector and parallel presentation walk, then batch every active camera across worlds. Complete owned packets keep their pipeline identity on stable world lanes, ordinary display worlds are not reopened, and one renderer submission includes offscreen active cameras plus the display camera.
- [x] Add GPU-side sRGB handling, emissivity, mipmapping and bounding-box-first occlusion culling. Texture formats select linear or sRGB device sampling explicitly, complete mip chains remain resident, instance feature masks gate emissive shading, and HZB culling follows the frustum and bounding-box passes.
- [x] Add proper PBR with tests, dynamic ambient occlusion and render-only displacement maps that do not alter physical transforms. Roughness, metalness, material occlusion, emission and parallax displacement run in the PBR shaders. World, camera and instance feature precedence gates emission, displacement and SSAO without a CPU readback.
- [x] Add Fog, Clouds and Skybox compute-shader nodes, plus screen-space post-processing nodes. Skybox and clouds are separate cached compute stages over fixed, per-world 1024 by 512 history resources; depth fog is a separate screen-space graph stage. Non-compute devices use the tier B fallback graph.
- [x] Add EditableMesh and EditableImage packing and quantization components for float16, E4M3 float8, signed and unsigned integer16, integer8, integer4 and boolean formats. EditableMesh now uploads compact GPU streams and decodes them in packed draw shaders while canonical edit and collision arrays remain unchanged. EditableImage keeps canonical RGBA8 and reports unsupported sampled encodings explicitly.
- [x] Measure many 4k textures on the GPU and test a GPU atlas system before selecting packing defaults. `just gpu-texture-atlas-bench 1` passed with sixteen distinct 4096-square RGBA8 sources over four sequential 8192-square atlas pages: 1 GiB uploaded, 40.44 ms atlas GPU copy versus 44.90 ms standalone, a bounded 512 MiB source-page peak, four transfer operations instead of sixteen, sixteen whole-probe GPU allocations instead of thirty-two, sixteen cold copies, sixteen warm hits and readback validation passed.
- [x] Add automatic mesh decimation as the second LOD generation mode. Content intake builds and publishes deterministic artifacts from the base mesh, shares matching artifacts across worlds, preserves material, winding and skin boundaries, and lets each nil `CustomMeshLOD` slot fall back to the generated `AutoMeshLOD` artifact.
- [x] Profile release CPU and GPU work, residency, caching and transfer bytes after the medium feature set is integrated. The full 15-second `just medium-render-profile` run covers 1, 2 and 8 active worlds through the explicit headless offscreen final target and `--render-pipeline` path. Busiest frames reached 26/57/243 draws and 744/2,232/11,160 triangles; GPU live memory was 133.2/235.2/847.3 MiB; frame means were 0.703/1.598/51.979 ms. Authored demo compute, post and FXAA spans plus GPU delta-upload spans were present.
- [x] Finish Terrain editable collision worker profiling and optimization as a separate performance task. Dirty refresh keeps unrelated ECS-owned BVHs resident, detects missing triangle resources even at a matching revision, and sends eight terrain chunks across the worker dispatch floor. The latest five-sample release run measured the complete eight-chunk refresh beside 2,000 resident shapes at 7.16 ms, with the retained 64-chunk ledger at 50.99 us.
- [x] Complete portal image host and session contracts for fresh destination captures, current-camera routing, capture retention across route and body waits, inverse lens mapping, lease disconnects and player return handoff. Authorization withdrawal now retires source portal and body compositions, with the full shadow integration, focused withdrawal and server grant suites covering lease retirement.
- [x] Add tessellation as a composable render-graph node, with view- and capacity-aware plans, compute-readable resident mesh streams and material-matched draws. The Vulkan hard-render fixture proves the dedicated geometry handler submits the exact compute plan and wins over the authored fallback of the same kind.
- [x] Add bounded screen-space global illumination, ray and path estimators as composable render nodes with view-signature history and submission-safe accumulation. Cached frames execute only the precomputed retained-node closure, path history advances from one to two samples, and a changed view resets it to one. These are screen-space estimators, not acceleration-structure tracing.
- [x] Add demo render pipelines for the bounded screen-space ray and path estimators. The Vulkan fixture captures numeric RGBA16F results and proves retained path, history-store and tone-map stages run while unchanged albedo and global-illumination stages stay cached.
- [x] Add projected-area triangle reduction and GPU cluster selection with indirect draws, coverage culling and no CPU readback during LOD changes.
- [x] Add composable image-processing and visual-compositor nodes, a compositor demo pipeline and signed Studio controls for node settings. The real-device compositor fixture proves numeric changes through exposure, HSV, mix, transform, horizontal and vertical blur, then validates display, scene and output captures.
- [x] Add portal particle and ribbon layer peeling through the transparent-layer path.
- [x] Device-validate foreign-world captures from fresh destination geometry with the current camera, including exact parallax and disocclusion. The 186-assertion Vulkan fixture captures a blue foreign occluder over a red backdrop, preserves that nonblack image while the moved-camera request is pending, then proves the fresh image uses the new camera by revealing the red backdrop at the same pixel.
- [x] Reproduce the original black frame with a valid image handle and retain the last valid image during topology waits. The device regression first reproduces 4,225 black pixels, then passes 203 assertions while the replacement topology is pending.
- [x] Verify seamless player and body crossing, Humanoid camera subjects, camera obstruction, clipping and return trips under delay, restart and lost acknowledgements. The 42,077-assertion product round-trip matrix covers 150 ms latency, presentation-producer restart and two deliberately dropped LeaseAdopted replies. Both dropped replies recover through real lease renewal after the body has left its source authority.
- [x] Verify portal lighting, shadows, transparency, particles, ribbons, spatial UI and animated character accessories through the seam. Two actual portal-exchange Vulkan modes pass 102 and 104 assertions across the complete visual layer set.
- [x] Check oblique, rolled and scaled portal views at all angles, then finish visual review of the non-Euclidean demo. The scripted angle matrix passes 11,920 assertions, and seven rendered tour shots pass 127 Vulkan assertions for oblique, rolled, non-uniformly scaled and return views.

`datafactories-docs/MCP-ADDITIONS.md` defines one public data-factory contract. The engine process is the MCP host. It owns isolated scene lifecycle, validated scene setup and edits, deterministic stepping, structured observations, captures and resources. The external data factory owns recipes, parameters, batching, storage, manifests, training and evaluation. Its Python `api.py` is a typed client for the engine MCP tools, not a second engine API or a training runtime. Agents register the same stdio `mcpbridge`, discover the same schemas and call the same bounded tools. No capability counts as exposed until both an external program and an MCP agent can discover and call it.

Definition of done for each factory capability:

| Layer | Required result | Proof required |
|---|---|---|
| Engine service | Owns bounded scene setup or mutation, time control, and structured or binary export. | Service tests cover valid results, limits, and unsupported cases. |
| Engine-hosted MCP tool | Validates requests and exposes the service through the versioned MCP schema. | MCP tests cover schemas, lifecycle guards, and error envelopes. |
| Typed Python client | Calls the MCP tool through `mcpbridge` with typed bounded requests and replies. | Python tests cover negotiation, decoding, checksums, and failure handling. |
| Agent discovery and callability | An MCP agent discovers and calls the identical advertised tool. | An acceptance test runs Python and agent workflows and compares artifacts. |

Implementation order:

1. **Usable external and agent loop:** from both Python and an agent, connect and negotiate, create or reset an isolated factory-owned world, submit a pinned scene package or a validated patch, drive explicit fixed ticks, then snapshot camera and object poses, sizes, frustum, visibility and 2D projections, poll captures and fetch verified resources. Selecting the existing local client world remains a useful compatibility path, but it does not complete the unattended factory workflow.
2. **Rich render and simulation labels:** add capture-backed visibility, IDs, masks, flow, material, lighting, physics, rig and audio truth on the same snapshot contract.
3. **Scale and inverse tasks:** add atomic multicamera and multiworld batches, complete checkpoint and fork replay, durable dataset finalization, counterfactuals, inverse scene hypotheses and release profiling.

Connect, discover and expose the engine:
- [x] publish one supported engine MCP endpoint, startup flow and server manifest for external programs and MCP agents. `client --data-factory --mcp-port 8736`, the checked `.mcp.json` entry, generic stdio `mcpbridge`, `RUNNING.md` and the served `data-factory` agent prompt expose the same versioned tools, limits and structured unsupported results.
- [_] add an end-to-end acceptance fixture in which the Python client and an MCP agent independently connect, negotiate, run the starter workflow and fetch the same verified artifacts. The Python workflow now passes both its low-level transport fixture and a live client-host run that creates an isolated world, loads the package, warms presentation, re-pauses, captures ten verified planes in one ticket, writes ten PNG views plus one canonical JSON sidecar, releases the capture and retires the world. Independent MCP-agent artifact parity remains open.
- [x] expose capability, version and schema discovery, and report unsupported features explicitly.
- [x] provide the Python sibling API's negotiation, thin reads, lifecycle and ranged, BLAKE3-verified resource reads.
- [_] define MCP idempotency, expected versions, structured status, cancellation, capability limits, permissions and audit records. Capture submission, cancellation and release now carry bounded unique operation IDs and exact ticket identity checks; the full cross-tool policy remains open.
- [_] finish thin MCP adapters for every engine service used by the contract. Verified client adapters now cover factory-owned create, reset and retire, lifecycle control, snapshots, interventions, render-only submission, script packages, scene and physics observations, spatial queries, camera metadata, rigs, audio, event narratives, capabilities, source checking, asynchronous capture, bounded resource reads, cancellation and release. Server and Studio lifecycle hosts, multicamera coordination and remaining render labels are open.
- [_] add the remaining MCP tools and examples for multicamera, multiworld, optical flow, lighting contribution and interop, plus independent MCP-agent artifact coverage for the existing segmentation tools.

Create and modify isolated scenes:
- [_] expose create, select, reset and retire operations for isolated data-factory worlds in client, server and Studio hosts. Client MCP can now create one caller-named local world in an empty host, stage create or reset through a scratch Universe, install product systems before commit, return the new zero-tick world all-systems paused, retain bounded idempotency tombstones and drain captures before retirement. Compatibility worlds cannot be claimed or retired. Server and Studio hosts remain open.
- [_] load repository and inline script packages with source and asset hashes, seeded parameters, type checking, sandboxing and atomic scene edits. The client MCP accepts bounded inline `atomic.data-script.v1` Luau packages, verifies BLAKE3 source and asset hashes plus typed manifest parameters, runs them in a one-shot capability-limited scratch world, and swaps only after a terminal result. Descriptor-safe repository loading lives in the Python client. Static Luau admission loads generated engine declarations, narrows them to the package sandbox, forces strict checking and refuses bounded syntax or type failures before live-world serialization. `DataFactoryPackageDemo.luau` is a rerunnable fixture that replaces only its owned workspace subtree. Server and Studio hosts remain open.
- [x] accept text instructions with reference images, structured controls and video motion constraints, then apply only externally interpreted, engine-validated scene patches. The Python boundary copies and bounds every instruction, artifact, control tree, evidence ID and motion interval. An external interpreter returns a typed candidate grounded in that evidence; only validated `CausalEdit` values cross MCP through `apply_intervention` at an exact snapshot, tick, epoch and version.
- [x] share engine services through Luau `DataSceneService`, with VM-neutral ECS metadata, queued lifecycle work and Luau or JavaScript render bridges.
- [x] expose thin MCP `pause`, `resume`, `step`, `snapshot`, `checkpoint` and `restore` commands.

Drive deterministic time and state:
- [x] support all-system pause for one local client with `--data-factory`, including the SDL device barrier. Factory create and reset establish this barrier atomically before returning their tick-zero revision.
- [x] support physics-only pause, including clock and character gates.
- [x] define rational timing metadata and deterministic manual tick boundaries.
- [_] finish deterministic action and script sequencing at fixed-tick boundaries; rational timing and manual tick boundaries are checked.
- [x] support render-only steps with zero simulation advance and an explicit temporal-history policy. The paused-world session validates retained snapshots and revisions, the client submits one forced frame with zero particle or simulation delta, and Luau, MCP and Python expose bounded submit and poll records. Preserve is supported now; reset and disable return explicit unsupported states until renderer-local history control exists.
- [x] keep snapshots immutable with stable `DataFactoryId` identities and copied clocks.
- [x] keep the draw collector's current transform derived with no clock advance and unchanged serialized snapshot bytes.
- [_] provide full checkpoint coverage for ECS, physics warm start, RNG, script schedulers, events, clocks, string IDs and pinned assets; the API requires a real host rehydrator.
- [_] restore checkpoints only when compatible, and create fresh versions after restore.
- [_] implement backward seek as checkpoint plus replay, never negative dt, with bounded history.
- [_] support forks and versioned causal edits, including effects outside the edited spatial region while keeping branches isolated.

Deliver the first useful structured observation loop:
- [x] add position and rotation values in world, derived parent-relative and stable reference-object space. The Python factory has immutable point, vector, quaternion and rigid-pose records with explicit world IDs, rigid reference validation, canonical JSON and tested bidirectional conversions.
- [x] define camera intrinsics, both extrinsic transform directions, near and far planes, requested resolution, crop, units and coordinate conventions. Scene snapshots include derived world and parent-relative transforms plus bounds-derived sizes; bounded camera observations project identified OBBs with explicit frustum and unavailable occlusion evidence. Exact render projection, lens distortion and temporal jitter remain unavailable until a capture resolves them.
- [x] define exact projection metadata for completed captures.
- [x] add a dedicated OBB geometry test. The core suite checks rotated half extents and containment of every transformed corner.
- [x] provide real headless raycast, AABB and OBB spatial queries through Luau, JavaScript, typed engine calls, thin MCP tools and the Python client. The starter Python workflow records the exact query inputs, snapshot, units, availability and results beside its captured images.
- [x] provide a small `DataFactoryDemo.luau` that creates one camera and three identified objects, reads the scene snapshot and camera object observations, and prints the stable IDs, poses, sizes, visibility and projected bounds.
- [x] retain the broader API walkthrough in `DataFactoryAdvancedDemo.luau`, including metadata reads, image-buffer round trip and capture request setup.
- [_] complete an autonomous MCP capture workflow around the starter demo. The Python workflow prefers the full advertised lifecycle, creates a caller-named isolated world, binds every later call to that world, submits the pinned rerunnable package, optionally warms presentation before re-pausing, snapshots, requires the three stable object IDs, records raycast, AABB and OBB evidence, polls one ten-plane capture with a monotonic deadline, fetches BLAKE3-verified byte ranges, releases or cancels the ticket and retires its world on success or failure. The checked live example persists the exact raw bytes for all ten planes before ticket release, writes `scene.png`, depth, normal, albedo, packed material, emissive, ambient-occlusion, object-ID, semantic-ID and part-ID PNG views, and writes `scene.json` v2 with each raw plane's file, byte size and SHA-256 plus lifecycle, camera, object, geometry-query and exact opaque or masked per-object visible-mask evidence joined to frustum and projected bounds. The ambient-occlusion plane is source-only R8/unorm8 at native half resolution, with raw bytes, PNG and bounded statistics; it reports a 12-sample, radius-0.65 screen-space SSAO visibility factor with no denoiser or temporal history. Value one may mean unoccluded, background or disabled, and the plane is not ground truth, shadow visibility, material occlusion or global ambient occlusion. It uses an existing local world only when the complete lifecycle is absent and never retires that fallback. Occlusion cause, amodal masks and disocclusion remain unavailable. Independent agent artifact parity remains open.
- [_] provide occupancy, SDF, BEV, navmesh and affordance queries with authored semantics.

Capture and persist aligned artifacts:
- [x] add `EditableImage:ToBuffer()` and `EditableImage:FromBuffer(buffer)` RGBA8 support to Luau and the engine.
- [x] validate RGBA8 buffers as exactly `width*height*4`, including orientation, color space, alpha and copy semantics.
- [x] keep typed HDR buffers separate from RGBA8 buffers.
- [x] add the data-capture graph's default PBR data-capture node.
- [x] allow the default capture node to copy source, depth and normal without requiring optional ambient-response and lighting-baseline planes. A Vulkan regression records all three declared planes and rejects an unpaired lighting-response declaration.
- [x] capture RGB linear HDR, depth and packed normals.
- [x] make one-camera asynchronous capture retain one aligned ticket with ranged bytes and exact projection metadata for RGB linear HDR, linear depth, packed shading normals, PBR albedo, packed PBR material, PBR emissive, source-only ambient occlusion, object IDs, semantic IDs and part IDs. Integer label captures include bounded dense sidecars from integer labels to stable strings; background and unidentified opaque or masked gbuffer pixels use zero. The ambient-occlusion plane is R8/unorm8 at native half resolution and carries raw bytes, PNG and bounded statistics for a 12-sample, radius-0.65 screen-space SSAO visibility factor with no denoiser or temporal history. A value of one can mean unoccluded, background or disabled. It is not ground truth, shadow visibility, material occlusion or global ambient occlusion. Transparent surfaces, particles and later composited layers do not write the label planes, so final-composite visibility truth remains open.
- [_] make step plus snapshot plus multicamera capture atomic, with asynchronous readback completion.
- [_] batch scenes on GPU headless or offscreen, with explicit capability and readiness reporting.
- [_] write durable artifact manifests, schemas, checksums and chunks with retention, atomic finalization, crash resume and bounded backpressure. The Python factory already provides durable chunks, sample finalization, pins, recovery, provenance, sweeps, holdouts and metrics.
- [_] maintain acceptance fixtures for replay roundtrip, no-time-advance, image-label alignment, retry isolation, invalid data and Python or agent parity. Headless fixtures cover rerunnable package replacement, exact object, semantic and part ID pixels, stable sidecars, malformed and partial label replies, typed Python parsing and cleanup after malformed terminal captures. The live Python example verifies checksums, persists all ten exact raw plane payloads before release, records each payload's byte size and SHA-256 in `scene.json`, and writes all ten aligned channel views with per-label pixel bounds and counts plus ambient-occlusion statistics. A live MCP-agent artifact-parity run remains open.

Add richer scene, render and multimodal truth:
- [x] capture object IDs as exact `R32_UINT` opaque or masked gbuffer pixels with stable string sidecars and explicit zero background or unidentified values. Transparent surfaces, particles and later composited layers are excluded.
- [_] capture semantic masks and part masks. Opaque and masked gbuffer draws export exact `R32_UINT` `semantic_ids` and `part_ids` planes from authored `DataFactorySemanticId` and `DataFactoryPartId` attributes. Snapshot-scoped dense sidecars retain stable strings, shared semantic classes and distinct parts across regular and packed draws, with zero for background or unidentified geometry. The bounded MCP capture tool, Luau bridge, typed Python client, starter fixtures and live Python artifact example cover both channels. Independent MCP-agent artifact parity and later composited visual layers remain open under the four-layer completion gate above.
- [_] record visibility, occlusion, disocclusion and visible or amodal masks. The live starter `scene.json` v2 records exact opaque or masked gbuffer visible-mask coverage per object, joined to frustum and projected bounds. Occlusion cause, amodal masks and disocclusion remain unavailable.
- [_] record optical flow, motion vectors, trajectories, scene cuts and validity flags. `get_temporal_sample` now binds one camera and up to 64 stable object poses to a verified retained paused snapshot, with exact fixed-tick timing and explicit missing or invalid pose rows. The Python factory builds bounded `temporal-sequence/v1` camera and object tracks, exact rational intervals, interval-average world-space velocity, declared camera-cut provenance and per-link validity. The starter capture saves two consecutive zero-motion samples. Optical flow, pixel motion, disocclusion and continuous renderer-history validity remain open.
- [_] expose PBR albedo, roughness, metallic, emissive, specular, transmission, shading geometry, normals and UV maps. One aligned capture now exports authored sRGB albedo, the raw packed material attachment, linear HDR emissive and packed shading normals through MCP and the typed Python client. The built-in material decode is named R roughness, G metalness, B material occlusion and A reserved; the raw attachment remains authoritative. The example saves each as a viewable PNG while preserving raw type, shape, stride, origin, statistics and checksums in JSON. Specular, transmission, UVs and complete shading-geometry records remain open.
- [_] `data-scene/v1` now exports `lighting_observation`: resolved global lighting with provenance, plus stable identified authored local-light records resolved before portal copying and the camera-dependent 16-light selection. Per-pixel light contribution, shadow caster or receiver attribution, and physical photometry remain explicitly unavailable. This slice does not complete the broader lighting, shadow, or ambient-occlusion truth item.
- [_] support reflections from SSR, probes, mirrors and portals, including secondary views, recursion and staleness.
- [x] describe render-graph passes and resources with budgets and dependencies, without inventing ground truth. Typed Python `get_render_graph` and the starter workflow now live-verify the world-scoped `Default PBR#0` graph and write 36 nodes, 37 resources, 21 dependency waves and logical budget estimates. Timing, physical overlap, driver residency and historical capture identity remain unavailable.
- [_] expose physics contacts, impulses, forces, torque, sleep, assemblies, joints, controller fields and units. `physics-observation/v1` exports bounded identified contacts, event phases, completed-solver impulses with world-space basis, sleep, rigid assembly identity, authored joints, humanoid and input controller fields, units and explicit availability. Persistent force and torque accumulators remain unsupported.
- [_] export rigs, skeletons, keypoints, skinning data and animation tracks. The client MCP returns bounded `data-rig/v1` skeleton and bone frames with stable IDs, exact tick rationals, deterministic ordering, authored semantic keypoints and decoded bounded buffered animation channels. `RigKeypoint` scene instances, Luau authoring, portal transfer, demo coverage and exact v1 validation work. Mesh skin weights remain open.
- [x] synchronize audio waveforms with source events and timing. The audio mixer retains a fixed-capacity, allocation-free applied-command and natural-finish trace; immutable observations copy post-clip float32 samples, exact sample clocks, stable scene source names, spatial state, attenuation provenance and explicit unsupported occlusion state. Client data-factory mode exposes bounded `audio_observation/v1` MCP records and immutable SHA-256 waveform ranges from deterministic full-tick null-device captures. The Python client validates records and reconstructs complete waveforms.
- [x] emit structured event narratives with time, knowledge, belief and provenance fields.
- [x] align text, image, video and audio structured records with controls, grounding points, boxes, masks, crops and marks. The Python factory validates immutable text spans, points, semantic and part masks, control IDs, frame times and audio sample rates against media bounds.
- [x] track source evidence IDs, deduplicate facts, mark stale or missing evidence, and define repair and external-factory ownership. The evidence ledger enforces stable source and fact identities, legal freshness transitions, monotonic observation times, atomic bounded updates and repair-result lineage across engine and external owners.

Scale into dataset generation, evaluation and inverse tasks:
- [x] generate counterfactual pairs, parameter sweeps, domain randomization and holdouts without label leakage. The Python factory builds immutable seeded plans, paired one-axis variants, typed choice and uniform sweeps, connected group-aware holdouts and leakage checks across scenes, pairs, assets, sources and provenance.
- [x] declare interop subsets for glTF, USD, COCO, YOLO, GeoJSON and WKT. The named profiles define represented fields, bundle sidecars, coordinate rules, stable-ID mappings, machine-readable loss classes and atomic import refusal conditions.
- [_] finish broad multimodal and prediction tools in the Python factory. It has immutable bounded results for lifecycle revisions, interventions, render-only submission, durable resources, ten aligned capture channels and artifacts, camera metadata, object, semantic and part segmentation, spatial queries, rigs, audio, event narratives, capability negotiation and engine Luau source checks. Multicamera alignment, final-composite and transparent segmentation, flow, advanced lighting, inverse reconstruction and training task orchestration remain open.
- [_] expose ambient-occlusion estimator provenance. The live starter exports source-only R8/unorm8 SSAO at native half resolution with raw bytes, PNG and bounded statistics. The current estimator uses 12 samples at radius 0.65, with no denoiser or temporal history. Its values are screen-space visibility factors only. Disabled and background pixels can both be one, so this does not provide ground-truth, shadow, material or global ambient-occlusion labels. Enable-state provenance and broader light or shadow contribution remain open.
- [_] support forward scene-to-modalities and inverse observation-to-scene patches, with rerendered numeric and semantic metrics plus ambiguity masks.
- [_] profile release captures for actual bytes, allocations, peak memory, timings, throughput and output quality.

General:
- [_] physics profiler in studio is non-functional. idk if its capturing snapshots or anything, but it shows no values. Use `slide` demo to test it.
- [x] blackhole warp curves inward consistently across spin phases.
- [x] character collision and wall sliding work against objects.
- [x] batch and reuse selection geometry across viewports.
- [x] marquee selection and direct surface drag work, with undo support.
- [x] node canvas dragging works.
- [x] left-click selection has a 150 ms deadzone plus a pointer threshold before dragging.
- [x] add column sorting to asset profiler
- [x] Heap Profiler has a custom millisecond window with Latest, Average, Max and Min, sampled at 1 second resolution.
- [x] Frame Graph has a mode dropdown.
- [x] View > Datastores, rename to View > DataStore Editor
- [x] View > Datastore, rename to View > DataStore Config
- [x] View > CDN, rename to View > CDN Config
- [x] rename View > Physics Solver to View > Physics Profiler, place it under View > Pipeline Profiler, and add stage and flame views, pause, and joined worker timings including tick exchange.
- [x] queue particle:Emit through ECS, drain bursts in batches, and include Enabled continuous emission.
- [x] separate Network Profiler waiting, active wire, ready and failed states, with stage and flame views.
- [x] Store script demos in `mono.engine/examples/assets/scripts/` and world
  demos in `mono.engine/examples/assets/worlds/`, staged as
  `assets/examples/scripts/` and `assets/examples/worlds/`, with `DemosLoader`
  as the shared interface. Bladeborne is a plain XML `.aworld` with client,
  server, and shared role-separated scripts; Studio, client, server, and
  launcher access the demo tree.
- [x] when im interacting with ui in a viewport, its ONLY for that viewport. server view when running worlds do NOT show ui (only in studio for ui in StarterGui).
- [x] when i play on a client and interact with the ui, its only for that client. startergui ones do NOT show here, they are cloned into PlayerGui.

Extra:
- [x] merge flamegraph visuals into the Physics and Network profilers.
- [_] merge flamegraph visuals into the remaining profilers, and add tabs to swap between `Tabular` and `Flamegraph` views.

### v0.25

- [_] remake the tornado simulation demo using the C++ repository as a base. Check the existing documentation, add missing engine features that we need (ask user questions about it first, you'll need to swap into plan mode), then implement once you get the OK.
- [_] update and prune old content in documentation. check each statement, update, remove or replace.
- [_] go through engine and consolidate and cleanup dead code branches. remove backport compatibilities with previous engine versions and ground this as the version.

### v0.26

- [_] project demos: space engineers asteroids + planets full demo, huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot, ai village with daily routines and such
- [_] create another demo of a ai npc village where they have daily tasks and things like that (dwarf fortress style - personality, occupation, etc).
- [_] pathfinding
- [_] more advanced pathfinding where you can specify wall climbing and stuff, like a "can climb" zone or stuff lik that for ai too

- [_] gtlf default character (unreal style)
- [_] /docs/future-work/character-system.md
- [_] /docs/future-work/world-streaming.md
- [_] /docs/future-work/terrain-system.md
- [_] /docs/future-work/navigation-ai-system.md
- [_] /docs/future-work/physics-expansion.md
- [_] /docs/future-work/vfx-system.md
- [_] /docs/future-work/camera-and-cinematics.md
- [_] /docs/future-work/ui-system.md
- [_] /docs/future-work/input-system.md
- [_] /docs/future-work/prefab-package-system.md
- [_] /docs/future-work/procedural-generation.md
- [_] /docs/future-work/session-and-social.md
- [_] /docs/future-work/audio-system.md

QOL:
- [_] ```const char *CameraModeName(scene::CameraMode mode) {
	switch (mode) {
	case scene::CameraMode::Classic:
		return "classic";
	case scene::CameraMode::LockFirstPerson:
		return "lock_first_person";
	case scene::CameraMode::ShiftLock:
		return "shift_lock";
	case scene::CameraMode::Scriptable:
		return "scriptable";
	}
	return "unknown";
}``` move all character management code to a "PlayerModule" script called CameraController.
Same with movement system, needs to be server authoritive but pure-lua so it can be changed.
When you create a new world/scene, it auto appends the scripts in.

### FUTURE

- [_] go through docs/future-work/REVISIT_IDEAS.md for things we can do sooner.
- [_] maybe consider converting a bunch of custom tools to plugins and have them built-in to studio, or make a plugin pack as a extra release file you can import to a plugins/ folder in ~/Documents/atomic-game-engine/studio/plugins
- [_] (procedural, node-based) terrain generator (refer to discord references) - editablemesh, greedymesh, noise layers, node graph with previews, chunk-based, etc. Add voxel mode (which separates cardinal facing direction Fnt/Bk/Lft/Rgt/Top/Bott faces into groups - only renders the two groups it can see). Expand with surfacecameras, portals, etc, so it culls, occulusion culls, etc.
- [_] unity porting tools / unity shop
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
- [_] built-in whiteboxing tools (planning) for building (plugin)
- [_] full procedural terrain studio tools
- [_] full ui feature buildout + custom
- [_] ui creation tool, full aspect ratio scaling, select how it scales, how panels scale, etc. easier version of tooling than manually building them out
- [_] html-based ui creation (html-script?) => auto handles aspect constraints and whatnot as well, css as well. "virtual container" that makes/simulates the instances?
- [_] figma import tools
- [_] import blender files in asset explorer natively (drag .blend files on engine)
- [_] rpg maker port tool
- [_] photoshop file reader and import tool
- [_] docs/MOBILE.md implementation
- [_] concept idea: setup a public mcp repository in python, add .mcp.json in project folder that loads it, it watches forums channels in the discord server for new/existing bugs. agent writes a message in the channel stating you're fixing it, other agents work on other bugs. agents can write that "this bug is a big rewrite" in the channel too which could be helpful. as a custom plugin? maybe just consider as a separate project.
- [_] localization support
- [_] could we try some minecraft shaders / pbr texture packs as test items? maybe upload to my cdn and then load it and ill check if it works
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms. add a container component flag to enable it. add a [experiment] marker.
- [_] add model providers (e.g. npcs in a game and can chat with you)
- [_] VR support (oculus rift s)
- [_] setup a studio permissions system for: microphone, camera, etc
- [_] setup a example plugin for mocap with camera point track
- [_] breakpoint history list per-script (show each iteration of breakpoint, can see change overtime)
- [_] expand breakpoint system to also include profilers like the heap allocation and timed flamegraph, you can see bottlenecks per iteration then (e.g. we can setup a "total compute", "total memory alloc", "total memory release", etc)
- [_] expose automation tools like mouse clicks and keyboard inputs to luau scripts (so we can create ai that plays for you)
- [_] expose a AutomationService that does this for you (need to enable it for it to be useable).
- [_] ECS driven RL agent environments
- [_] use a spatial walk (octree) to find hallways and such and use that baked information for things like the LOD, unrendering objects, etc. full node based logic for customisation.

### Open Decision

1. Move "roblox files to atomic game files" to a external program? The port tool.
