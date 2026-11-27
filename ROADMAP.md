
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

## Rules

Each version remains active until its outstanding validation and optimization gates are (fully) complete or with user's permission to defer only when it cannot be completed by means.
1. Must plan the generalisation of these systems so they're adaptable for any purpose (i.e. why we're making a lot of things graphs and have luau APIs, so they're accessible).
2. Prune (document) upon completion.
3. Three rounds of optimisations to improve clock time, latency, memory, compute, etc.
4. Plan first, present to user, user says any changes, tweak, repeat until user happy, then implement once told (usually a goal will be set). Include key factors in your documents and whatnot so we know what we're doing.

## VERSIONS

### v0.26.2

STUDIO:
- [_] Start a server `Script` added at runtime. Only the client role runs new scripts (`Runtime::RunNewScripts`), so a server script cloned or parented in while the game runs never starts; `StressScripts.luau` uses closures for this reason. Maybe just don't replicate server scripts altogether.
- [_] Add team test (run server on team create host machine, everyone can join team test, when host stops server all disconnect / cancel connecting).
- [_] Add a team create chat if not already done
- [_] Test 16 person team test and improve where needed
- [_] Build out the script editor and polish the workflow around it
- [_] Add preference options for external editors for the game - temporary two way sync in filesystem?
- [_] Remake all the demos, prune non-required ones, make each demo meaningful and display many features at once for automated tests.
- [_] BUG: portal does not render lights that travel through (same and maybe other) portals. The portal light demo has it.
- [_] Bring native cloth simulation to the engine from private plugin. Create lattices and a interface surrounding groups of lattices, adding ways to combine the edge of a lattice to another, edit properties like rigidity, elasticity, anchor points for cloth (i.e. they dont move from this location/attachment, they're stuck there), and a way to attach it to a mesh edge/vertex/etc so it follows it, etc, far easier.
- [_] Bring native soft-bodies to the engine from private plugin. Create meshes that act as soft bodies, creasing layers, soft layers that give way to objects, deforms the mesh, has physics colliders, maybe a "true position" and "deform position" preview modes where it shows a adornee of the mesh in its true location without soft-body deforming and then adornee of its soft-body deformed location.
- [_] Make new demos and move current for cloth simulation and the native soft-bodies from plugin.
- [_] Add an engine-level way to get the location of a vertex in a mesh in real-time (only track selected ones when it exists). Need a way to check if the mesh is still alive as well. Maybe something like MeshTracker (with settings to disable options like surface normal direction + color) -> TrackedVertex/TrackedTriangle -> State + Position + Surfance Normal (direction) + Color, etc. Also accounts for pbr items.
- [_] Build the raycast library up using what the plugin lacks (and what luau code ended up needing) as new luau library (functions). Both standalone math and other class items.
- [_] Build up the SurfacePoint in the engine as well, could fall under MeshTracker or another service.
- [_] Build up the Triangle library in the engine as well as part of this (closestBarycentric, closest). Both standalone math and MeshTracker/other class items.
- [_] Also build out splines and curves as data types.
- [_] Also build out inverse-kinematic and forward-kinematic library in luau as c++ bindings (math.invkin and math.fwdkin or whatever names)
- [_] Also build out ways to build bone graphs with IK/FK support on them (so they can be animated but also coded to work). Maybe a IKCustomBoneController and IKHierarchyBoneController with related functions to attach bones, define hierarchy (its .Parent and :GetChildren() to determine hierarchy via RootBone property, or custom by calling functions and defining the hierarchy that way, that is why its split into two so we can just use a different component to store data and use one computation location to handle both types), etc. Maybe worth exposing a way to have the engine automatically do whatever interpolation using ticking, or allow the user to tick themselves via code, and ways to pull the interpolation data of bones in both scenarios like :GetBoneInterpolations() which gives last frame + current frame interpolation values.
- [_] Native PNG for scripting and engine (buffer in for pixels, png bytes out) or (png bytes in, buffer out for pixels), To and From PNG/Buffer/Array(2D)/flat array with width/height params
- [_] Native Gltf for scripting and engine, same behavior above but whatever gltf needs
- [_] Build geometry nodes into the engine as well from the plugin. Basically points along the mesh that track where it is when it deforms, and can be used to help animate and whatnot by cloth simulation / soft-body manipulation via scripting or engine.
- [_] Completely fix NodeGraph UI element and functionality in full (Nodes + Ports + Connecting ports + Dragging around + Insertion + Viewing + Supported Node Visualisations (progress bars, state text, coloring the outline), etc). Really polish and ensure its functioning properly with all interactions.

IMAGECOMPOSER:
- [_] PBR textures support subgraphs
- [_] Add small graph icon in textbox area on far right in SurfaceAppearance/Textures/Decals
- [_] Support rendering the static AND dynamic images
- [_] Support loading textures into the graph
- [_] Support calling other (saved) graphs into this graph (check for recursive)

SPATIAL/RAYCAST:
- [_] Revisit and consolidate the spatial querying system. It may need some upgrades and consolidation. Do this before raycasting (but keeping it in mind, as well as bbox-based searching). Also consider ECS-based algorithms around it for performance (i.e. gpu? vectorised? contiguous memory.).
- [_] Revisit and consolidate the raycasting system. It may need some upgrades and consolidation as its split and unperformant (needs better spatial querying algorithms). Also consider ECS-based algorithms around it for performance (i.e. gpu? vectorised? contiguous memory.).

CHARACTER:
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
}``` move all camera and character management code to a "PlayerModule" script called CameraController / ControlModule.
Same with movement system, needs to be server authoritive but pure-lua so it can be changed.
When you create a new world/scene, it auto appends the scripts in.
- [_] character spawning & preset outfits built into engine
- [_] proper character & camera controller luau modules (instead of C++ unreachable code)
- [_] proper character movement & AutoRotate & StepHeight
- [_] character intent item in document
- [_] Define player-independent characters, player roster and possession ownership,
  and neutral driver leases for players, AI, scripts, and replay. See the
  [character system plan](docs/system-plans/character-system.md) and the
  [navigation and AI action seam](docs/system-plans/navigation-ai-system.md).
- [_] Stress demos to build once the character and constraint work above lands: a crowd of a few hundred animated, skinned characters (skin palettes, animation tracks, humanoid physics) and long joint and weld chains against the 4,096 joint cap, which still faults on count.
- [_] animation graphs & event
- [_] default animation controllers and graphs
- [_] G08 (animation graphs, masks, synchronise, and event markers)
- [_] C02 (Simulation and animation levels of detail)
- [_] Define character state-machine policy, animation graph composition and
  blending, and how animation follows committed character state. See the
  [character system plan](docs/system-plans/character-system.md).

### v0.26.3

PHYSICS:
- [_] P02 (contact policies & complete physical events)
- [_] P03 (complete fast-body collision and batched world queries)
- [_] R08 (real geometry ray queries and reference path tracing)
- [_] Physics expansion (docs/system-plans/physics-expansion.md)
- [_] World / scene management (docs/system-plans/world-streaming.md)

WEATHER (docs/system-plans/weather-system.md):
- [_] Native tornado field source (analytical vortex publishing wind, pressure and condensation) so TornadoSim combines `CloudCompute`, `Volume`, `GpuParticleField` and `PhysicalParticleField` instead of its scripted `StormField`.
- [_] Size-aware hail melt in `mono.engine/weather/src/Consumers.cpp`; hail now melts out over warm columns.
- [_] Precipitation channel in the GPU field (`weather-field.glsl`) so particle consumers stop depending on the CPU sampler.
- [_] Storm dynamics: sounding, multi-layer wind profile, cloud deformation by shear and updrafts, storm cells, cold pools, fronts, rain evaporative cooling, hail growth and transport, water and energy accounting.
- [_] Engine dims ambient light from cloud cover automatically (today only scripts do it).
- [_] Run weather on Metal and run the NEON evaluation branch.

PORTALS AND REPLICATION:
- [_] Replication performance (docs/system-plans/replication.md): pose lane with per-client slots and baseline deltas, per-subtree interest invalidation, per-client publish cost.
- [_] Seamless portals (docs/system-plans/portals.md): fenced handoff, exactly-once input, and passing the product walk visual continuity and acceptance tests.
- [_] Fix high-FPS portal-crossing failures in `mono.client/tests/PortalWalk.cpp` (one all-black frame at 144 FPS, `adoptedClearSamples` gate failures); next step is producer and consumer readbacks at the failing pose.
- [_] Ribbons and billboards across a portal seam, using the path `CollectParticleBatches` uses for particles.

RENDER (docs/render-hooks.toml, planned cleanup):
- [_] Split `ViewRecording` into `ViewInput`, `PreparedView` and `RecordingState`.
- [_] One bounded surface frontier for mirrors and portals, then delete the recursive recorders.
- [_] Separate each main rendering system into subgraphs, then hook those subgraphs as a `composite` node then connect them together in the main rendering graph. This way users can modify each subsystem.

ENGINE:
- [_] localization support
- [_] Plan a consolidation of services and functions to better-named locations (i.e. resolve what mesh-related functions go where - MeshTracker, MeshPart entities/components, PBR, etc)
- [_] Maybe move all the mathematical helper functions (i.e. standalone computations) under one math library like math.raycast.nearest, math.triangles.closestTo, etc. Also make/use particular data types for those functions as well in a expected format, like defining Triangle/Quad/Mesh, using  and expecting a certain layout with a optional validate function (IsBufferCompatible) etc.

### v0.26.4

RENDERING:
- [_] R3 (material graphs, instances and permutation control)
- [_] R4 (projected decals and mesh-particle effect authoring)
- [_] C03 (Reusable graph assets, versioning, and live replacement)
- [_] more imagegraph implementation work (2D images, 3D mesh projection - textures/editable-image)
- [_] idea: for the LOD system, could we make support for the imagegraph with rescaled and resampled images (like normal, roughness, color, etc) maps as part of the LOD? this way far away we simplify textures used and reduce memory pressure (stream lower quality from cpu to gpu in update polls with prediction).

DEMO:
- [_] Was thinking of making a "foilage" and "forest" test world based on `[Chlorophyll Engine] 10M Trees - 15km² Test` video.
- [_] Was thinking for the `Screen Space Shadows and Micro Shadows test in the forest demo` and level-of-detail shadow system and adding it to the forest demo and engine as a whole too.
- [_] Also `Custom Engine Forest Showcase [uR5EvBWxN14].webm` is another great demo.
- [_] Was thinking how we can make the render graph more convenient and conventional. Need ways to mark recursivity in renders (e.g. portals, mirrors, etc), and showing them in graph and making them editable - i.e. modular.

### v0.26.5

- [_] R5 (HLOD, imposters & foliage authoring)
- [_] add two render graph nodes for: `path tracing` and `ray tracing` as built-in extras with mixed scales (upsampling? dlss support? amd one? etc.) and add presets to use them.

### v0.26.6

- [_] breakpoint history list per-script (show each iteration of breakpoint, can see change overtime)
- [_] expand breakpoint system to also include profilers like the heap allocation and timed flamegraph, you can see bottlenecks per iteration then (e.g. we can setup a "total compute", "total memory alloc", "total memory release", etc)
- [_] Input actions, normalized devices, and a bounded `VirtualDeviceService`
  for custom controllers and external ingress. See the [input system
  plan](docs/system-plans/input-system.md) and [virtual device service
  plan](docs/system-plans/virtual-device-service.md).
- [_] expose automation tools (AutomationService) like mouse clicks and keyboard inputs to luau scripts (so we can create ai that plays for you)

### v0.26.7

- [_] W04 (Parties, matchmaking, and cross-server travel)
- [_] U01 (Project-owned gameplay actions and device profiles)
- [_] U02 (Audio effects, spatial filtering, and acoustic zones)
- [_] U03 (Streaming media, adaptive music, and voice transport)
- [_] U04 (Camera rigs, sequences, and cinematic authoring)

### v0.26.8

- [_] gtlf default character (unreal style)
- [_] docs/system-plans/terrain-system.md
- [_] Player-independent characters, controller state, animation graphs, and
  player roster/possession, plus calibrated body/hand tracking mapped to rig
  bones, in the [character system plan](docs/system-plans/character-system.md).
- [_] docs/system-plans/vfx-system.md

### FUTURE

- [_] Renderer residency program: virtual geometry, virtual texturing, virtual shadow maps and GPU-driven submission, measured as one.
- [_] Frame-budget policy: temporal rendering, dynamic resolution, variable-rate shading and automatic quality allocation.
- [_] Frame capture and replay format for renderer diagnosis.
- [_] Shared signed-distance-field service, only once two or more subsystems have a measured need.
- [_] Engine-wide performance-budget regression suite based on real release scenes.
- [_] Studio time-travel inspection and subsystem freezing, after recording exposes a stable snapshot contract.

AI/NAV:
- [_] Navigation and AI: bounded navigation, perception, named inspectable AI
  states, behaviour plans, and driver-safe character actions in the
  [navigation and AI plan](docs/system-plans/navigation-ai-system.md).
- [_] pathfinding
- [_] more advanced pathfinding where you can specify wall climbing and stuff, like a "can climb" zone or stuff lik that for ai too

DEMOS:
- [_] project demos:
* space engineers asteroids + planets full demo (`docs/FULL-PLANET-DEMO.md`)
* huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot
* ai village with daily tasks, occupations, relationships, and things like that (dwarf fortress style - personality, occupation, etc).
* floating islands with village houses on them with bridges connecting them together, floating above clouds, minecraft-like
- [_] (procedural, node-based) terrain generator (refer to discord references) - editablemesh, greedymesh, noise layers, node graph with previews, chunk-based, etc. Add voxel mode (which separates cardinal facing direction Fnt/Bk/Lft/Rgt/Top/Bott faces into groups - only renders the two groups it can see). Expand with surfacecameras, portals, etc, so it culls, occulusion culls, etc.
- [_] could we try some minecraft shaders / pbr texture packs as test items? maybe upload to my cdn and then load it and ill check if it works

PLANS:
- [_] docs/system-plans/camera-and-cinematics.md
- [_] docs/system-plans/prefab-package-system.md
- [_] docs/system-plans/procedural-generation.md
- [_] docs/system-plans/session-and-social.md
- [_] docs/system-plans/audio-system.md
- [_] full audio DAW (digital audio workbench) system (nodes) - NEEDS A PLAN (if audio-system.md doesnt have it)
- [_] docs/MOBILE.md implementation

EXTERNAL-TOOLS:
- [_] unity porting tools / unity shop
- [_] figma import tools
- [_] import blender files in asset explorer natively (drag .blend files on engine)
- [_] rpg maker port tool
- [_] photoshop file reader and import tool
- [_] RL agent environments (character controlling + camera view) - setup MCP bindings

ENGINE+TOOLS:
- [_] full ui feature buildout + custom
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] built-in whiteboxing tools (planning) for building (plugin)
- [_] ui creation tool, full aspect ratio scaling, select how it scales, how panels scale, etc. easier version of tooling than manually building them out
- [_] html-based ui creation (html-script?) => auto handles aspect constraints and whatnot as well, css as well. "virtual container" that makes/simulates the instances?
- [_] full procedural terrain studio tools
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms. add a container component flag to enable it. add a [experiment] marker.
- [_] VR support, with Rift S as a requested validation target: OpenXR stereo
  views, in-game per-eye image-clarity calibration, and calibrated glasses-style
  magnification. Capability-based controllers, arbitrary tracked sources, body
  and hand calibration, and character bone/IK mapping share the input and
  character paths. Add Luau-defined virtual controllers and reusable tracking
  modules, with optional computer vision adapters feeding the same samples.
  Hardware support depends on provider integration; physical
  optical focus depends on headset support. Expand the
  Far Cry VR and Space Marine VR references from Zero Latency Adelaide when
  POVs arrive. Check OpenXR profiles for Vive wands, Rift S Oculus Touch, Valve
  Index, and the simple-controller fallback; targets remain unverified and
  runtime-dependent. Check Cosmos and Focus separately. See the [VR system
  plan](docs/system-plans/vr-system.md), [input system
  plan](docs/system-plans/input-system.md), and [virtual device service
  plan](docs/system-plans/virtual-device-service.md).
- [_] mobile & console builds
- [_] web renderer?
- [_] godot's render graphs
- [_] 2D & PSEUDO & ORTHOG & 3D view graph modes (also allow for orthogonal views/projections)
- [_] more built-in useful libraries
- [_] build toolbar and plugin system out
- [_] improved custom version of lumen based on what it does? (real-time global illumination)
- [_] setup a studio permissions system for: microphone, camera, etc
- [_] studio plugin for mocap with camera point track (use camera permissions)
- [_] improve physics to real-time (look at arxiv papers?)
- [_] terrain diffusion as a noise generation option

MISC:
- [_] concept idea: setup a public mcp repository in python, add .mcp.json in project folder that loads it, it watches forums channels in the discord server for new/existing bugs. agent writes a message in the channel stating you're fixing it, other agents work on other bugs. agents can write that "this bug is a big rewrite" in the channel too which could be helpful. as a custom plugin? maybe just consider as a separate project.
