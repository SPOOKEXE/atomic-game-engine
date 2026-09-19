
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
- [x] make the Studio physics profiler retain and display completed physics work. It keeps the last solver sample across presentation-only frames, includes reported worker spans with their scheduler ancestors, and the headless `Slide.luau` regression passes 9 assertions across live capture and retention.
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
- [x] declare interop subsets for glTF, USD, COCO, YOLO, GeoJSON and WKT. The named profiles define represented fields, bundle sidecars, coordinate rules, stable-ID mappings, machine-readable loss classes and atomic import refusal conditions.
- [x] expose ambient-occlusion estimator provenance. The live starter exports source-only R8/unorm8 SSAO at native half resolution with raw bytes, PNG, bounded statistics and `ssao-provenance/v1`. The record carries `estimated`, `cleared_disabled`, `cleared_no_pass` or `unavailable` source state, resolved enabled bool when known, producer frame, 12 shader-shared samples, radius 0.65, no denoiser, no temporal history, background value 1 and explicit unavailable background classification. Cached captures preserve their producer frame; custom R8 remains unavailable. Values are screen-space visibility factors only. Broader light, shadow and ambient-occlusion truth remain open.
- [x] generate counterfactual pairs, parameter sweeps, domain randomization and holdouts without label leakage. The Python factory builds immutable seeded plans, paired one-axis variants, typed choice and uniform sweeps, connected group-aware holdouts and leakage checks across scenes, pairs, assets, sources and provenance.
- [x] export rigs, skeletons, keypoints, skinning data and animation tracks. The client MCP returns bounded `data-rig/v1` skeleton and bone frames with stable IDs, exact tick rationals, deterministic ordering, authored semantic keypoints, decoded bounded buffered animation channels and positive mesh skin influences encoded as uint16 unorm weights. `RigKeypoint` scene instances, Luau authoring, portal transfer, demo coverage and exact v1 validation work. Meshes above the 1,024-vertex export bound report skinning unavailable instead of returning a misleading prefix, while rendering retains the complete mesh.
- [x] expose physics contacts, impulses, forces, torque, sleep, assemblies, joints, controller fields and units. `physics-observation/v1` exports bounded identified contacts, event phases, completed-solver impulses with world-space basis, sleep, rigid assembly identity, authored joints, humanoid and input controller fields, units and explicit availability. Dynamic rigid bodies now own persistent world-space `AppliedForce` in N and `AppliedTorque` in N*m; each is applied once per completed fixed physics substep using mass and world inverse inertia, wakes a loaded sleeper, and is exposed through Luau getters and setters, capability flags, and bounded stable-ID data-scene rows with source, units, world coordinate space, timing and coverage. These are authored persistent loads, not gravity, contact resultants, or fabricated total-force ground truth.
- [x] align text, image, video and audio structured records with controls, grounding points, boxes, masks, crops and marks. The Python factory validates immutable text spans, points, semantic and part masks, control IDs, frame times and audio sample rates against media bounds.
- [x] capture object IDs as exact `R32_UINT` opaque or masked gbuffer pixels with stable string sidecars and explicit zero background or unidentified values. Transparent surfaces, particles and later composited layers are excluded.
- [x] describe render-graph passes and resources with budgets and dependencies, without inventing ground truth. Typed Python `get_render_graph` and the starter workflow now live-verify the world-scoped `Default PBR#0` graph and write 36 nodes, 37 resources, 21 dependency waves and logical budget estimates. Timing, physical overlap, driver residency and historical capture identity remain unavailable.
- [x] emit structured event narratives with time, knowledge, belief and provenance fields.
- [x] synchronize audio waveforms with source events and timing. The audio mixer retains a fixed-capacity, allocation-free applied-command and natural-finish trace; immutable observations copy post-clip float32 samples, exact sample clocks, stable scene source names, spatial state, attenuation provenance and explicit unsupported occlusion state. Client data-factory mode exposes bounded `audio_observation/v1` MCP records and immutable SHA-256 waveform ranges from deterministic full-tick null-device captures. The Python client validates records and reconstructs complete waveforms.
- [x] track source evidence IDs, deduplicate facts, mark stale or missing evidence, and define repair and external-factory ownership. The evidence ledger enforces stable source and fact identities, legal freshness transitions, monotonic observation times, atomic bounded updates and repair-result lineage across engine and external owners.
- [x] merge flamegraph visuals into the Physics and Network profilers.
- [x] expose capability, version and schema discovery, and report unsupported features explicitly.
- [x] provide the Python sibling API's negotiation, thin reads, lifecycle and ranged, BLAKE3-verified resource reads.
- [x] publish one supported engine MCP endpoint, startup flow and server manifest for external programs and MCP agents. `client --data-factory --mcp-port 8736`, the checked `.mcp.json` entry, generic stdio `mcpbridge`, `RUNNING.md` and the served `data-factory` agent prompt expose the same versioned tools, limits and structured unsupported results.
- [x] expose create, select, reset and retire operations for isolated data-factory worlds in client, server and Studio hosts. Client and headless server MCP can create one caller-named local world in an empty host, stage create or reset through a scratch Universe, install product systems before commit, return the new zero-tick world all-systems paused, retain bounded idempotency tombstones and retire owned worlds. Compatibility worlds cannot be claimed or retired. The server skips presentation while all systems are paused, advertises only its headless tool subset, requires a working MCP listener and honors empty-host run bounds. Studio now hosts the isolated lifecycle with create, select, reset, retire, pause, resume and step, and requires a working MCP listener.
- [x] extend DataSceneOptions with named cameras, compact storage and injected noise. Named capture cameras resolve a unique camera `DataFactoryId`, rebuild portal, surface-camera and spatial-GUI packets from that camera, isolate exact prepared tickets on failure, and restore normal presentation cache state on the next frame. Synchronized scene sidecars and typed Python identity and lifecycle validation are implemented. Engine and Python capture paths support `lossless` and `training_compact`, with measured float16 depth conversion and exact native ID or mask storage. The external Python factory can apply bounded seeded Gaussian noise to copied linear RGB observations, preserves alpha and source bytes, and records the exact NumPy generator version plus measured error. The engine applies deterministic Gaussian noise only to copied RGB linear HDR capture bytes after storage transforms. It records the requested sigma, Q24 round-to-nearest-even effective sigma, algorithm version, zero-seed state policy, alpha and clamp policy, and measured error. Label, depth, normal and sidecar data remain exact.
- [x] load repository and inline script packages with source and asset hashes, seeded parameters, type checking, sandboxing and atomic scene edits. Client, headless server and Studio MCP hosts accept the same bounded `atomic.data-script.v1` Luau package, verify BLAKE3 source and asset hashes plus typed manifest parameters, run it in a one-shot capability-limited scratch world, and swap only after a terminal result. Descriptor-safe repository loading lives in the Python client. Static Luau admission loads generated engine declarations, narrows them to the package sandbox, forces strict checking and refuses bounded syntax or type failures before live-world serialization. The server installs only server systems and performs no presentation. Studio refuses swaps during Run or Play, retires Store borrowers before replacement, then releases presentation and reloads plugins against the committed world. `DataFactoryPackageDemo.luau` is a rerunnable fixture that replaces only its owned workspace subtree.
- [x] add a typed, non-parented `DataSceneOptions` value created by `DataSceneService:CreateOptions()` and accepted by `CaptureBundle(snapshotId, options)`. It carries the versioned channel set, camera, pipeline, view slot, storage profile, derived-mask choices, coordinate spaces and bounded noise settings. Submission validates and freezes one copied request so later script edits cannot change a pinned capture. The tested Luau and Python slice supports `current_view` or a unique named camera, `lossless` or `training_compact` raw planes, world/camera/image coordinates, and bounded deterministic RGB noise. `IncludeSceneData` requests a bounded synchronized `data-capture-scene-sidecar/v1` record retained with the ticket; the typed Python client validates its request intent, schema, snapshot, tick, epoch, version and nested `data-scene/v1` record. `IncludeExactMasks` remains explicitly unsupported. Snapshot identity remains explicit for Luau capture.
- [x] expose the tested DataSceneOptions base slice through nested MCP `capture_bundle` options. It validates the `data-scene-options/v1` schema, bounded strings, duplicate channels, paired second-surface planes and bounded noise, preserves exact tick, epoch, version, storage profile and operation guards, and reuses capture polling, cancellation, release, teardown and retained-resource lifecycle. `IncludeSceneData: true` returns the retained scene sidecar only on ready or partial polls, while false returns null. Named camera and noise requests use the same capability and lifecycle gates as direct capture and script calls. Exact masks remain open below.
- [x] accept text instructions with reference images, structured controls and video motion constraints, then apply only externally interpreted, engine-validated scene patches. The Python boundary copies and bounds every instruction, artifact, control tree, evidence ID and motion interval. An external interpreter returns a typed candidate grounded in that evidence; only validated `CausalEdit` values cross MCP through `apply_intervention` at an exact snapshot, tick, epoch and version.
- [x] expose thin MCP `pause`, `resume`, `step`, `snapshot`, `checkpoint` and `restore` commands.
- [x] share engine services through Luau `DataSceneService`, with VM-neutral ECS metadata, queued lifecycle work and Luau or JavaScript render bridges.
- [x] define rational timing metadata and deterministic manual tick boundaries.
- [x] keep snapshots immutable with stable `DataFactoryId` identities and copied clocks.
- [x] keep the draw collector's current transform derived with no clock advance and unchanged serialized snapshot bytes.
- [x] support all-system pause for one local client with `--data-factory`, including the SDL device barrier. Factory create and reset establish this barrier atomically before returning their tick-zero revision.
- [x] support physics-only pause, including clock and character gates.
- [x] support render-only steps with zero simulation advance and an explicit temporal-history policy. The paused-world session validates retained snapshots and revisions, the client submits one forced frame with zero particle or simulation delta, and Luau, MCP and Python expose bounded submit and poll records. Preserve is supported now; reset and disable return explicit unsupported states until renderer-local history control exists.
- [x] add a dedicated OBB geometry test. The core suite checks rotated half extents and containment of every transformed corner.
- [x] add position and rotation values in world, derived parent-relative and stable reference-object space. The Python factory has immutable point, vector, quaternion and rigid-pose records with explicit world IDs, rigid reference validation, canonical JSON and tested bidirectional conversions.
- [x] define camera intrinsics, both extrinsic transform directions, near and far planes, requested resolution, crop, units and coordinate conventions. Scene snapshots include derived world and parent-relative transforms plus bounds-derived sizes; bounded camera observations project identified OBBs with explicit frustum and unavailable occlusion evidence. Exact render projection, lens distortion and temporal jitter remain unavailable until a capture resolves them.
- [x] define exact projection metadata for completed captures.
- [x] provide a small `DataFactoryDemo.luau` that creates one camera and three identified objects, reads the scene snapshot and camera object observations, and prints the stable IDs, poses, sizes, visibility and projected bounds.
- [x] provide real headless raycast, AABB and OBB spatial queries through Luau, JavaScript, typed engine calls, thin MCP tools and the Python client. The starter Python workflow records the exact query inputs, snapshot, units, availability and results beside its captured images.
- [x] retain the broader API walkthrough in `DataFactoryAdvancedDemo.luau`, including metadata reads, image-buffer round trip and capture request setup.
- [x] add optional packed HDR or motion encodings and uint16 depth. The Python writer keeps native storage as the default and preserves source bytes. Opt-in uint16 depth records scale, offset, valid range, 65535 invalid sentinel and measured reconstruction error, and binds second-surface depth to its exact validity plane. Opt-in little-endian float16 storage accepts HDR and declared motion or position planes with exact component shapes, rejects finite overflow, preserves nonfinite classes and records source checksums plus measured absolute reconstruction error. Live motion-vector capture remains a separate observation task.
- [x] add `EditableImage:ToBuffer()` and `EditableImage:FromBuffer(buffer)` RGBA8 support to Luau and the engine.
- [x] add explicit `lossless` and `training_compact` artifact storage profiles for the synchronized bundle. Lossless storage preserves each native plane and applies chunk compression without changing values. The engine compact profile converts float32 first and second-surface depth to little-endian IEEE binary16 with exhaustive round-to-nearest-even coverage, preserves NaN and infinity classes, rejects finite overflow, strips source row padding and keeps complete source descriptors. Integer IDs, masks and all other native encodings remain exact. Luau, MCP and the typed Python client retain and validate the profile on pending, terminal and ready replies, enforce coherent nullable error metadata and reject descriptor drift. Explicit writer options also provide uint16 depth and little-endian float16 HDR, motion and position storage without changing the default profile. Every stored plane declares its retained and source encoding, byte order, row stride, color space, units, validity source and checksum, and lossy forms include numeric error against finite native samples.
- [x] add the data-capture graph's default PBR data-capture node.
- [x] allow the default capture node to copy source, depth and normal without requiring optional ambient-response and lighting-baseline planes. A Vulkan regression records all three declared planes and rejects an unpaired lighting-response declaration.
- [x] capture RGB linear HDR, depth and packed normals.
- [x] emit v4 content-addressed artifacts with deterministic independent 1 MiB zlib or identity chunks, per-chunk and whole-payload checksums, strict bounded decoding and exact lossless or compact-byte reconstruction. The checked live artifact remains uncompressed v3 until the next approved live capture. Bounded thread and cross-process writer locks plus checksum-proven prior-manifest cleanup protect concurrent reruns and user files. Spawned-process fixtures prove exclusive lock handoff, automatic release after owner exit and complete committed capture manifests from competing writers.
- [x] keep typed HDR buffers separate from RGBA8 buffers.
- [x] make one-camera asynchronous capture retain one aligned ticket with ranged bytes and exact projection metadata for RGB linear HDR, linear depth, packed shading normals, PBR albedo, packed PBR material, PBR emissive, source-only ambient occlusion, object IDs, semantic IDs and part IDs. Integer label captures include bounded dense sidecars from integer labels to stable strings; background and unidentified opaque or masked gbuffer pixels use zero. The ambient-occlusion plane is R8/unorm8 at native half resolution and carries raw bytes, PNG and bounded statistics plus `ssao-provenance/v1`: `estimated`, `cleared_disabled`, `cleared_no_pass` or `unavailable` source state, resolved enabled bool when known, producer frame, 12 shader-shared samples, radius 0.65, no denoiser, no temporal history, background value 1 and explicit unavailable background classification. Cached captures preserve their producer frame; custom R8 remains unavailable. It is not ground truth, shadow visibility, material occlusion or global ambient occlusion. Transparent surfaces, particles and later composited layers do not write the label planes, so final-composite visibility truth remains open.
- [x] validate RGBA8 buffers as exactly `width*height*4`, including orientation, color space, alpha and copy semantics.

