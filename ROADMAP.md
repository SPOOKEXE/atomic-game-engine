
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

### v0.20

- [x] StackGuard to prevent stack errors, infinite recursion, etc.
- [x] Random.new(seed) with functions
- [x] fix billboard gui in scene not rendering properly and stuff (tests pass)
- [x] check if particles load textures properly (fallback works, demo asset external)
- [x] breakpoints do NOT function. i set one in the script editor in Rings example ```local function layout(names: { string })
	for index, mesh in ipairs(names) do``` on the for loop, and it does not trigger. the Debugger widget shows the breakpoint exists, but 0 hits.
- [x] add stack watch and breakpoint watch dock widgets
- [x] auto complete popup doesn't position properly above/below text we're editing.
- [x] add a CTRL+SHIFT+F keybind for Search-All-Replace-All
- [x] The Script/LocalScript/ModuleScript buttons in Script tab is really wide for some reason
- [x] add a components view (inspector in unity) where you can see components - roblox instances too should show it.
- [x] add a way to "expose" component values as configs for "components view" - this way you can tweak values without opening it. do for all roblox values.
- [x] ensure we have the ability to create custom components in scripting, and also to query them, etc. Batched with multi-filtering (whitelist/blacklist - include/exclude).
- [x] add a way to set a tag for a given component AND per-component-value. like [deprecated], [experiment], [constant], etc.
- [x] viewport indictator direction gizmo (select and lock to certain directions)
- [x] 3d cursor and camera orbit options under gizmo
- [x] add tests to validate each step of the rendering pipeline graph
- [x] add websocket support (full async, test 10k connections and improve bottleneck spots).
- [x] add script coloring for keywords and whatnot based on theme. add theme configs for script editor highlighting.
- [x] build out the "Changes" widget properly, add a record of changes where the file saves as xml or such (timestamp-changes.xml) and you look for them and parse them.
- [x] properly build out team create menu
- [x] team create build out options properly
- [x] thoroughly implement every user-interface element, including `SurfaceGui` and `BillboardGui` - `SurfaceGui` gains `ZOffset`, `MaxDistance`, `ClipsDescendants` and `Active`, and `BillboardGui` gains `Active`, `Brightness`, `ClipsDescendants`, `CurrentDistance`, `DistanceStep`, `ExtentsOffsetWorldSpace`, `SizeOffset` and `PlayerToHideFrom`; new classes `UIGradient`, `UITableLayout`, `UIPageLayout` and `UIDragDetector`; `ScrollingFrame` completed with `ScrollingEnabled`, `AutomaticCanvasSize`, the two `ScrollBarInset`s, `VerticalScrollBarPosition`, `ElasticBehavior`, the three bar images and `AbsoluteCanvasSize`/`AbsoluteWindowSize`, plus wheel and thumb-drag input; `RichText`, `MaxVisibleGraphemes`, `ContentText`, `TextBounds` and `TextFits` on every text class; `Interactable`, the four `NextSelection*`, `SelectionOrder` and `SelectionImageObject` on `GuiObject`; `HoverImage`, `PressedImage` and `ResampleMode` on the image classes; `Enabled` and `ApplyStrokeMode` on `UIStroke`. Laid out, drawn by both backends, saved, replicated, bound and in the Properties panel. `D00129` carries the members that need a subsystem this engine has not got (filed as `D00120`, renumbered at v0.17 - that number was already a retired entry)

- [x] consolidate render pipeline to a easy-to-find location for future work.
- [x] ensure when you read the render pipeline, its obvious what it does and in what order
- [x] build out all remaining roblox surfaces with available underlying surface - all seven texture channels now resolve, stream, preview and render; metalness reaches forward and deferred PBR, alpha follows Overlay, Transparency, TintMask and Opaque semantics including masked shadows, and surface colour, emission and resampling are saved, replicated, bound and packed into the 48-byte GPU-resident instance row. Content-object aliases remain outside this item because the engine has no `Content` object type beneath them
- [x] batch world compute across stable pinned lanes when the measured work clears the dispatch crossover, while a single world keeps the worker pool for its own ECS, physics and effects batches. ECS iteration uses contiguous table spans, replication combines scene surveys with `Authority::PublishMany` and client lanes, and `Isolation::Dedicated` remains the process-per-world boundary. The regular loops remain compiler-vectorisable at release optimisation levels without a second handwritten SIMD path.
- [x] profile the remaining simulation bottlenecks and verify component iteration in dev and release builds. `docs/SIMULATION_PERFORMANCE.md` records the reproducible `-O0`, `-O1`, `-O2` and `-O3` ladder, identifies physics solve and broadphase as the remaining large costs, and separates the lightweight simulation sweep from the expensive replication publish sweep.
- [x] consolidate old demo launchers into `scripts/demos/run-demo.sh` and `run-demo.bat`; keep the authored scenes under `mono.engine/examples/` as Luau, TypeScript or JavaScript, with specialised capture and local-server tools left separate.
- [x] inventory GPU-resident and intentionally nonresident data in `RENDER_PIPELINE.md`, then verify the revision, signature and dirty-range gates that control each upload. Scene data, retained interface geometry, effects and render targets stay resident; dynamic values use compact structured transfers; statistics and flame-graph damage are isolated from scene caches. ImGui sends changed vertex and index data plus texture updates, never a CPU-rasterised GUI image.
- [x] complete and test `UserInputService` and `ContextActionService` in both VMs. The shared surfaces provide input methods, mutable and read-only properties, six signals, priority action binding, reports, callback replacement and cleanup; Luau and JavaScript pumps consume the same input stream and action stack.
- [x] implement and test `Beam`, `Trail`, `Decal` and `Texture` as saved, replicated and script-bound classes whose content is demanded and whose visible geometry reaches the ribbon renderer.
- [x] complete collider components for `Box`, `Sphere`, `Cylinder`, `Capsule`, `Hull` and exact `Mesh` geometry. Bounds, support, contacts, rays, inertia and Studio previews cover the analytic shapes; baked hulls and triangle soups cover mesh-backed shapes. `MeshPart` exposes mesh, seven surface maps and triangle metadata through the shared property surface.
- [x] enforce script access levels, capabilities and sandbox profiles for plugin, game, server and client contexts. Both VMs derive or accept explicit grants, refuse unavailable services with the required capability named, keep plugin host access separate from server services, and retain memory, step, job, global and host-surface sandbox limits.

