
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

### v0.17

- [x] pbr rendering support
- [x] ~/Documents/GitHub/node-graph-template
- [x] batch multiple worlds and cameras into one GPU command buffer, with world-scoped graph work submitted once per world and pipeline
- [x] submit instance, particle, ribbon and overlay uploads through dedicated SDL copy command buffers without a CPU fence; cycle destinations per view and expose actual upload bytes and submissions
- [x] submit async-eligible compute prefixes on dedicated command buffers when no dependency-bound GPU work precedes them; expose actual dispatch and async submission counts
- [x] replace the old fixed renderer with the render graph; remove the fixed pass API and flat pipeline, ship a default graph, and make the graph-owned `View` batch the only public rendering entry point
- [x] Studio render graph editor with Blender-style typed sockets and wires for images, buffers, entity lists, cameras and material channels, plus controls for culling, queue choice, async eligibility, GPU compute dispatch, transfers and resource lifetime
- [x] execute authored frustum, distance and tag culling as composable entity-flow nodes; Studio exposes only the controls those nodes consume and the backend refuses misplaced hints
- [x] integrate `SurfaceAppearance` colour, normal, roughness, ambient-occlusion and emissive maps with the default graph
- [x] sample `SurfaceAppearance.HeightMap` with parallax UVs in the opaque and G-buffer material paths
- [x] render pipeline is per world, not per process
- [x] render pipelines are saved in the world's export data
- [x] rendering pipeline debugger and profiler with per-node operations, CPU wall time, GPU timestamps, expandable stage images and histograms; the default graph ends at one `Output Image` after scene, debug and interface image composition
- [x] preserve valid per-node Vulkan GPU timestamps when an async-eligible compute prefix uses a dedicated command buffer submitted before the main graph command buffer
- [x] rendering pipeline additions: ambient occlusion, emissive, PBR and a default node setup containing all of them
- [x] apply the useful traffic, clear and CPU-to-GPU lessons from https://www.youtube.com/watch?v=SnNm7rSSvlg (Threat Interactive Tutorial: How To Optimize Almost Every Step In Modern Game Rendering)
- [x] review https://github.com/fini03/vkDuck for queue, resource and command-recording architecture
- [x] redo generic authored `raster`, `dispatch`, `viewer`, `capture` and pipeline/scope-owned graph-target allocation from `renderer-before-revert` against the current renderer instead of restoring the stale branch
- [x] connect Lighting service properties to both client and Studio rendering
- [x] concise `RUNNING.md` tutorial for publishing a CDN store and using it as a folder or HTTP server
- [x] concise `RUNNING.md` tutorial for pointing a game server at a custom CDN
- [x] concise `RUNNING.md` tutorial for adding a local store or remote origin to Studio
- [x] concise `RUNNING.md` tutorial for launching a client that accepts the server-announced CDN, grant and publisher key, with an explicit CDN override mode
- [x] ensure we can attach shaders to (visual) assets as well to change how they render: mesh, texture, gifs, images and videos
- [x] localtransparency property for clients (only visible in local side, server cannot see), take priority over standard transparency if not set to 0
- [x] character auto-rotation
- [x] set character with LocalTransparency=1 when you zoom in (make a camera controller system with shift lock, scroll zoom, I/O zoom, poppercam system - add items to ignore in zoom and make partly transparent using LocalTransparency). Would it be worth changing LocalTransparency to a component and we can add/remove as needed per-client?
- [x] add compute and postprocessing shaders to meshes/textures/pbr
- [x] wireframe view mode
- [x] add emissive textures / pbr
- [x] real mesh creation in real-time (EditableMesh) + ensure it renders properly
- [x] ensure EditableImage works and renders
- [x] add conservative occlusion culling after the renderer has a depth hierarchy and indirect draw path; explicit occlusion documents are refused until then
- [x] create portal test scenes for lighting through portals and out of portals (one light in portal, check if emits out of portal, two colored lights on either side of portal, check emits inside and mixes)
- [x] audit every remaining service, object and instance property so edits mutate authoritative state and reach their consumers
- [x] prototype portal seam light-field capture: render each room against a lit void, capture both directions, and project the matching result into the portal entrance; expose an `Enabled` property on Portal components to skip this capture path - prototype-scoped: the `portal-capture` node renders each enabled mouth's far room into a 128x128 probe cleared to the world ambient (fog disabled), and `deferred-lighting.frag` projects the nearest two probes out of their entrances as a windowed rectangle light; `Portal.Enabled = false` withdraws the seam and with it the probe, the spill and the light copies. Two projectors per view, opaque-list content, no interreflection - the full seam-lighting item below is where that grows up
- [x] check raytracing with portal test scene as well - there is no executable raytraced path yet: the catalogue's `raytrace` kind has no backend node, the renderer refuses such a graph whole and the profile falls back to Default PBR, pinned in `client.scene.worldpipelines`; portal scenes therefore light through the raster seam-copy path only until the raytrace port lands
- [x] split dependency-bound compute and later transfer work into traffic-plan command buffers; physical overlap remains unavailable because SDL exposes one unified queue rather than independent graphics, compute and transfer queues - `graph::PlanCommandBuffers` is the traffic plan, downloads ride a later-transfer buffer submitted after the main one, the async compute prefix is plan-gated, and dependency-bound compute stays in the main stream because the present is bound to the buffer that acquired the swapchain
- [x] `ShapeCast` needs a swept-volume walk and its own de-duplication rule; the current union of the start and end box bounds is loose over long sweeps, and the ray walk's run rule only applies to the centre line
- [x] show Universe in Explorer with mutable execution mode, maximum catch-up ticks, bus budget, channel queue limit and channels-per-world; show federated mode, world counts, fault counts, tick cost and bus traffic read-only
- [x] build ui features out (more roblox): flex list layouts - `UIListLayout` `Wraps`/`HorizontalFlex`/`VerticalFlex`/`ItemLineAlignment` and `UIFlexItem` with `FlexMode`/`GrowRatio`/`ShrinkRatio`, laid out, saved, bound and in the Properties panel
- [x] build out code editor with auto complete (more vs-code like, hover on keywords, right-side minimap) - hover tooltips from the same language-aware surface the completion uses, a run-stripe minimap with click/drag scroll, Tab accepts, matched prefix highlighted, kind/doc footer on the list
- [x] add per-world physics tick-rate and replication tick-rate - `WorldSettings` gains `PhysicsTickRate` and `ReplicationTickRate`, zero on either meaning "follow the tick rate"; `physics::PhysicsClock` is a per-world store resource that turns simulated seconds into fixed steps, so a world may solve slower or faster than it ticks and a fast world finishes its extra steps inside `physics.contacts`; the replication clock lives on `World` and holds the change bits across the ticks it does not publish, so a property written on a skipped tick still reaches the wire; both survive the `.agame` file and the universe snapshot, both reach a host process through `--physics-tick-rate`/`--replication-tick-rate`, and the control surface reports them. The studio gap this left is closed: clicking a world row in the Explorer selects it, and Properties edits `TickRate`, `IdleTickRate`, `PhysicsTickRate`, `ReplicationTickRate`, the simulated latency and the fault limit against a new `Universe::Reconfigure` - which ignores the name, because the registry is keyed on it, and which the editor follows with the `physics::SetPhysicsTickRate` push that `world` at L4 cannot make itself. The same pass fixed the universe row, which had never been selectable at all: its tree node was missing `ImGuiTreeNodeFlags_OpenOnArrow`, so every click toggled it open and the `IsItemToggledOpen` guard threw the selection away, leaving already-editable universe settings unreachable
- [x] full particle demo (spam a ton of emitters in a baseplate scene) - `examples/StressParticles.luau`: ten labelled bays carrying every authored `ParticleEmitter` property `Particles.luau` leaves untouched (`Brightness`, `LightEmission` against `LightInfluence`, `EmissionDirection`, `ShapePartial`, `InAndOut`, `TimeScale`, `ZOffset`, all four `Orientation`s, `Enabled` pulsed from the tick, and `LockedToPart` against `VelocityInheritance` on a swinging arm), and then 102,400 emitters on the baseplate at five particles each with `Additive` alternating so the draw loop's state grouping has two runs rather than one. 320 squared is not arbitrary: it is the largest grid the 524,288-slot default pool holds without refusing emitters, and a refused emitter is silent. All three of these scenes, and every other example shipped in `mono.engine/examples`, are reachable in the studio as `New Scene from Example` - in the `World` menu, in the universe's context menu in the explorer, and behind the Worlds panel's `Example...` button, which are the three places that already offer New World. One function submits the rows and it walks the staged directory through `examples::ExampleScenes` rather than a hand-kept list, so a scene added to the repository is in all three without a second edit
- [x] full physics demo (100k parts moving around) - `examples/StressPhysics.luau`: 100,000 unanchored blocks in a 240 metre tray whose floor and four walls are one rigid body re-placed every tick, so the low corner walks round the tray and nothing ever settles. `Cube.luau` is the pile at rest at ten thousand, which is the solver's worst case and a scene that ends; this is the case where every body has a velocity every tick, so nothing sleeps and the dynamic index is rebuilt from nothing each time. It costs five property writes a tick and no per-block work at all. **`client --script` will not simulate it** - a client installs `RegisterCharacterSystems` and not the pipeline, so the blocks hang in the air - and the two hosts that do are the studio's Play and `server --game`. `--physics-tick-rate` is the v0.17 knob the scene exists to be pointed at, and it is measured in the file's header: `release`, 24 threads, headless at 30 Hz, 600 ticks, the tick rate costs a 361 ms mean tick and a rate of 10 costs 125 ms - two ticks in three at 22 ms and the third carrying the whole solve. A hundred thousand contacting bodies is past interactive on this machine either way, which is the honest reading of a stress scene rather than a caveat on one
- [x] make a ico-sphere mirror ball scene (subdivisions, radius) - basically to stress test mirrors - `examples/StressMirrors.luau`: an ico-sphere subdivided in Luau (0 is 20 facets, 1 is 80, 2 is 320, 3 is 1,280) with a flat mirror tile on every face, sized from its own edge so `RADIUS` and `SUBDIVISIONS` both scale it, and an `EditableMesh` core behind them so a reflection has something to be on. **The finding is `scene::MAX_SURFACES`, which is sixteen** - a slot owns a ping-ponged texture pair, `AimSurfaceCameras` hands them out in walk order, and a camera that gets none is aimed every frame and reaches no screen - so the scene authors sixteen cameras spread over the ball and leaves the other facets plain, with `MIRRORS` as the deliberate lever for asking for more. `workspace.SurfaceBounces = 1` is a guard rather than a taste: a ball is the worst shape the automatic depth rule has, every pane can see most of the others so it says "deeper" at every level, and the passes go as `panes x (panes - 1) ^ (levels - 1)` - 16 at one level, 240 at two, 3,600 at three
- [_] tunnels demo portal void has parts in the center of the walk path, take screenshots in a test to ensure it works
- [_] fix cross-world portal rendering (cannot see character on other side) - ensure test
- [_] editablemesh has no transform component (does not render)
- [_] mirror stress test needs ALL faces to have mirrors, also should be a filled icosphere instead of a sphere with holes
- [_] terrain demo, color them, also only do a 512x512 area with perlin noise 1 unit vertex interval.
- [_] a few of the portal scenes (in all the various worlds) have z-index fighting issues
- [_] mirrors-4-worlds demo, the reflection is super dark. this is for ALL mirrors.
- [_] add a right-click zoom-to in studio for objects with transforms
- [_] magic demo is broken (script error) - ```script error: examples/Magic.luau:44: ReplicatedStorage.MagicCore was not mounted - is assets/lib staged?
[C] function assert
examples/Magic.luau:44```. Same with TerrainCore.
- [_] the custom terrain system should be: 1. a underlying EditableMesh 2. pure luau code
- [_] move magic core system INTO the scene demo, it should NOT be a studio library, move to pure script
- [_] when you enter a portal, the (replication) interpolation makes it jump through space visibly. should we have a "portal move" packet or a "instant move" or a "full component sync" for it?
- [_] PortalLightMix, when you're on the side view of the mirror, you move left the light shows up only on left side, you move right left side hides and right sides hows up, lighting culling with portals
- [_] PortalLightMix Lighting service brightness is set to 0 and no lights emit when brightness is 0.
- [_] EditableImage demo does nothing its just purple-black checker? was it meant to make a editableimage then set the meshpart's texture to it? if so, we'll probably need to setup something proper for the texture component where we have a underlying asset delivery for pulling cdn items or in-memory items from luau / ecs engine like EditableImage objects and their buffered data.
- [_] particles are SUPER slow and they don't render visually either (refreshEmitters, stepParticles)
- [_] add a PAUSE button to the flamegraph so i can pause it
- [_] add a dropdown to add a update interval option to the flamegraph AND add a "average" checkbox that will average the results across the interval OR only shows on the update outputs