The data-factory contract lives in `datafactories-docs/MCP-ADDITIONS.md`. The engine owns worlds, time, edits and observations; the external factory owns recipes, storage, training and evaluation. Engine MCP and typed Python agree on bounded results and refusals.

Progress: client, server and Studio expose isolated-world lifecycle and versioned discovery; typed Python and independent MCP callers agree on twelve capture planes, and named multicamera capture shares one renderer frame. The final rebuilt release Vulkan default headless run completed a 12-plane capture and retired its world. The default headless data-factory now paces at 60 FPS; at 11 seconds, idle CPU was about 8.7% versus about 100% uncapped, with a longer capped sample at about 3.3%. The broad render check passed all 105/105 cases and 1,690,379 assertions, clearing the earlier one-case failure. The Python sibling full suite passed 604 tests.

- [x] Export a bounded glTF 2.0 GLB subset with built-in, EditableMesh and resident delivered geometry, exact-owner source textures, material runs, stable IDs and cameras. Live typed export covered all seven published meshes and sixteen source sheets in a 249,232,616-byte ranged GLB. Decoded PNG pixels matched all sixteen baked sources, with no unavailable rows in that fixture. Source retention is enabled only for data-factory client runs.
- [x] Expose bounded `raw-scene/v1` MCP extraction with portable little-endian mesh sections, original source texture bytes, authored material facts, stable IDs, revision fences and BLAKE3-verified ranged reads. The typed Python reader and a direct MCP caller returned identical bytes from a live headless Vulkan world with an EditableImage map. The first resource remains capped at 320 MiB.
- [x] Convert validated `raw-scene/v1` extracts to a bounded glTF 2.0 GLB subset in sibling `datafactories-docs/raw_scene_gltf.py`. The live `export_raw_scene_gltf_live.py` command creates and retires its owned world, snapshots and fences the raw extract, and emits a verified five-file bundle carrying `engine_version` and `snapshot_id`. The release Vulkan demo produced a 4,576-byte GLB with four nodes, three meshes, one camera and seven explicit losses. Khronos validation after the unused-object fix reported 0 errors, warnings and infos; default loss policy refuses and `--allow-losses` succeeds. Python full suite passed 604 tests after coverage reuse. The writer stages five files in a sibling directory, fsyncs each file and the staging directory, uses Linux `renameat2(RENAME_NOREPLACE)` or a Windows no-replace rename, and refuses safely on unsupported POSIX targets before fsyncing the parent. The concurrent-writer test passed its 16-test suite. A parent-sync failure can leave a published bundle whose durability is unconfirmed. The full external glTF profile remains open.

