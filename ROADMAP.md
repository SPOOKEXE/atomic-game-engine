
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

### v0.21

- [x] build out file format even more: save ShaderScripts and universe/world settings; support `.auniverse` manifests with separate `.aworld` files and optional recursive discovery that never walks links.
- [x] ground every verified catalogue asset into the standalone `.aworld` export's sibling `assets/` store, fetching through the configured delivery sources and optionally including raw authoring folders without following links.
- [x] in CDN, support persisted trained compression dictionaries, configurable Zstd levels, compressed bundle traffic, and prepared-frame caching.
- [x] let Luau and JavaScript plugins modify viewport grid colours, step/scale, major spacing, reach/size, strength, alpha, and X/Z offsets through the shared host surface.
- [x] let plugins enable or disable visible particles without changing emitter state.
- [x] keep render/debug/transport views native until replacement is useful; migrate the daily editing panels whose declarations benefit from plugin ownership.
- [x] route native editor behavior through one value-based plugin host surface shared by Luau and JavaScript.
- [x] move Explorer, Properties, Component Inspector, and Script Editor declarations into the Default Studio plugin while retaining their native ECS adapters.
- [x] add plugin functions for dropdowns, toolbar layout, viewport creation/options, docked widgets, and script-editor source/open integration.
- [x] give a universe a shared `assets/` store and mount it automatically as the first local content source when the universe loads.
- [x] add a universe loading widget that lists declared CDNs and requests permission before enabling HTTP content access.
- [x] add universe loader tabs: General, Assets, Permissions, CDN, and Misc, with all-world and inherited per-world views.
- [x] add `.auniverse` export options for verified processed assets and optional raw authoring files under `assets/`.
- [x] add script-editor customization: a persisted code background override can follow or replace the active theme, and the minimap can be toggled independently.
- [x] add external editor connections for the system default, Visual Studio Code, Notepad and a custom executable; program and ShaderScript tabs use watched staging files, import external writes, and expose two-sided conflict resolution.
- [x] add a Script Editor preferences page for the selected external editor, executable override, background and minimap.
- [x] more cleanly separate the luau/js and roblox-style system from ECS. We want a clean shim where: luau/js => roblox instance => shim => ECS-driven underlying. Find areas where we're not doing this and improve it.
- [x] add shared DataStore and MemoryStore administration, durable local DataStore images, isolated mock/live folders, server and Studio local-provider settings, and a Default Studio dataset editor plugin.
- [_] add remote datastore choices and per-datastore backend assignment through the DataStoreRouter/DataStoreAdapter seam below.
- [x] add platform-specific backend tooling where only "admitted keys" can connect to a given server - the client proves a platform-issued Ed25519 play seed without sending it, servers accept repeatable configured public keys, and the loopback control surface can list, allow, revoke or explicitly open admission for future sessions.
- [x] replace the launcher's raw option wall with discoverable mode cards, searchable Common/All options/Engine settings tabs, typed controls and path pickers generated from each program's declarations, a command preview, retained per-mode forms, and supervised launch/restart/stop status.
- [x] consolidate CDN deployment configuration into declared `cdn.*` settings with config/environment/CLI precedence, readable serving and publishing options, repeatable named upstreams, explicit local/cache/proxy switches, safe opt-in ingest and forwarding, validation of contradictory setups, and a terminal dashboard for live status.
- [x] build out live client presentation settings: EditableMesh and EditableImage uploads, particles and post-processing can be disabled by config/CLI and toggled in-game without restarting.
- [x] add a ESC settings menu to the client and in studio client. add the client settings to it.
- [x] let client Luau and JavaScript add, relabel, remove and activate named ESC menu actions through SettingsService; the ECS owns the bounded action list and both standalone and Studio Play render it.
- [x] add gamepad and joystick support: SDL mapped pads and raw joysticks occupy eight stable world slots; normalized buttons, sticks, triggers and hats drive movement, camera, jump and firing in the client and Studio Play; Luau and JavaScript receive matching polling APIs, connection signals, input edges and analog changes.
- [x] use SDL GPU's Vulkan-convention render path with SPIR-V on Vulkan devices and translated MSL on native Metal devices; choose the device's supported shader format at runtime and keep both built-in and live ShaderScripts on the same path.
- [_] resolve the remaining graphics-backend direction: either require Vulkan everywhere and package MoltenVK for Apple targets, or retain native Metal and add a DXIL build/runtime translation path before enabling SDL GPU's Direct3D 12 backend.
- [x] wire the future scene vocabulary: Skeleton, Bone, AnimationClip, Animator, AnimationTrack, Constraint, LevelOfDetail, Atmosphere, Clouds and Terrain are registered, reflected, persisted where their variable payload requires it, documented, and covered by scene suites.
- [_] skinning pipeline: assets::MeshVertex gains joint indices/weights; bake fills them; render builds palette per rig; animation handler samples clips
- [x] render atmosphere, clouds and textured or procedural skies from each view's `WorldLighting::EnvironmentState`: the environment render-graph pass selects authored or compute modes, keeps signatures and resident targets per presentation slot, and consumes presentation-only scene values without feeding simulation.
- [x] review v0.21 Studio coverage: ShaderScripts are editable in the shared source editor with GLSL autocomplete, save through their revisioned ECS property, round-trip in world/universe files and compile by name in the renderer; the full headless check covers the authored chain.

