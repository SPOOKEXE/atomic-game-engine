
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

Full render plan: [docs/RENDER-REFACTOR.md](docs/RENDER-REFACTOR.md), including
the consolidated materials, shaders and rendering optimization work.

- [x] Add opt-in render-stage image snapshots, raw pixels, metadata and a visual index. GPU overwrite checks pass; capture stalls affect timing.
- [x] Fix topology renewal after cache expiry and consume ready topology replies before camera routing.
- [x] Add portal startup readiness and Humanoid camera routing fixes. All 16 product crossing variants pass: 30/60 Hz, first/third person, explicit/automatic subject, held/released movement.
- [x] Capture a missing-eye-image black frame at its first render stage; retain useful images and remove bulk captures.
- [x] Reduce editable collision BVH build work and scratch storage. Full Terrain worker profiling remains below.

Continue on `docs/RENDER-REFACTOR-TASKS.md` in feature-first order. Complete
stabilization work after the feature milestones so isolated bugs do not block
the renderer build-out.

#### 1. Easy

- [x] Add per-mesh, global-lighting and camera render capability fields to GPU-resident rows so compute passes can branch without CPU readback. Instance policy occupies the existing 64-byte resident row, world and camera policy occupy the shared view uniform, and authored compute nodes can request both without CPU readback.
- [x] Expose the existing render passes as render-pipeline nodes. Every native render pass has a graph node and backend handler; the remaining work below extends that graph with new features.
- [x] Add selectable antialiasing choices as render-pipeline nodes. FXAA, TAA and all three SMAA stages have graph node kinds, default shaders and multi-target authored-raster support. A Vulkan graph fixture runs every choice against a hard diagonal, verifies softened output pixels, and verifies TAA's paired history output.
- [x] Add four authored mesh LOD levels with GPU-side per-instance selection beside occlusion culling. `CustomMeshLOD` overrides `AutoMeshLOD` per level, while nil custom slots fall back to the matching automatic artifact. Each resolved level has its own resident instance row and indirect draw command; a per-view compute pass selects one from projected bounds without returning the result to the CPU. The Vulkan fixture proves near views draw the detailed quad and distant views draw the coarse triangle while preserving the part's authored bounds.
- [x] Allow visual items to attach compute and post-processing shader nodes, resident only while their pipeline is active. Scene records, serialization, BasePart script properties and policy-aware demand checks select lazy graph shaders. Inactive post and compute nodes pass their input through; a Vulkan fixture proves independent activation, output changes, and target retirement when the pipeline is removed.
- [x] Add a demo pipeline that exercises capability toggles, antialiasing, post-processing and authored LOD selection. `RenderFeatures.pipeline` extends the production PBR graph with lazy visual compute and post nodes plus FXAA; `RenderFeaturesDemo.luau` authors world, camera and instance policies, then demonstrates `CustomMeshLOD` overriding and falling back per level to `AutoMeshLOD`. The example fixture checks the staged document against its C++ recipe and executes the scene through Luau.

#### 2. Medium

- [x] Define the visual-compositor graph contract using the Unity Scriptable Render Pipeline and Visual Compositor as references. The contract now fixes typed ports, scopes, queues, resource versions, capabilities, fallbacks, lifetime, history and authoring-only metadata: https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html and https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html.
- [x] Move residency and delta upload into nodes, then remove each replaced legacy rendering path. `mesh-residency` and `delta-upload` now record on the shared frame command buffer, and the pre-graph mesh flush has been removed.
- [x] Add the product-side active-scene collector and parallel presentation walk, then batch every active camera across worlds. Complete owned packets are copied on stable world lanes, ordinary display worlds are not reopened, and one renderer submission includes offscreen active cameras plus the display camera.
- [x] Add GPU-side sRGB handling, emissivity, mipmapping and bounding-box-first occlusion culling. Texture formats and sampling preserve sRGB intent, emissive data reaches deferred lighting, mip chains are resident inputs, and HZB culling follows the frustum and bounding-box passes.
- [x] Add proper PBR with tests, dynamic ambient occlusion and render-only displacement maps that do not alter physical transforms. The PBR graph, SSAO path and visual displacement fields are covered by graph, scene and render fixtures.
- [x] Add Fog, Clouds and Skybox compute-shader nodes, plus screen-space post-processing nodes. Environment compute stages and post nodes are catalogued, serialized, scheduled and exercised by the render graph suites.
- [x] Add EditableMesh and EditableImage packing and quantization policies for float16, E4M3 float8, signed and unsigned integer16, integer8, integer4 and boolean formats. Mesh presentation applies supported policies without changing authored or collision data. EditableImage keeps canonical RGBA8 and reports compact GPU storage as unsupported until TextureTable gains matching sampled formats.
- [_] Measure many 4k textures on the GPU and test a GPU atlas system before selecting packing defaults. The optimized headless probe compares four standalone 4096-square RGBA8 textures with one 8192-square atlas and reports CPU recording, GPU timestamps, residency, allocations, uploads, transfers and cache hits. Run `just gpu-texture-atlas-bench 1` for the device result.
- [x] Add automatic mesh decimation as the second LOD generation mode. The bake graph now publishes deterministic coarse meshes per material run, preserves safe skinning boundaries and supplies actual triangle counts to runtime LOD selection.
- [_] Profile release CPU and GPU work, residency, caching and transfer bytes after the medium feature set is integrated. The atlas probe and existing frame metrics expose the required counters; final integrated release measurements remain after the device run.
- [x] Finish Terrain editable collision worker profiling and optimization as a separate performance task. Dirty refresh mutates the ECS-owned shape resource only after workers join and keeps unrelated BVHs resident; the release bench measured 0.885 ms for a 4,225-point terrain chunk beside 2,000 resident shapes.

