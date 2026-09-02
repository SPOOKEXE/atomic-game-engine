
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

### v0.22

- [x] make animations scriptable with a world-owned `AnimationBuffer` that `Animation` references while `AnimationTrack` keeps its one reference to `Animation`. Luau buffers and JavaScript ArrayBuffers can procedurally build keyframes, bake to the canonical AAN1 format, import or export baked bytes, save and replicate them, and play through a revision-cached render path. The animation demo and both script runtimes cover the full path.
- [x] build out ArcHandles, BoxHandleAdornments and related handle and selection adornments
- [x] make the abstract GUI hierarchy virtual and non-creatable while keeping each class as metadata over its inherited component set
- [x] ensure `:IsA()` walks the virtual class hierarchy
- [x] ensure value objects work
- [x] build out the UI items and input paths, including drag detectors
- [x] ensure welds, weld constraints and legacy joints work
- [x] ensure `ViewportFrame`, isolated viewport worlds and `WorldRoot` work
- [x] host each plugin dock as a real ECS `DockWidgetPluginGui` tree with cached layout, ImGui painting, input routing and lifecycle cleanup
- [x] expose live Studio automation through MCP with screenshots, emulated mouse clicks, keyboard keys and text input, plus command-palette discovery and execution by stable command id
- [x] allow Studio simulation and rendering to run uncapped without display pacing, and verify the StressParticles bottleneck in live uncapped release Studio
- [x] fix selection box not being aligned to object
in StressParticles demo:
- [x] optimise `graph.cull-bound`: the axis-aligned bound path reduced the 1,000-object release benchmark from 17.38 us to 7.90 us; uncapped release Studio measured 0.009 ms mean and 0.013 ms p99.
- [x] account for the section between `ViewRecording::Begin` and `execute graph`: it is node-table construction, now reported as `build node table`; uncapped release Studio measured 0.051 ms mean and 0.080 ms p99.
- [x] remove unnecessary render preparation work: static draw lists are reused, no-rig scenes skip skin palettes, and StressParticles measured 0.010 ms mean render preparation with 0.002 ms mean collection in uncapped release Studio.
- [x] send only independently changed object rows, indices, skin offsets, joint words and occlusion data to the GPU; the warm StressParticles Studio run reused its draw list on all 4,005 captured frames and `upload-instances` rounded to 0.000 ms mean.
- [x] stop object updates in StressParticles: the moving host Parts caused them, so the demo now animates child Attachments while host Parts remain static and only particle state changes.

- [x] optimise `build-node-table` in `Renderer::RendererView` 0.365ms in StressParticles demo.
- [x] see if we can optimise `transparent pass` in `transarent` in `execute graph` in `Renderer::RendererView`

- [_] do a 10, 100, 250, 500 and 1000 world stress test and list all the bottleneck locations. create a table of the top-10 items. write to docs/world-stress-test.md. use flamegraph and heap to help. Use Rings demo to test.
- [_] optimise the top-10 world stress test.

### v0.23

- [_] /docs/future-work/character-system.md
- [_] gtlf default character (unreal style)
- [_] security audit, fuzzy tests, bound tests, etc.
- [_] node graph editor built-in library for canvas + nodes + async compute + etc? can create a new instance for it called NodeCanvas or such that is a ui object. zooming, moving around, resize nodes, etc. think of comfyui. setup output typed ids so filtered node connections, add callback functions to process as well, etc.

### v0.24

- [_] find a way to (easily) and thoroughly test rendering steps and ensure they produce the right image with right projections
- [_] finish portals so lighting, physics, projection, clipping and geometry crossing the seam are seamless, build an actual demo that agent can see that properly visualises this
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

### v0.25