### v0.18

- [_] build out proper code architecture documents (AGENTS.md, docs/CODE_FORMAT.md, docs/CODE_QUALITY.md) => CODE_ARCH.md
- [_] DOMAIN DRIVEN DESIGN & HEXAGONAL ARCHITECTURE
- [_] check if we need to move files / classes / structures around in the codebase to properly fit (mainly focus on engine)
- [_] properly make a ECS component document list so i can see all components and what they're for
- [_] clean up all ECS components that exist and find better ways to represent stuff (e.g. merge, split, rename).
- [_] in the engine, rename Anchored to Static, keep roblox shim as Anchored but refer to static when changing property
- [_] think plan for future features as well listed in roadmap and plan for them now
- [_] rename mono.unified_server_client to mono.unified_tests which imports all the mono reports into the code so it can fully test all features with all variations between clients, servers, cdn, networking, engine, etc. This one focused more on CROSS communication and management, NOT per-mono specific systems and such.
- [_] deferred catchup
- [_] improve build times (flamegraph => optimise)
- [_] mono.launcher (run singleplayer game, host game on server, join server, run studio, run cdn, etc)

- [_] optimise full particles (try reach 5 million rendering particles)
- [_] optimise physics (physics demo, -O0 and in studio)
- [_] optimise mirrors (try varying bounce levels to see bottlenecks in demo scene)