#### 3. Hard

- [_] Render foreign worlds from fresh destination geometry and capture data using the current camera, with correct parallax and disocclusion. The packet current-camera Vulkan test passes 155 assertions and in-flight request coalescing closes camera loss.
- [_] Finish retained-world observation with authorized content, complete visual layers, handoff lifetime and gameplay lease retirement. The staging prototype is rolled back.
- [_] Add tessellation and global illumination as composable render nodes.
- [_] Add smart triangle reduction based on projected triangle surface area, then investigate Nanite-style virtualized geometry without CPU readback during LOD changes.
- [_] Port semi-real ray tracing and path tracing into render nodes.
- [_] Add demo render pipelines for semi-real ray tracing and path tracing.
- [_] Finish the visual-compositor system and build out additional Blender-like pipeline nodes and workflows.
- [_] Reproduce and fix the original black frame with a valid image handle, and retain the last valid image during topology waits.
- [_] Verify seamless player and body crossing, Humanoid camera subjects, camera obstruction, clipping and return trips under delay, restart and lost acknowledgements.
- [_] Verify portal lighting, shadows, transparency, particles, ribbons, spatial UI and animated character accessories through the seam.
- [_] Check oblique, rolled and scaled portal views at all angles, then finish visual review of the non-Euclidean demo.