- [_] `~/Documents/GitHub/BLADEBORNE_UNIFIED/game` port and also studio place `~/Documents/Bladeborne Floor 0.rbxl`. Turn this into a demo file.
- [_] roblox porting tools (rbxl) - in the widget that pops up, show all asset ids and make a assets selector so you can click which asset id points to which file asset (same for animations and whatnot where possible).
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) - untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds. Will show a widget that tells you conflicts and missing classes.
- [_] build out the bladeborne demo into a full mmo, test with thousands of connected clients with ai minds for them so they can run and do stuff

- [x] add the datastore routing seam: `DataStoreRouter` assigns stable datastore names to `DataStoreAdapter` providers owned by `DataStorageServices`; server and Studio persistence use its built-in atomic file adapter, while remote provider choices remain the current-work item above.

### v0.22

- [_] default R6 base character (capsule collider)
- [_] gtlf default character (unreal)
- [_] plan out full character system + roblox humanoid shim + full roblox character controller shim (essentially custom instances for exposing the controller stuff)
- [_] make humanoid a shim for character controller (so not a black box), loads a default one in
- [_] character controller + humanoid + character states + state controller + bone controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.
- [_] animation handler
- [_] skinning and animation - `bake` skips joints and weights and keeps the rest pose, because there are no skeletons in the engine yet - joint palettes are visual transform state and go GPU-resident beside the instance rows; the animation *controller* that produces them stays on the CPU, per the split above
- [_] add accessories support
- [_] add future addition spots like animation trees, blueprints, state blueprints, etc. blueprints = node graph.
- [_] animation + animation track + animator => binds to character controller
- [_] build out ArcHandles, BoxHandleAdornments (and similar), etc
- [_] make virtualised Gui2D and such where they are just a set of components instead of an actual class that is accessible (since you normally cannot create them in studio). Ask user which to keep and which to convert to virtual instances. Needs hierarchy too.
- [_] ensure :IsA() handles virtual instances and does hierarchy.
- [_] ensure valueobjects work
- [_] ensure you built out all the UI items and they work (e.g. drag selector)
- [_] ensure weld / weld constraints work
- [_] ensure ViewportFrame and WorldRoot are implemented and work

### v0.23

- [_] security audit, fuzzy tests, bound tests, etc.

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

### FUTURE

- [_] (procedural, node-based) terrain generator (refer to discord references) - editablemesh, greedymesh, noise layers, node graph with previews, chunk-based, etc. Add voxel mode (which separates cardinal facing direction Fnt/Bk/Lft/Rgt/Top/Bott faces into groups - only renders the two groups it can see). Expand with surfacecameras, portals, etc, so it culls, occulusion culls, etc.
- [_] unity porting tools / unity shop
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
- [_] embedded whiteboxing tools (planning) for building
- [_] full procedural terrain studio tools
- [_] full ui feature buildout + custom
- [_] level-of-details (4 different meshes version, auto-decimate version, smart-triangle-reduction-version thinking of nanite triangle surface area) - LOD selection is a per-instance visual decision and belongs in the GPU-resident set beside the occlusion cull that already runs there, so a level change costs no CPU round trip.
- [_] project demos: space engineers asteroids + planets full demo, blackhole simulator (warp space, warp visual, etc), huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot, user interface (copy bladeborne's for demo?)
- [_] html-based ui creation (html-script?) => auto handles aspect constraints and whatnot as well
- [_] import blender files in asset explorer natively (drag .blend files on engine)
- [_] rpg maker port tool
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
- [_] create another demo of a ai npc village where they have daily tasks and things like that (dwarf fortress style - personality, occupation, etc).
- [_] VR support (oculus rift s)