Remaining service and state work:

- [_] Complete MCP host and observation fences, cancellation and remaining thin service adapters.
- [_] Make step, snapshot and multicamera capture atomic.
- [_] Complete durable autonomous production with retention, atomic finalization, crash resume and backpressure.

Remaining observation and interop work:

- [_] Complete synchronized multimodal observations with explicit frame identity, bounds, unavailable facts and first-surface validity.
- [_] Complete object motion and optical flow with scene cuts and validity; extend beyond current static-surface camera reprojection and temporal pose tracks.
- [_] Complete the full external glTF export profile.

Remaining scale and evaluation work:

- [x] Check ticket-specific release Vulkan GPU copy timing and capture transfer accounting. After the capture-specific forced timestamp fix, a live 12-plane release capture measured 1,788,416 ns (1.788 ms) engine-ticket GPU copy time with global profiling off. The prior baseline reason was `no_capture_gpu_timestamp_slot`. The ticket reports 47,109,120 B host readback reserved capacity and 47,155,200 B device staging reserved capacity; these are reserved capacities, not allocator peaks.
- [x] Measure release capture throughput and compact output quality. The external workflow completed in 1.730641471 s for 47,109,120 source bytes, or 27.220612 MB/s. Compact depth maximum absolute errors were 0.0285186768 m for `linear_depth` and 0.0268325806 m for `second_surface_depth`. The release build and focused render GPU test passed 13 assertions; live output is `.cache/build/release/capture-profile-live-timed-20260920/scene.json`.
- [_] Profile release allocator peaks, GPU residency and broader GPU work. Allocator peaks and GPU residency remain unavailable; process RSS HWM of 290,672 kB is not an allocator peak.

