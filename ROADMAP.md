
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

### v0.25

- [x] USER WORK: cleanup documents in `docs/`, maybe a `docs/systems` folder would be more suited for things like `RENDER-HOOKS.md`, `DEMOS.md`, `ECS_COMPONENTS.md`, `schema.toml` and `schema-data.toml`.
- [x] simplify down RUNNING.md, should be minimal, shows each available `just` job, how to build each, etc. Should not contain lots of descriptive information about how those systems work, just short descriptions and what they are aimed at to do.
- [x] improve `schema.toml` and `schema-data.toml` so its better laid out (schema is the general layout, schema-data is the actual useful information that we would grep and search specific classes, components and functions in). Like Roblox Studio Class API Reference.
- [x] consolidate/improve `CONTRIBUTING.md`, `SECURITY.md`, `docs/THIRD_PARTY_NOTICES.md`, `CODE_ARCH.md`, `CODE_DOCUMENTING.md`, `CODE_FORMAT.md` and `CODE_QUALITY.md`, with small sentences at the start of the file describing what they contain in succinct detail.
- [x] cleanup documentation doxy and layout
- [x] update and prune old content in documentation (doxy). check each statement, update, remove or replace.
- [x] check LOD is cleaned up when the mesh changes / is deleted / LOD properties are changed so they release and are recomputed
- [x] fix multi-select multi-property editing (when i select multiple objects, it should check all objects for the same component and value im editing and match them).
- [x] add a override LOD distance per-item with default value being set to preference one.
- [x] separate the LOD component into LODAuto/LODCustom/LODSettings, LODCustom overlays LODAuto (so we can still have auto options but overriden by LODCustom options).
- [x] rename preferences LOD distance to "Default Mesh LOD distances".
- [x] fix camera detached in bladeborne aworld demo
- [x] fix lights passing through portals not working
- [x] add extensive (freecam) camera tests (like flying through portals)
- [x] add extensive client character tests
- [x] add extensive client character CAMERA tests (zooming out and projecting camera through portal)
- [x] improve atomic-game-engine build file usage sizes. Takes over 100GB right now, needs to be reduced. Reduce hash for each mono repository to a reasonable size for each, cleanup old files, etc. Find what takes up all the space and try improve it.
- [x] Prune `PLAN-procedural-planets.md`, `PORTAL-HANDOFF.md`, `RENDER-POST-HOOK-REFACTOR.md`, `RENDER-REFACTOR-TASKS.md`, `RENDER-REFACTOR.md` and `TORNADOSIM.md`.
- [x] Prune old files in `docs/future-work/` as well. e.g. merge `FUTURE_COMPONENTS.md` components into relevent document files in `docs/future-work/`, then leave the remaining orphaned ones in `FUTURE_COMPONENTS.md`.
- [x] review and plan a cleanup of the render pipeline. Write docs/v025-RENDER-PIPELINE-CLEANUP.md. This can be logic cleanup, better layout, components separation, merging, renaming, potential test points, areas to investigate logic (that seem wrong and need to be investigated), etc.
- [x] plan a consolidation and cleanup for the MCP-ADDITIONS.md systems. Super big, needs to ensure its properly implemented and looks good. Maybe even isolating code to a separate folder and then using hooks to ingest (and make those hooks hot loadable and unloadable so we can disable when we don't need to use them). Write docs/v025-MCP-CLEANUP.md.
- [x] add a "light path visualiser" that shows a visualisation of the spatial casting of light emitters so i can see what path they take, what they hit, etc. basically blue for empty space it travels, red for end of light, orange for pass-through or reflections.
- [x] create a "SkyGridPBR" demo of floating terrain balls with each one having one of 8 custom made shaders, then have the camera fly forward between the seams. this is a benchmark called BenchmarkSkyGrid.luau built-in demo example. We'll also use this as a performance profiler for editablemesh + terrain + etc.
- [x] create two stress test demos: 100 unique 4k textures on material spheres with PBR (like the PBR demo), and 1 unique 4k texture on material spheres with PBR. tests instancing (for 1 duplicate item) and mem/compute usage for the uniques.
- [x] add a way to "virtually lock" the camera position, with a adornment visual, such that all camera behavior acts as if its from that location, this way i can test if culling works and other behaviors.
- [x] pack multi-channel supporting render data in other channels, depth = r channel - ambient occulusion = g - anti-alias = b, for example.
- [x] Do cleanup in `docs/v025-RENDER-PIPELINE-CLEANUP.md`
- [x] Do cleanup in `docs/v025-MCP-CLEANUP.md`
- [x] stress test all underlying engine systems (input, cdn, assets, parallel world, physics, hundreds of players + characters all moving around randomly, etc). for each, find at least 5 optimisations.
- [x] add typed, read-only physics observation hooks at declared fixed-tick boundaries. Use stable string discovery names, typed immutable per-hook contexts and bounded non-blocking records. Define whether each record observes pre-solve, completed-solver or post-integration state; retain exact tick, world, units and availability; and expose completed records to data-factory MCP without allowing a hook to mutate physics state.
- [x] add typed, read-only replication observation hooks at declared exchange boundaries. Use stable string discovery names, typed immutable per-hook contexts and bounded non-blocking records. Preserve world, authority, client, baseline, tick and exchange-round identity; expose applied, rejected, repaired and dropped work without crossing a world boundary by pointer; and keep private payloads behind the existing permission boundary.
- [x] optimise server startup time
- [x] optimise and improve tests (particularly server and physics, can we add deterministic hooks so we can immediately wait for an update for a change instead of guessing with timestamps? test.solver, test.replication, etc)
- [x] LOD system billboard render support
- [x] /docs/future-work/ui-system.md
- [x] plan how to fix portals so they are seamless. really plan out how to make them seamless and how to handle "standing in the middle" so objects are visually there on both sides of the portal with no seam especially during movement (and how to make replication seamless too). Write docs/v025-SEAMLESS-PORTALS.md.
- [x] implement seamless portals plan

- [x] Fix client cleanup for rows after a visibility Forgotten message, with regression coverage.
- [x] Optimize recovery-row serialization by moving the ByteWriter buffer. The recovery benchmark improved from 294±35 to 212±22 ns/item across 15 samples.
- [_] Investigate scoring performance. A candidate fast path showed a noisy 4-5% mean improvement and was reverted, so scoring remains unresolved.

- [_] more lighting capabilities; god rays, blue, depth of field, fog fields (not global fog, more like "fog across area of ground" - maybe 'fog volumes' with different shapes and size/squash/fluffy sliders?).
- [_] create a weather system demo using all the lighting capabilities (clouds, atmosphere, rain particles, etc).
- [_] remake the tornado simulation demo using the C++ repository as a base. Check the existing documentation, add missing engine features that we need (ask user questions about it first, you'll need to swap into plan mode), then implement once you get the OK.
- [_] fix and expand current lighting items (clouds, sunrays, atmosphere, etc). Needs actual tests, overlapping, compute shader tests to ensure they work, etc.
- [_] get an agent to interact with and use studio EXTENSIVELY. check all dropdowns and ensure all those work, interact with studio, make a little game/scene/scenario and run it / play in it, move character around, check all 'classes' and their capabilities and ensure they work, etc.

### v0.26

- [_] go through engine and consolidate and cleanup dead code branches. remove backport compatibilities with previous engine versions and ground this as the version.
- [_] go over render system and consolidate/improve hooks, nodes, graph system and visualiser of graph system

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
* floating islands with village houses on them with bridges connecting them together, floating above clouds, minecraft-like

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

- [_] idea: for the LOD system, could we move mesh details into a normal map as part of the LOD? this way we can have a 'performance mode' that focuses on using this method instead of pure mesh data to show the details (even if it looks slightly uglier)