- [x] plan out and implement the base plugin system with stable manifests, isolated runtimes, persisted enable state, a built-in Default Studio plugin, and safe teardown before world replacement.
- [x] add a toolbar editor where tabs can be created, hidden and removed, tools can be moved or hidden, and script controls can declare and change bounded widths.
- [x] add plugin dock widgets with stable identities, first-use dock targets, size constraints, an editor panel, and a generated View menu for reopening them.
- [x] add a plugin manager widget with metadata, running and faulted states, persisted enable controls, error details, widget access, and reload.
- [x] bridge engine behavior through the shared plugin host surface in Luau and JavaScript with matching value, service and callback behavior.
- [x] move every ribbon control, including transport, scene selection, manipulators and snap amounts, into atomic cells owned by the built-in Default Studio plugin.
- [x] add plugin functions for buttons, toggles, dropdowns, toolbar visibility and sizing, dock widgets, viewport options and creation, script source editing, and opening script-editor tabs.
- [x] convert viewport indicator gizmo and 3d cursor / camera orbit around it to plugin with buttons in toolbar
- [x] compose plugin toolbars as cached pinned or tabbed row, column and cell grids, with persisted tab creation, renaming, ordering, hiding and deletion from the tab context menu.
- [x] make Default Studio disable-able, expose matching Luau and JavaScript toolbar tab, row, column, cell and label functions, and hot-reload changed plugin source trees with debounced targeted restarts and structural rescans.

- [_] benchmark job system, add different types of jobs (Serial, Threaded, Processed) contexts.
- [_] can we do something to help async-compute more complex computations like noise terrain generation? it freezes main thread.

- [_] port many particle features from unity to here (https://docs.unity3d.com/6000.5/Documentation/ScriptReference/ParticleSystem.html) - the existing lifetime curves, shape emission, drag, velocity inheritance, texture sheets and orientation modes are joined by distance emission, a live `MaxParticles` capacity, one-shot `Emit` from disabled emitters and `Clear`, a speed ceiling, scrolling procedural noise, and radial and tangential acceleration. Shared emitter values occupy the six reserved words in the GPU parameter row, while the 28-byte quantised `ParticleInstance` remains unchanged. The host fallback and `particle-step.comp` implement the same forces, the authored controls save and bind in both languages, and limit edits reclaim the resident block at its new size. Collision, sub-emitters and external force fields are not inert properties here: each needs an underlying collision/event/field subsystem before it can honestly be exposed
- [_] add better memory packing for components by adding a DataQuantization component or something similar. Add quantization support for storing and packing values within components, like (u)float16, (u)float8, (u)int16, (u)int8, (u)int4 and bool. Need a way to decide who packs with what, maybe a `Component Data Packing` dock widget that shows you what values a component stores?
- [_] expand ShaderCapabilities out
- [_] expand compilation steps with: constant folding, common-subexpression elimination, node fusion and resource aliasing. When we select a shader asset, we should be able to see its ShaderCapabilities and resources it'd take (estimate compute, memory, etc).
- [_] build a RENDER_PIPELINE.md that lists current pipeline and what we need to do to make it more modular (like Unity/Unreal) and with shader compilation.

### Engine Graph + ECS Improvements + parallel::Jobs Storage (v0.20)
- [x] engine graph architecture: Input Graph → AI Graph → World Graph → Physics Graph → Animation Graph → Render Graph, unified dependencies, scheduling, CPU/GPU jobs, synchronization, resource lifetime, profiling
- [x] one registration path for render nodes: fold scope/queue metadata and requirements into graph::NodeCatalogue; derive BackendNodes(); studio widgets read Params
- [x] DeviceCaps probe + CheckCapabilities: probe in Initialise(); wire refusal messages; studio requirements column
- [x] custom native node kinds: RegisterNodeKind + Renderer::InstallNodeHandler; lifecycle hook on reinstall; demo custom kind in examples
- [x] per-node GPU profiling: mark assignment in GraphRunner, grid column, tier switch; assert dropped-mark accounting
- [x] explicit conversion nodes + narrowing rule: explicit blit format targeting; LossyWire demoted to hint when explicit conversion sits between producer/consumer
- [x] tiered default pipelines by capability: Tier B/C documents; capability-driven pick at install; WorldPipelines extension asserting fall-through reasons
- [x] ECS component change tracking: dirty bits/version counters so systems only process changed data
- [x] archetype/query optimizer: cached ECS queries, change filters, parallel query execution
- [x] entity references/handles: generation-safe references instead of raw entity IDs
- [x] world snapshots & cloning: serialize/restore entire world state; instant-ish duplicate world for testing, previews, server simulation
- [x] rollback/snapshot system for networking and deterministic simulation
- [x] system dependency graph: explicitly declare before/after, parallelize independent systems
- [x] frame scheduler: CPU jobs, GPU jobs, async jobs and synchronization points represented together
- [x] engine tick phases: Input → Simulation → Physics → Animation → Replication → Render preparation → Render
- [x] determinism mode: detect nondeterministic simulation and optionally enforce deterministic ordering
- [x] hot-reloadable components/systems
- [x] parallel::Jobs pool fix: replace the leaked static pool and join worker waiters during teardown

- [_] `~/Documents/GitHub/BLADEBORNE_UNIFIED/game` port and also studio place `~/Documents/Bladeborne Floor 0.rbxl`. Turn this into a demo file.
- [_] roblox porting tools (rbxl) - in the widget that pops up, show all asset ids and make a assets selector so you can click which asset id points to which file asset (same for animations and whatnot where possible).
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) - untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds. Will show a widget that tells you conflicts and missing classes.

