
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
- [_] Render foreign worlds from the current camera with correct parallax and disocclusion. The moving-camera whole-eye test still fails with flat images.
- [_] Finish retained-world observation: authorized content, complete visual layers, handoff lifetime and gameplay lease retirement. The staging prototype is rolled back.
- [_] Reproduce and fix the original black frame with a valid image handle; prevent missing-image black frames during topology waits.
- [_] Verify seamless player/body crossing, Humanoid camera subjects, camera obstruction, clipping and return trips under delay, restart and lost acknowledgements.
- [_] Verify portal lighting, shadows, transparency, particles, ribbons, spatial UI and animated character accessories through the seam.
- [_] Check oblique, rolled and scaled portal views at all angles; finish visual review of the non-Euclidean demo.
- [_] Profile release CPU/GPU work, residency, caching and transfer bytes; finish Terrain editable collision worker optimization.

Portal evidence and next steps: [render task list](docs/RENDER-REFACTOR-TASKS.md).
Passing crossing tests do not yet establish seamless rendering at every angle.

- [_] ensure per-mesh render capabilities, global lighting render capabilities, camera lighting render capabilities, etc. compute shaders, post-processing, etc. - per-mesh capability flags are per-instance visual state and belong in the GPU-resident row, so a compute pass can branch on them without a CPU readback
- [_] simplify and strip old rendering code that is not part of the node system. Everything should be in the node system. - the residency and delta upload are a node too, so the sweep and the GPU-resident work are the same refactor rather than two passes over the same files
- [_] port semi-real raytrace and path-trace as part of nodes
- [_] make demo render pipelines with semi-real raytrace and path-trace
- [_] add compute shaders / postprocessing shaders to all visual items as a additional node to attach (render pipeline pulls and residents shaders on gpu when active)
- [_] (dynamic) ambient occulusion, emissivity, mipmapping, occulusion culling (bbox first, extra after), sRGB handle, proper PBR with tests, tesselation, add Fog/Clouds/Skybox compute shader support, screen-space, global illumination, displacement maps (make it rendering only but not physical) - "rendering only but not physical" is exactly the transform/visual split the GPU-resident set draws, so all of this is GPU-side state with no CPU mirror to keep in step
- [_] more blender-like render pipeline ideas and build-out
- [_] render pipeline nodes for above
- [_] plan the entire rendering system to a visual compositor system like Unity. https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html
- [_] ensure full parallel/vectorised (i.e. get all active scenes => build entity list => update gpu resident => batch render all cameras in every scene) - stable entity slots, per-world particle pools and batched camera submission are built. The remaining work is the product-side active-scene collector and parallel presentation walk; every camera can already read its world's buffers without re-uploading them.
- [_] better memory packing for editablemeshes and editabletextures. also add quantization support for editablemesh and editabletexture as a component that rounds values and such (e.g. (u)float16, (u)float8, (u)int16, (u)int8, (u)int4, bool) test many 4k textures on gpu and packing. test an atlas system on gpu too.
- [_] different antialiasing choices as render nodes
- [_] level-of-details (4 different meshes version, auto-decimate version, smart-triangle-reduction-version thinking of nanite triangle surface area, nanite style) - LOD selection is a per-instance visual decision and belongs in the GPU-resident set beside the occlusion cull that already runs there, so a level change costs no CPU round trip.

[MCP-ADDITIONS.md](MCP-ADDITIONS.md) describes proposed data-factory requirements; these are design targets, not verified implemented APIs.