- [_] Keep a narrow, extract-only MCP boundary for external data factories. After scene generation, expose one revision-fenced raw spatial record with stable IDs, transforms, geometry, cameras and explicit unavailable facts through bounded reads. Scene transformation, dataset storage and model training stay external. Maintain only the bare connector API and contract tests in `datafactories-docs`.

### v0.25

- [_] review over v0.24 and consolidate, improve, tweak, etc.
- [_] simplify down RUNNING.md, should be minimal, shows each available `just` job, how to build each, etc. Should not contain lots of descriptive information about how those systems work, just short descriptions and what they are aimed at to do.
- [_] USER WORK: cleanup documents in `docs/`, maybe a `docs/systems` folder would be more suited for things like `RENDER-HOOKS.md`, `DEMOS.md`, `ECS_COMPONENTS.md`, `schema.toml` and `schema-data.toml`.
- [_] Prune `PLAN-procedural-planets.md`, `PORTAL-HANDOFF.md`, `RENDER-POST-HOOK-REFACTOR.md`, `RENDER-REFACTOR-TASKS.md`, `RENDER-REFACTOR.md` and `TORNADOSIM.md`.
- [_] Prune old files in `docs/future-work/` as well. e.g. merge `FUTURE_COMPONENTS.md` components into relevent document files in `docs/future-work/`, then leave the remaining orphaned ones in `FUTURE_COMPONENTS.md`.
- [_] improve `schema.toml` and `schema-data.toml` so its better laid out (schema is the general layout, schema-data is the actual useful information that we would grep and search specific classes, components and functions in). Like Roblox Studio Class API Reference.
- [_] consolidate/improve `CONTRIBUTING.md`, `SECURITY.md`, `THIRD_PARTY_NOTICES.md`, `CODE_ARCH.md`, `CODE_DOCUMENTING.md`, `CODE_FORMAT.md` and `CODE_QUALITY.md`, with small sentences at the start of the file describing what they contain in succinct detail.
- [_] cleanup documentation and layout
- [_] review and cleanup render pipeline (plan first)
- [_] plan a consolidation for the MCP-ADDITIONS.md systems. Super big, needs to ensure its properly implemented and looks good. Maybe even isolating code to a separate folder and then using hooks to ingest (and make those hooks hot loadable and unloadable so we can disable when we don't need to use them).
- [_] update and prune old content in documentation. check each statement, update, remove or replace.
- [_] create a "sky grid" of floating terrain balls with each one having one of 8 custom made shaders, then have the camera fly forward between the seams. this is a benchmark called BenchmarkSkyGrid.luau built-in demo example. We'll also use this as a performance profiler for editablemesh + terrain + etc.
- [_] add gpu resource constraining (freezes all other applications right now)

- [_] add typed, read-only physics observation hooks at declared fixed-tick boundaries. Use stable string discovery names, typed immutable per-hook contexts and bounded non-blocking records. Define whether each record observes pre-solve, completed-solver or post-integration state; retain exact tick, world, units and availability; and expose completed records to data-factory MCP without allowing a hook to mutate physics state.
- [_] add typed, read-only replication observation hooks at declared exchange boundaries. Use stable string discovery names, typed immutable per-hook contexts and bounded non-blocking records. Preserve world, authority, client, baseline, tick and exchange-round identity; expose applied, rejected, repaired and dropped work without crossing a world boundary by pointer; and keep private payloads behind the existing permission boundary.
- [_] optimise server startup time
- [_] optimise and improve tests (particularly server and physics, can we add deterministic hooks so we can immediately wait for an update for a change instead of guessing with timestamps? test.solver, test.replication, etc)

- [_] fix lights passing through portals not working

### v0.26

- [_] remake the tornado simulation demo using the C++ repository as a base. Check the existing documentation, add missing engine features that we need (ask user questions about it first, you'll need to swap into plan mode), then implement once you get the OK.
- [_] go through engine and consolidate and cleanup dead code branches. remove backport compatibilities with previous engine versions and ground this as the version.
- [_] go over render system and consolidate/improve hooks, nodes, graph system and visualiser of graph system

- [_] pack multi-channel supporting render data in other channels, depth = r channel - ambient occulusion = g - anti-alias = b, for example.
- [_] do heavy memory, cpu and gpu benchmarking and profiling and see if we can squash data into multi-channel representations, improve computations and memory usage, trade lower precision for tiny visual changes, etc.

- [_] add typed render hooks at proven boundaries. `DataFactoryHookBind` registers and discovers twelve stable read-only data-capture hooks and the synchronous `view.camera` mutation hook. Data capture remains pinned to exact pipeline, revision, view, snapshot and node identity and returns bounded asynchronous readbacks. `view.camera` accepts owned one-shot camera-frame, camera, and projection patches only for an exact installed pipeline revision, world, snapshot, and view slot; conflicts, stale revisions, malformed values, cancellation, and generation reuse are explicit. The renderer copies only the matching view before capture preparation and never calls external code or exposes a mutable view. The release CPU lifecycle benchmark fell from 41.62 to 20.48 microseconds per 64 lifecycles after dispatch stopped copying the full graph snapshot; validated capture planes now take ownership of readback vectors instead of copying them. GPU and readback release profiling plus a second real consumer remain open. A hook is an observer node, declared node output, or this bounded typed pre-view patch, not a second callback graph beside it.
  1. Inventory the current node output, resource lifetime, GPU submission and asynchronous readback boundaries, then choose one stable post-pass observation point.
  2. Give each internal hook an enum value and a stable string name for discovery, manifests and MCP. Never serialize the enum number.
  3. Give each hook its own typed immutable context. Do not use a generic `any` bag or an inheritance tree. A context states its valid lifetime, thread, resource access and unavailable fields.
  4. Keep observation non-blocking and read-only. GPU hooks append bounded records or schedule bounded asynchronous readback; the client polls completed records later. They never wait for the CPU or call arbitrary external code from the render thread.
  5. Apply render changes through CPU-owned scene or graph state before submission, then upload the normal delta. The bounded `view.camera` patch is the named exception: it is a synchronously consumed value record, not an observation callback.
  6. Make the existing data-capture resource observation the first consumer. Expose supported hook names and limits through capability discovery, then verify snapshot, camera, frame, crop and resource identity remain aligned.
  7. Profile record bytes, allocations, readback latency, dropped records and GPU work in a release capture before adding another hook point.
  8. Design separate typed observation hooks for physics and replication only after the render hook has two real consumers. Reuse the naming, bounded queue and polling rules, while keeping each subsystem's own tick, thread and lifetime contract.

### v0.27

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

- [_] breakpoint history list per-script (show each iteration of breakpoint, can see change overtime)
- [_] expand breakpoint system to also include profilers like the heap allocation and timed flamegraph, you can see bottlenecks per iteration then (e.g. we can setup a "total compute", "total memory alloc", "total memory release", etc)
- [_] expose automation tools (AutomationService) like mouse clicks and keyboard inputs to luau scripts (so we can create ai that plays for you)

- [_] /docs/future-work/physics-expansion.md
- [_] /docs/future-work/ui-system.md
- [_] /docs/future-work/world-streaming.md

### FUTURE

- [_] /docs/future-work/navigation-ai-system.md
- [_] pathfinding
- [_] more advanced pathfinding where you can specify wall climbing and stuff, like a "can climb" zone or stuff lik that for ai too

- [_] gtlf default character (unreal style)
- [_] project demos:
* space engineers asteroids + planets full demo (`docs/FULL-PLANET-DEMO.md`)
* huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot
* ai village with daily tasks, occupations, relationships, and things like that (dwarf fortress style - personality, occupation, etc).

- [_] localization support
- [_] /docs/future-work/terrain-system.md
- [_] /docs/future-work/character-system.md
- [_] /docs/future-work/vfx-system.md
- [_] /docs/future-work/input-system.md
- [_] /docs/future-work/camera-and-cinematics.md
- [_] /docs/future-work/prefab-package-system.md
- [_] /docs/future-work/procedural-generation.md
- [_] /docs/future-work/session-and-social.md
- [_] /docs/future-work/audio-system.md
- [_] go through docs/future-work/REVISIT_IDEAS.md for things we can do sooner.
- [_] maybe consider converting a bunch of custom tools to plugins and have them built-in to studio, or make a plugin pack as a extra release file you can import to a plugins/ folder in ~/Documents/atomic-game-engine/studio/plugins
- [_] (procedural, node-based) terrain generator (refer to discord references) - editablemesh, greedymesh, noise layers, node graph with previews, chunk-based, etc. Add voxel mode (which separates cardinal facing direction Fnt/Bk/Lft/Rgt/Top/Bott faces into groups - only renders the two groups it can see). Expand with surfacecameras, portals, etc, so it culls, occulusion culls, etc.
- [_] full procedural terrain studio tools
- [_] unity porting tools / unity shop
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
- [_] built-in whiteboxing tools (planning) for building (plugin)
- [_] full ui feature buildout + custom
- [_] ui creation tool, full aspect ratio scaling, select how it scales, how panels scale, etc. easier version of tooling than manually building them out
- [_] html-based ui creation (html-script?) => auto handles aspect constraints and whatnot as well, css as well. "virtual container" that makes/simulates the instances?
- [_] figma import tools
- [_] import blender files in asset explorer natively (drag .blend files on engine)
- [_] rpg maker port tool
- [_] photoshop file reader and import tool
- [_] docs/MOBILE.md implementation
- [_] concept idea: setup a public mcp repository in python, add .mcp.json in project folder that loads it, it watches forums channels in the discord server for new/existing bugs. agent writes a message in the channel stating you're fixing it, other agents work on other bugs. agents can write that "this bug is a big rewrite" in the channel too which could be helpful. as a custom plugin? maybe just consider as a separate project.
- [_] could we try some minecraft shaders / pbr texture packs as test items? maybe upload to my cdn and then load it and ill check if it works
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms. add a container component flag to enable it. add a [experiment] marker.
- [_] VR support (oculus rift s)
- [_] setup a studio permissions system for: microphone, camera, etc
- [_] setup a example plugin for mocap with camera point track
- [_] ECS driven RL agent environments
- [_] Move "roblox files to atomic game files" to a external program - the port tool?