- [_] /docs/future-work/world-streaming.md
- [_] /docs/future-work/terrain-system.md
- [_] /docs/future-work/navigation-ai-system.md
- [_] /docs/future-work/physics-expansion.md
- [_] /docs/future-work/vfx-system.md
- [_] /docs/future-work/camera-and-cinematics.md
- [_] /docs/future-work/ui-system.md
- [_] /docs/future-work/input-system.md
- [_] /docs/future-work/prefab-package-system.md
- [_] /docs/future-work/materials-and-shaders.md
- [_] /docs/future-work/procedural-generation.md
- [_] /docs/future-work/session-and-social.md
- [_] /docs/future-work/audio-system.md

### FUTURE

- [_] maybe consider converting a bunch of custom tools to plugins and have them built-in to studio, or make a plugin pack as a extra release file you can import to a plugins/ folder in ~/Documents/atomic-game-engine/studio/plugins

- [_] review additions from v0.20 and refine further, was rewriting alot so its experimental
- [_] (procedural, node-based) terrain generator (refer to discord references) - editablemesh, greedymesh, noise layers, node graph with previews, chunk-based, etc. Add voxel mode (which separates cardinal facing direction Fnt/Bk/Lft/Rgt/Top/Bott faces into groups - only renders the two groups it can see). Expand with surfacecameras, portals, etc, so it culls, occulusion culls, etc.
- [_] unity porting tools / unity shop
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
- [_] built-in whiteboxing tools (planning) for building (plugin)
- [_] full procedural terrain studio tools
- [_] full ui feature buildout + custom
- [_] level-of-details (4 different meshes version, auto-decimate version, smart-triangle-reduction-version thinking of nanite triangle surface area) - LOD selection is a per-instance visual decision and belongs in the GPU-resident set beside the occlusion cull that already runs there, so a level change costs no CPU round trip.
- [_] project demos: space engineers asteroids + planets full demo, blackhole simulator (warp space, warp visual, etc), huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot, user interface (copy bladeborne's for demo?), ai village with daily routines and such
- [_] create another demo of a ai npc village where they have daily tasks and things like that (dwarf fortress style - personality, occupation, etc).
- [_] port TornadoSim as a demo scene in the engine.
- [_] ui creation tool, full aspect ratio scaling, select how it scales, how panels scale, etc. easier version of tooling than manually building them out
- [_] html-based ui creation (html-script?) => auto handles aspect constraints and whatnot as well
- [_] figma import tools
- [_] import blender files in asset explorer natively (drag .blend files on engine)
- [_] rpg maker port tool
- [_] photoshop file reader and import tool
- [_] docs/MOBILE.md implementation
- [_] concept idea: setup a public mcp repository in python, add .mcp.json in project folder that loads it, it watches forums channels in the discord server for new/existing bugs. agent writes a message in the channel stating you're fixing it, other agents work on other bugs. agents can write that "this bug is a big rewrite" in the channel too which could be helpful. as a custom plugin? maybe just consider as a separate project.
- [_] localization support
- [_] could we try some minecraft shaders / pbr texture packs as test items? maybe upload to my cdn and then load it and ill check if it works
- [_] atomic engine icons
- [_] studio icons
- [_] pathfinding
- [_] more advanced pathfinding where you can specify wall climbing and stuff, like a "can climb" zone or stuff lik that for ai too
- [_] go through docs/future-work/REVISIT_IDEAS.md for things we can do sooner.
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms. add a container component flag to enable it. add a [experiment] marker.
- [_] add model providers (e.g. npcs in a game and can chat with you)
- [_] VR support (oculus rift s)
- [_] setup a studio permissions system for: microphone, camera, etc
- [_] setup a example plugin for mocap with camera point track
- [_] breakpoint history list per-script (show each iteration of breakpoint, can see change overtime)
- [_] expand breakpoint system to also include profilers like the heap allocation and timed flamegraph, you can see bottlenecks per iteration then (e.g. we can setup a "total compute", "total memory alloc", "total memory release", etc)
- [_] expose automation tools like mouse clicks and keyboard inputs to luau scripts (so we can create ai that plays for you)
- [_] expose a AutomationService that does this for you (need to enable it for it to be useable).

### Open Decision

1. Move "roblox files to atomic game files" to a external program? The port tool.