- [_] add EditableImage:ToBuffer() and EditableImage:FromBuffer(buffer) (RGBA) to luau and engine.
- [_] share engine services through Luau DataSceneService, with thin MCP adapters at the boundary.
- [_] expose capability, version and schema discovery, and report unsupported features explicitly.
- [_] validate RGBA8 buffers as exactly width*height*4, including orientation, color space, alpha, copy semantics and separate typed HDR buffers.
- [_] load repository script packages with source and asset hashes, seeded parameters, type checking, sandboxing and atomic scene edits.
- [_] support all-system pause separately from physics-only pause.
- [_] define exact fixed-tick actions with rational timing and deterministic tick boundaries.
- [_] support render-only steps with zero simulation advance and an explicit temporal-history policy.
- [_] checkpoint ECS, physics, RNG, script schedulers, events, clocks, string IDs and pinned asset dependencies, rejecting unsupported state.
- [_] restore checkpoints only when compatible, and create fresh versions after restore.
- [_] implement backward seek as checkpoint plus replay, never negative dt, with bounded history.
- [_] support forks and versioned causal edits, including effects outside the edited spatial region while keeping branches isolated.
- [_] keep snapshots immutable with stable string identities and captured clocks.
- [_] make step, snapshot and multicamera capture atomic, with asynchronous readback completion.
- [_] batch scenes on GPU headless or offscreen, with explicit capability and readiness reporting.
- [_] capture RGB linear HDR, depth, normals, IDs, semantic masks and part masks.
- [_] define camera intrinsics, extrinsics, projection conventions, near/far, jitter, lens distortion, crop, units and world/camera coordinates.
- [_] record visibility, occlusion, disocclusion and visible or amodal masks.
- [_] record optical flow, motion vectors, trajectories, scene cuts and validity flags.
- [_] expose PBR albedo, roughness, metallic, emissive, specular, transmission, shading geometry, normals and UV maps.
- [_] label lights, shadows, per-light caster and receiver contribution, and ambient-occlusion estimator provenance.
- [_] support reflections from SSR, probes, mirrors and portals, including secondary views, recursion and staleness.
- [_] describe render-graph passes and resources with budgets and dependencies, without inventing ground truth.
- [_] expose physics contacts, impulses, forces, torque, sleep, assemblies, joints, controller fields and units.
- [_] provide spatial queries for raycasts, AABB, OBB, occupancy, SDF, BEV, navmesh and affordances with authored semantics.
- [_] export rigs, skeletons, keypoints, skinning data and animation tracks.
- [_] synchronize audio waveforms with source events and timing.
- [_] align text, image, video and audio structured records with controls, grounding points, boxes, masks, crops and marks.
- [_] accept text instructions with reference images, controls, video motion constraints and externally interpreted engine-validated patches.
- [_] support forward scene-to-modalities and inverse observation-to-scene patches, with rerendered numeric and semantic metrics plus ambiguity masks.
- [_] generate counterfactual pairs, parameter sweeps, domain randomization and holdouts without label leakage.
- [_] emit structured event narratives with time, knowledge, belief and provenance fields.
- [_] track source evidence IDs, deduplicate facts, mark stale or missing evidence, and define repair and external-factory ownership.
- [_] write durable artifact manifests, schemas, checksums and chunks with retention, atomic finalization, crash resume and bounded backpressure.
- [_] declare interop subsets for glTF, USD, COCO, YOLO, GeoJSON and WKT, including sidecars and known losses.
- [_] define MCP idempotency, expected versions, structured status, cancellation, capability limits, permissions and audit records.
- [_] maintain an acceptance fixture suite for replay roundtrip, no-time-advance, image-label alignment, retry isolation and invalid data.
- [_] profile release captures for actual bytes, allocations, peak memory, timings and output quality.
- [_] Add MCP tools for the ones that need them, then make a demo scene called DataFactoryDemo.luau which gets these values in scripts (and prints some metadata about them) and whatnot. This way we can see that it works and our mcp can use them (and i can tell other agent sessions to refer to the demo to see how they work).

Rendering extra fixes:
- [_] blackhole warp is opposite on one side to what it should be (quaternions can help do the curvature if needed).
- [_] character does not collide with objects

TODO tweaks:
- [_] batch compute the selection box rendering
- [_] fix selection box / left click drag / left click drag select, buggy
- [_] fix unable to drag in node canvases
- [_] left-click to select also drags them immediately, give a deadzone period before attempt dragging
- [x] add column sorting to asset profiler
- [_] add a timing selector and dropdown to select Average/Max/Min checkbox like Frame Graph to the Heap Profiler (average across N milliseconds)
- [_] swap average checkbox to a dropdown to select Average/Max/Min checkbox
- [x] View > Datastores, rename to View > DataStore Editor
- [x] View > Datastore, rename to View > DataStore Config
- [x] View > CDN, rename to View > CDN Config
- [_] rename View > Physics Solver to View > Physics Profiler, move under View > Pipeline Profiler, and remake it based on what Pipeline Profile contains.
- [_] change how particle:Emit works where we mark the particle as wanting to emit via a flag, then, do a batch emit (hook this into the Enabled as well). Big luau bottleneck (or maybe even a StoredEmitValue value would be nicer?).
- [_] Network profiler; in studio, says 715 requests in flight but no traffic is happening. Rebuild based on Physics Profiler and Pipeline Profiler. Flamegraph as well.

### v0.25

- [_] /docs/future-work/character-system.md
- [_] gtlf default character (unreal style)
- [_] merge flamegraph visuals into profilers, and add tabs to swap between `Tabular` and `Flamegraph` views.

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
