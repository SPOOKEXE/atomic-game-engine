
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

- [_] wire future components: scene.Skeleton, scene.Bone, scene.AnimationClip, scene.Animator, scene.AnimationTrack, scene.Constraint, scene.LevelOfDetail, scene.Atmosphere, scene.Clouds, scene.Terrain
- [_] skinning pipeline: assets::MeshVertex gains joint indices/weights; bake fills them; render builds palette per rig; animation handler samples clips
- [_] atmosphere/clouds at v0.22: render-graph node reading WorldLighting.Air/Sky; per-world presentation state, no simulation input

- [x] build out file format even more: save ShaderScripts and universe/world settings; support `.auniverse` manifests with separate `.aworld` files and optional recursive discovery that never walks links.
- [_] in world export, add a option to ground ALL assets into a assets/ folder that saves with the world. copies from cdn and all, only processed saved.
- [x] in CDN, support persisted trained compression dictionaries, configurable Zstd levels, compressed bundle traffic, and prepared-frame caching.
- [_] add custom backgrounds and customization to the script editor too
- [_] add external editor connections like vscode, notepad, etc.
- [_] add settings for selected external editors
- [_] plugin modify the viewport grid colors, sizes/scales, offsets, etc
- [_] plugin enable/disable visible particles
- [_] ask about the other view widgets and which you actually would change to dockwidgets.
- [_] build out more plugins layer that calls the functions needed for engine behaviors, hook to plugin luau and js, and hook other side to engine
- [_] move a bunch of View > ... widgets into plugins instead. List: explorer, properties, component inspector, script editor
- [_] build out more plugin functions (create dropdown, edit toolbar, edit viewport, edit script editors, etc)
- [x] give a universe a shared `assets/` store and mount it automatically as the first local content source when the universe loads.
- [x] add a universe loading widget that lists declared CDNs and requests permission before enabling HTTP content access.
- [x] add universe loader tabs: General, Assets, Permissions, CDN, and Misc, with all-world and inherited per-world views.
- [x] add `.auniverse` export options for verified processed assets and optional raw authoring files under `assets/`.
- [_] add the matching grounded-assets options to standalone `.aworld` export.
- [_] more cleanly separate the luau/js and roblox-style system from ECS. We want a clean shim where: luau/js => roblox instance => shim => ECS-driven underlying. Find areas where we're not doing this and improve it.
- [_] setup datastores and memorystores (sqlite, mongo, supabase, etc - make a selection with local and remote setups, server settings and studio settings). add mock options that separate into a mock/ folder vs live/ folder. make a dataset editor plugin too.
- [_] add platform-specific backend tooling where only "admitted keys" can connect to a given server - i.e. whitelist-based servers (press play on website => generate play key => platform tells server user is connecting with key => send key + server to user => user connects to server using key and info => join)
- [_] cleanup launcher options and make it far more user friendly
- [_] cleanup cdn config and make it far more friendly
- [_] build out client settings for enabling/disabling certain features to help performance (editablemesh, editableimage, etc). make it LIVE so you can change it in-game.
- [_] add a ESC settings menu to the client and in studio client. add the client settings to it.
- [_] add a way for scripts to modify the ESC menu.
- [_] gamepad and joystick support
- [_] completely move to vulkan and establish cross platform supports.

- [_] `~/Documents/GitHub/BLADEBORNE_UNIFIED/game` port and also studio place `~/Documents/Bladeborne Floor 0.rbxl`. Turn this into a demo file.
- [_] roblox porting tools (rbxl) - in the widget that pops up, show all asset ids and make a assets selector so you can click which asset id points to which file asset (same for animations and whatnot where possible).
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) - untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds. Will show a widget that tells you conflicts and missing classes.

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
