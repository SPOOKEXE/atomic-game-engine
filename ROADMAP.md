
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

### v0.26.1

- [_] Revisit the physics system, particularly around the PhysicsStress demo with the tens/hundreds of thousands of parts. Even with ECS that lags considerably which is an issue. We should be using spatial querying to reduce the search amount to only nearby entities (which should be blazing fast and can be parallel). Check if we can do a search then apply step so we can parallel/vectorise before the linear apply (and also apply as much in parallel when they are far away from each other enough like using a frontier loop (hold a frontier list, add neighbours that havent been scanned, run physics, set frontier to neighbors) system)
- [_] Check if we can convert a bunch of the .Each queries into .EachParallel for ECS items (ones that can be async like getters or individual updates that DO NOT rely on accessing and modifying other components).

DOCS:
- [_] Prune and combine/consolidate old docs/future-work/*.md documents that we do not require anymore (or can convert to simple one-two-sentence roadmap items). E.g. weather should be one document under `/docs/system-plans/` of which match our current implementation (separated components for generalisation and composition combination allows for the target weather systems).
- [_] `render-hook.toml` under `docs/` instead of `RENDER-HOOKS.md` and `RENDER-POST-HOOK-REFACTOR.md` - holds what each one is and where. Apply similar to other documents.
- [_] set generated path `ENGINE_STRESS_TEST.md` and `ECS_COMPONENTS.md` under `docs/engine/`
- [_] I moved to `docs/engine/`: `INPUT.md`.
- [_] Cleanup `RELEASING.md`, `THIRD_PARTY_NOTICES.md` (and patches made), `CODE_ARCH.md` (missing items mainly)
- [_] Regenerate schema.toml and schema-data.toml
- [_] Regenerate `ENGINE_STRESS_TEST.md` and `ECS_COMPONENTS.md`
- [_] Make `RENDER-HOOKS.md` generatable via `just` job.
Notes: pixel composer is NOT fully ported yet (another branch contains a bunch of work as well).

### v0.26.2

STUDIO:
- [_] Start a server `Script` added at runtime. Only the client role runs new scripts (`Runtime::RunNewScripts`), so a server script cloned or parented in while the game runs never starts; `StressScripts.luau` uses closures for this reason. Maybe just don't replicate server scripts altogether.
- [_] Add team test (run server on team create host machine, everyone can join team test, when host stops server all disconnect / cancel connecting).
- [_] Add a team create chat if not already done
- [_] Test 16 person team test and improve where needed
- [_] Build out the script editor and polish the workflow around it
- [_] Add preference options for external editors for the game - temporary two way sync in filesystem?
- [_] Polish the autocompletion and typing systems to be complete - Build out the "engine dump file" that contains the default built-in things like datatypes and services, dynamic resolution, and code-based resolution (i.e. entity creation and destroy).

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
  [character system plan](../future-work/character-system.md) and the
  [navigation and AI action seam](../future-work/navigation-ai-system.md).
- [_] Stress demos to build once the character and constraint work above lands: a crowd of a few hundred animated, skinned characters (skin palettes, animation tracks, humanoid physics) and long joint and weld chains against the 4,096 joint cap, which still faults on count.
- [_] animation graphs & event
- [_] default animation controllers and graphs
- [_] G08 (animation graphs, masks, synchronise, and event markers)
- [_] C02 (Simulation and animation levels of detail)
- [_] Define character state-machine policy, animation graph composition and
  blending, and how animation follows committed character state. See the
  [character system plan](../future-work/character-system.md).

### v0.26.3

PHYSICS:
- [_] P02 (contact policies & complete physical events)
- [_] P03 (complete fast-body collision and batched world queries)
- [_] R08 (real geometry ray queries and reference path tracing)
- [_] Physics expansion (docs/future-work/physics-expansion.md)
- [_] World / scene management (docs/future-work/world-streaming.md)

ENGINE:
- [_] localization support

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
  plan](../future-work/input-system.md) and [virtual device service
  plan](../future-work/virtual-device-service.md).
- [_] expose automation tools (AutomationService) like mouse clicks and keyboard inputs to luau scripts (so we can create ai that plays for you)

### v0.26.7

- [_] W04 (Parties, matchmaking, and cross-server travel)
- [_] U01 (Project-owned gameplay actions and device profiles)
- [_] U02 (Audio effects, spatial filtering, and acoustic zones)
- [_] U03 (Streaming media, adaptive music, and voice transport)
- [_] U04 (Camera rigs, sequences, and cinematic authoring)

### v0.26.8

- [_] gtlf default character (unreal style)
- [_] /docs/future-work/terrain-system.md
- [_] Player-independent characters, controller state, animation graphs, and
  player roster/possession, plus calibrated body/hand tracking mapped to rig
  bones, in the [character system plan](../future-work/character-system.md).
- [_] /docs/future-work/vfx-system.md

### FUTURE

- [_] go through docs/future-work/REVISIT_IDEAS.md for things we can do sooner.

AI/NAV:
- [_] Navigation and AI: bounded navigation, perception, named inspectable AI
  states, behaviour plans, and driver-safe character actions in the
  [navigation and AI plan](../future-work/navigation-ai-system.md).
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
- [_] /docs/future-work/camera-and-cinematics.md
- [_] /docs/future-work/prefab-package-system.md
- [_] /docs/future-work/procedural-generation.md
- [_] /docs/future-work/session-and-social.md
- [_] /docs/future-work/audio-system.md
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
  plan](../future-work/vr-system.md), [input system
  plan](../future-work/input-system.md), and [virtual device service
  plan](../future-work/virtual-device-service.md).
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