[MCP-ADDITIONS.md](MCP-ADDITIONS.md) describes proposed data-factory requirements; these are design targets, not verified implemented APIs:
- [_] accept text instructions with reference images, controls, video motion constraints and externally interpreted engine-validated patches.
- [_] add a dedicated OBB geometry test.
- [_] add the remaining MCP tools and demo coverage for script packages, multicamera, multiworld, segmentation, optical flow, lighting contribution, rigs, audio export and interop.
- [_] align text, image, video and audio structured records with controls, grounding points, boxes, masks, crops and marks.
- [_] batch scenes on GPU headless or offscreen, with explicit capability and readiness reporting.
- [_] capture IDs, semantic masks and part masks.
- [_] complete autonomous capture workflow in `DataFactoryDemo.luau`.
- [_] declare interop subsets for glTF, USD, COCO, YOLO, GeoJSON and WKT, including sidecars and known losses.
- [_] define MCP idempotency, expected versions, structured status, cancellation, capability limits, permissions and audit records.
- [_] define the remaining camera intrinsics, extrinsics, near/far, jitter, lens distortion, crop, units and world/camera coordinates.
- [_] describe render-graph passes and resources with budgets and dependencies, without inventing ground truth.
- [_] emit structured event narratives with time, knowledge, belief and provenance fields.
- [_] export rigs, skeletons, keypoints, skinning data and animation tracks.
- [_] expose PBR albedo, roughness, metallic, emissive, specular, transmission, shading geometry, normals and UV maps.
- [_] expose physics contacts, impulses, forces, torque, sleep, assemblies, joints, controller fields and units.
- [_] finish broad multimodal and prediction tools in the Python factory.
- [_] finish deterministic action and script sequencing at fixed-tick boundaries; rational timing and manual tick boundaries are checked.
- [_] finish thin MCP adapters for every service.
- [_] generate counterfactual pairs, parameter sweeps, domain randomization and holdouts without label leakage.
- [_] implement backward seek as checkpoint plus replay, never negative dt, with bounded history.
- [_] label lights, shadows, per-light caster and receiver contribution, and ambient-occlusion estimator provenance.
- [_] load repository script packages with source and asset hashes, seeded parameters, type checking, sandboxing and atomic scene edits.
- [_] maintain an acceptance fixture suite for replay roundtrip, no-time-advance, image-label alignment, retry isolation and invalid data.
- [_] make step plus snapshot plus multicamera capture atomic, with asynchronous readback completion.
- [_] profile release captures for actual bytes, allocations, peak memory, timings and output quality.
- [_] provide occupancy, SDF, BEV, navmesh and affordance queries with authored semantics.
- [_] record optical flow, motion vectors, trajectories, scene cuts and validity flags.
- [_] record visibility, occlusion, disocclusion and visible or amodal masks.
- [_] restore checkpoints only when compatible, and create fresh versions after restore.
- [_] support forks and versioned causal edits, including effects outside the edited spatial region while keeping branches isolated.
- [_] support forward scene-to-modalities and inverse observation-to-scene patches, with rerendered numeric and semantic metrics plus ambiguity masks.
- [_] provide full checkpoint coverage for ECS, physics warm start, RNG, script schedulers, events, clocks, string IDs and pinned assets; the API requires a real host rehydrator.
- [_] support reflections from SSR, probes, mirrors and portals, including secondary views, recursion and staleness.
- [_] support render-only steps with zero simulation advance and an explicit temporal-history policy.
- [_] synchronize audio waveforms with source events and timing.
- [_] track source evidence IDs, deduplicate facts, mark stale or missing evidence, and define repair and external-factory ownership.
- [_] write durable artifact manifests, schemas, checksums and chunks with retention, atomic finalization, crash resume and bounded backpressure.
- [x] add EditableImage:ToBuffer() and EditableImage:FromBuffer(buffer) (RGBA) to luau and engine.
- [x] add the data-capture graph's default PBR data-capture node.
- [x] capture RGB linear HDR, depth and packed normals.
- [x] define exact projection metadata.
- [x] define rational timing metadata and deterministic manual tick boundaries.
- [x] expose capability, version and schema discovery, and report unsupported features explicitly.
- [x] expose thin MCP pause, resume, step, snapshot, checkpoint and restore commands.
- [x] keep snapshots immutable with stable `DataFactoryId` identities and copied clocks.
- [x] keep the draw collector's current transform derived with no clock advance and unchanged serialized snapshot bytes.
- [x] keep typed HDR buffers separate from RGBA8 buffers.
- [x] make `DataFactoryDemo.luau` read and print metadata, round-trip a buffer and include a capture request example.
- [x] make asynchronous HDR, depth and packed-normal capture retain its ticket, ranged bytes and exact projection metadata.
- [x] provide durable Python factory chunks, sample finalization, pins, recovery, provenance, sweeps, holdouts and metrics.
- [x] provide real headless raycast and AABB spatial queries.
- [x] provide the Python sibling API's negotiation, thin reads, lifecycle and ranged, BLAKE3-verified resource reads.
- [x] provide the real headless OBB query.
- [x] share engine services through Luau DataSceneService, with VM-neutral ECS metadata, queued lifecycle work and Luau/JavaScript render bridges.
- [x] support all-system pause for one local client with `--data-factory`, including the SDL device barrier.
- [x] support physics-only pause, including clock and character gates.
- [x] validate RGBA8 buffers as exactly width*height*4, including orientation, color space, alpha and copy semantics.

Rendering extra fixes:
- [x] blackhole warp curves inward consistently across spin phases.
- [x] character collision and wall sliding work against objects.

TODO tweaks:
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

Verification: focused Vulkan lens, full-world, Studio and live Studio checks pass for the covered rendering, editor, profiler and pause work. The broader pipeline, source and generated-doc checks were outside this verification.

Extra:
- [_] update and prune old content in documentation. check each statement, update, remove or replace.

- [x] Store script demos in `mono.engine/examples/assets/scripts/` and world
  demos in `mono.engine/examples/assets/worlds/`, staged as
  `assets/examples/scripts/` and `assets/examples/worlds/`, with `DemosLoader`
  as the shared interface. Bladeborne is a plain XML `.aworld` with client,
  server, and shared role-separated scripts; Studio, client, server, and
  launcher access the demo tree.

### v0.25

- [_] /docs/future-work/character-system.md
- [_] gtlf default character (unreal style)
- [x] merge flamegraph visuals into the Physics and Network profilers.
- [_] merge flamegraph visuals into the remaining profilers, and add tabs to swap between `Tabular` and `Flamegraph` views.

### v0.26

- [_] project demos: space engineers asteroids + planets full demo, huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot, ai village with daily routines and such
- [_] create another demo of a ai npc village where they have daily tasks and things like that (dwarf fortress style - personality, occupation, etc).
- [_] pathfinding
- [_] more advanced pathfinding where you can specify wall climbing and stuff, like a "can climb" zone or stuff lik that for ai too

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
