
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

#### 3. Hard

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

`datafactories-docs/MCP-ADDITIONS.md` describes proposed data-factory requirements; these are design targets, not verified implemented APIs:
- [_] accept text instructions with reference images, controls, video motion constraints and externally interpreted engine-validated patches.
- [x] add a dedicated OBB geometry test. The core suite checks rotated half extents and containment of every transformed corner.
- [_] add the remaining MCP tools and demo coverage for script packages, multicamera, multiworld, segmentation, optical flow, lighting contribution, rigs, audio export and interop.
- [x] align text, image, video and audio structured records with controls, grounding points, boxes, masks, crops and marks. The Python factory validates immutable text spans, points, semantic and part masks, control IDs, frame times and audio sample rates against media bounds.
- [_] batch scenes on GPU headless or offscreen, with explicit capability and readiness reporting.
- [_] capture IDs, semantic masks and part masks.
- [_] complete autonomous capture workflow in `DataFactoryDemo.luau`.
- [x] declare interop subsets for glTF, USD, COCO, YOLO, GeoJSON and WKT. The named profiles now define exact represented fields, bundle sidecars, coordinate rules, stable-ID mappings, machine-readable loss classes and atomic import refusal conditions.
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
- [x] synchronize audio waveforms with source events and timing. The audio mixer retains a fixed-capacity, allocation-free applied-command and natural-finish trace; immutable observations copy post-clip float32 samples, exact sample clocks, stable scene source names, spatial state, attenuation provenance and explicit unsupported occlusion state.
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