### v0.19

- [x] thoroughly implement every user-interface element, including `SurfaceGui` and `BillboardGui` - `SurfaceGui` gains `ZOffset`, `MaxDistance`, `ClipsDescendants` and `Active`, and `BillboardGui` gains `Active`, `Brightness`, `ClipsDescendants`, `CurrentDistance`, `DistanceStep`, `ExtentsOffsetWorldSpace`, `SizeOffset` and `PlayerToHideFrom`; new classes `UIGradient`, `UITableLayout`, `UIPageLayout` and `UIDragDetector`; `ScrollingFrame` completed with `ScrollingEnabled`, `AutomaticCanvasSize`, the two `ScrollBarInset`s, `VerticalScrollBarPosition`, `ElasticBehavior`, the three bar images and `AbsoluteCanvasSize`/`AbsoluteWindowSize`, plus wheel and thumb-drag input; `RichText`, `MaxVisibleGraphemes`, `ContentText`, `TextBounds` and `TextFits` on every text class; `Interactable`, the four `NextSelection*`, `SelectionOrder` and `SelectionImageObject` on `GuiObject`; `HoverImage`, `PressedImage` and `ResampleMode` on the image classes; `Enabled` and `ApplyStrokeMode` on `UIStroke`. Laid out, drawn by both backends, saved, replicated, bound and in the Properties panel. `D00120` carries the members that need a subsystem this engine has not got
- [_] build out all remaining roblox surfaces with available underlying surface
- [_] port many particle features from unity to here (https://docs.unity3d.com/6000.5/Documentation/ScriptReference/ParticleSystem.html)

### v0.20

- [_] finish portals so lighting, physics, projection, clipping and geometry crossing the seam are seamless
- [_] ensure per-mesh render capabilities, global lighting render capabilities, camera lighting render capabilities, etc. compute shaders, post-processing, etc.
- [_] simplify and strip old rendering code that is not part of the node system. Everything should be in the node system.
- [_] port semi-real raytrace and path-trace as part of nodes
- [_] make demo render pipelines with semi-real raytrace and path-trace
- [_] (dynamic) ambient occulusion, screen-space, fog, atmosphere, clouds, global illumination, displacement maps (make it rendering only but not physical)
- [_] render pipeline nodes for above
- [_] plan the entire rendering system to a visual compositor system like Unity. https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html

### v0.21

- [_] build out default plugins (move all topbar tools and stuff to plugins as a "Default Studio" plugin)
- [_] build out plugin function suite (create dropdown, edit toolbar, edit viewport, edit script editors, etc)
- [_] universe shared assets folder and setup easy cdn with it (when you load the universe file, it sets up a cdn with it).
- [_] add a universe loading widget - shows cdns the universe has and asks to allow permission, also http enabled property if changed
- [_] add tabs to the universe importer: general, assets, permissions, cdn, misc with all or per-world breakdown

### v0.22

- [_] default R6 base character (capsule collider)
- [_] gtlf default character (unreal)
- [_] make humanoid a shim for character controller (so not a black box), loads a default one in
- [_] character controller + humanoid + character states + state controller + bone controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.
- [_] animation handler
- [_] skinning and animation - `bake` skips joints and weights and keeps the rest pose, because there are no skeletons in the engine yet
- [_] add accessories support

### FUTURE

- [_] unity porting tools / unity shop
- [_] roblox porting tools (rbxl) - in the widget that pops up, show all asset ids and make a assets selector so you can click which asset id points to which file asset (same for animations and whatnot where possible).
- [_] (procedural, node-based) terrain generator (refer to discord references)
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) - untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds. Will show a widget that tells you conflicts and missing classes.
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms.
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
- [_] embedded whiteboxing tools (planning)
- [_] full procedural terrain studio tools
- [_] full ui features
- [_] level-of-details (4 different meshes version, auto-decimate version, smart-triangle-reduction-version thinking of nanite triangle surface area)