### v0.21

- [_] wire future components — scene.Skeleton, scene.Bone, scene.AnimationClip, scene.Animator, scene.AnimationTrack, scene.Constraint, scene.LevelOfDetail, scene.Atmosphere, scene.Clouds, scene.Terrain
- [_] skinning pipeline — assets::MeshVertex gains joint indices/weights; bake fills them; render builds palette per rig; animation handler samples clips
- [_] atmosphere/clouds at v0.22 — render-graph node reading WorldLighting.Air/Sky; per-world presentation state, no simulation input

- [_] build out file format even more, save all shader scripts, universe/world settings, etc. Also support separating worlds into separate files and universe finds them in same folder / subfolders (enable a recursive flag, DO NOT walk links)
- [_] in world export, add a option to ground ALL assets into a assets/ folder that saves with the world. copies from cdn and all, only processed saved.
- [_] in cdn, add optinos for compression saving, compressed network traffic, etc.
- [_] add custom backgrounds and customization to the script editor too
- [_] add external editor connections like vscode, notepad, etc.
- [_] add settings for selected external editors
- [_] plugin modify the viewport grid colors, sizes/scales, offsets, etc
- [_] plugin enable/disable visible particles
- [_] ask about the other view widgets and which you actually would change to dockwidgets.
- [_] build out more plugins layer that calls the functions needed for engine behaviors, hook to plugin luau and js, and hook other side to engine
- [_] move a bunch of View > ... widgets into plugins instead. List: explorer, properties, component inspector, script editor
- [_] build out more plugin functions (create dropdown, edit toolbar, edit viewport, edit script editors, etc)
- [_] universe shared assets folder and setup easy cdn with it (when you load the universe file, it sets up a cdn with it).
- [_] add a universe loading widget - shows cdns the universe has and asks to allow permission, also http enabled property if changed
- [_] add tabs to the universe importer: general, assets, permissions, cdn, misc with all or per-world breakdown
- [_] create a universe/world export menu with options to include assets in the export (only processed, also raw), as a folder of a assets/ with name.aworld (or we can do a name.auniverse that loads a bunch of name.aworld and warns on missing ones listed in auniverse)?
- [_] more cleanly separate the luau/js and roblox-style system from ECS. We want a clean shim where: luau/js => roblox instance => shim => ECS-driven underlying. Find areas where we're not doing this and improve it.
- [_] setup datastores and memorystores (sqlite, mongo, supabase, etc - make a selection with local and remote setups, server settings and studio settings). add mock options that separate into a mock/ folder vs live/ folder. make a dataset editor plugin too.
- [_] add platform-specific backend tooling where only "admitted keys" can connect to a given server - i.e. whitelist-based servers (press play on website => generate play key => platform tells server user is connecting with key => send key + server to user => user connects to server using key and info => join)
- [_] cleanup launcher options and make it far more user friendly
- [_] cleanup cdn config and make it far more friendly
- [_] build out client settings for enabling/disabling certain features to help performance (editablemesh, editableimage, etc). make it LIVE so you can change it in-game.
- [_] add a ESC settings menu to the client and in studio client. add the client settings to it.
- [_] add a way for scripts to modify the ESC menu.

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
