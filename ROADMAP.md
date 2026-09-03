
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

### v0.23

- [x] for `just schema-dump`:
1. it toml file dumps
a. schema for components and classes
b. components
c. classes
2. saves under `docs/`
3. replace current ones (justfile). check git diff.
- [x] add typechecking to script editor (shows unused variables, bad expressions, etc)
- [x] when scripts crash, ensure studio does not crash and it handles it (just disable the script) - linux works fine but on windows it crashes the entire thing
- [x] when you edit a script (right click edit script or double click), focus on the script editor viewport and open if not open
- [x] the + button to add new instances just collapses the explorer instead of opening the instance list with search
- [x] when i stop the game, it closes any open scripts in script editor. should stay open and it should keep all viewports in their current open/closed state (e.g. it keeps swapping back to Live Instances instead of keeping explorer selected)
- [x] instead of a standalone `script editor` menu, it should be each script has their own editor with the name as the script
- [x] add multi-line comment highlighting and support and such in script editor
- [x] in the heap profiler, add the ability to click columns to sort by that.

- [x] add a script ticking rate slider for script update hertz
- [x] add CanQuery to PVInstance/BasePart, etc. Same with CastShadow.
- [x] in World => Create World From Demo, move to under Demo dropdown and separate by simple/advanced categories.
- [x] in the flamegraph, add the ability to click on a bar to focus only on that bar and subitems, then LEFT clicking in empty space returns to root, and RIGHT clicking goes UP A PARENT for the bar (so if we're inspecting scene culling and right click, it goes up a parent to the renderer bar or whatever). This will show the parent bar with its stuff underneath.

- [x] check all luau library bindings are async-compute (start operation, poll operation, end operation, a underlying c++ operation manager that uses parallel/async job management and such). Ensure we add to flamegraph / profilers as well.
- [x] create a new flamegraph called `Scripting` for luau script bindings (abstract away though for when we add JavaScript and later maybe C# bindings to it).
* allows us to see per-binding flamegraph and active usage
* setup benchmark tests for each C++ to script binding so we can find bottlenecking ones and optimise them
* does not show script stuff (which fits in `Script Profiler` below), specifically targets the bindings
- [x] `Script Profiler`;
* shows a flamegraph of all the scripts and scripts that requrie other scripts and operations they call.
* Breaks apart by function name (or anonymous with line number), any library calls, etc.
* can view a hierarchy view or per-script view with search and filters and sorting columns (bytes allocated, per-function-milliseconds-compute, yielding sections of code, etc.
* Also add a "folds" view for each script, where you can click on a script to open a `Script Folds Profiler` for it where it shows you per-fold computation (i.e. for loops, functions, event callbacks, anonymous functions, what line, how much time spent, yielding libraries, etc)
* Separate per-scripting-bindings into sub-bars as well so if one specific script binding has yielded the luau loop, we can obviously see that.

- [_] optimise remaining physics bottlenecks using StressPhysics demo
- [_] ensure chunked physics world implementation is correct and optimised (world is split into chunks, all physics computations happen in the chunks on separate threads in parallel, all computations are also batch computed, chunk borders - edges and corners - are computed separately or handled post center).

- [_] node graph editor built-in library for canvas + nodes + async compute + etc? can create a new gui object instances for it called NodeCanvas or such that is a ui object. zooming, moving around, resize nodes, etc. think of comfyui. setup output typed ids so filtered node connections, add callback functions to process as well, etc.

new demos:
- [_] port TornadoSim as a demo scene in the engine.
- [_] blackhole simulator (warp space, warp visual, etc)
- [_] user interface (copy bladeborne's for demo?), luau scripting and such
- [_] update DEMOS.md with GIFs uploaded to repository
- [_] quadsphere, quadtree planet

- [_] atomic engine icons?
- [_] add icon pack to studio?
- [_] security audit, fuzzy tests, bound tests, etc.

### v0.24

- [_] /docs/future-work/character-system.md
- [_] gtlf default character (unreal style)

### v0.25

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
- [_] level-of-details (4 different meshes version, auto-decimate version, smart-triangle-reduction-version thinking of nanite triangle surface area, nanite style) - LOD selection is a per-instance visual decision and belongs in the GPU-resident set beside the occlusion cull that already runs there, so a level change costs no CPU round trip.

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
- [_] /docs/future-work/materials-and-shaders.md
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

### Open Decision

1. Move "roblox files to atomic game files" to a external program? The port tool.
