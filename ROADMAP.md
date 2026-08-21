
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

### v0.18

- [x] add release builds for linux (windows and mac, if builds)
- [x] in the engine, replace Anchored for a better component. Keep roblox shim as Anchored but refer to the new property. Update: "Simulated" property.
- [x] expose spatial querying to luau / quickjs
- [x] fix ui scaling and scene camera viewport issue (image)
- [x] entire game locks when meshes are loading (should be async and non-locking)
- [x] build interface is 0.2ms still? see if we can improve
- [x] when you delete a mirror (surfacecamera), it does not clean up the image it was displaying so it becomes a ghost image
- [x] content.deliver is 0.038ms even though no assets are changing? we should be caching these assets and whatnot
- [x] in Renderer::RenderView there are a bunch of big gaps
- [x] fix renderer stuff with gpu sync
- [x] proper fps management with millis
- [x] magic demo micro-optimisations with stalling replication (heaps better)
- [x] fix runaway memory allocation
- [_] add heap profiler for dev builds and in flamegraph with graph visualisation
- [_] make the heap profiler super granular throughout code
- [_] add a automatic test/benchmark that picks up long-term runaway allocations via heap profiler (simulate 5 minutes of running different scenes).
- [_] mono.launcher (run singleplayer game, host game on server, join server, run studio, run cdn, etc). [launcher opens] => select mode => different options per each (e.g. cdn - select folder, ip, lan-only, remote-mode, etc). Plan and ask user questions first.

- [_] rename mono.unified_server_client to mono.unified_tests which imports all the mono reports into the code so it can fully test all features with all variations between clients, servers, cdn, networking, engine, etc. This one focused more on CROSS communication and management, NOT per-mono specific systems and such.
- [_] add heap profiler to mono.unified_server_client / mono.unified_tests

- [_] ensure we benchmark critical systems (network, multiplayer, physics, particles, etc)

---

### v0.19

- [_] build out proper code architecture documents (AGENTS.md, docs/CODE_FORMAT.md, docs/CODE_QUALITY.md) => CODE_ARCH.md.
- [_] DOMAIN DRIVEN DESIGN & HEXAGONAL ARCHITECTURE.
- [_] check if we need to move files / classes / structures around in the codebase to properly fit (mainly focus on engine).
- [_] properly make a ECS component document list so i can see all components and what they're for.
- [_] clean up all ECS components that exist and find better ways to represent stuff (e.g. merge, split, rename).
- [_] think plan for future features as well listed in roadmap and plan for them now.
- [_] improve build times (flamegraph => optimise).
- [_] quic implementation.
- [_] choose networking backend (quic, tcp, udp, etc) for each engine feature like replication and whatnot.
- [_] build out a full logging, metrics, etc so we can track what the engine is doing in dev builds effectively.
- [_] add more MCP integrations in engine for models to use.

### v0.20

- [x] thoroughly implement every user-interface element, including `SurfaceGui` and `BillboardGui` - `SurfaceGui` gains `ZOffset`, `MaxDistance`, `ClipsDescendants` and `Active`, and `BillboardGui` gains `Active`, `Brightness`, `ClipsDescendants`, `CurrentDistance`, `DistanceStep`, `ExtentsOffsetWorldSpace`, `SizeOffset` and `PlayerToHideFrom`; new classes `UIGradient`, `UITableLayout`, `UIPageLayout` and `UIDragDetector`; `ScrollingFrame` completed with `ScrollingEnabled`, `AutomaticCanvasSize`, the two `ScrollBarInset`s, `VerticalScrollBarPosition`, `ElasticBehavior`, the three bar images and `AbsoluteCanvasSize`/`AbsoluteWindowSize`, plus wheel and thumb-drag input; `RichText`, `MaxVisibleGraphemes`, `ContentText`, `TextBounds` and `TextFits` on every text class; `Interactable`, the four `NextSelection*`, `SelectionOrder` and `SelectionImageObject` on `GuiObject`; `HoverImage`, `PressedImage` and `ResampleMode` on the image classes; `Enabled` and `ApplyStrokeMode` on `UIStroke`. Laid out, drawn by both backends, saved, replicated, bound and in the Properties panel. `D00129` carries the members that need a subsystem this engine has not got (filed as `D00120`, renumbered at v0.17 - that number was already a retired entry)
- [_] build out all remaining roblox surfaces with available underlying surface
- [_] port many particle features from unity to here (https://docs.unity3d.com/6000.5/Documentation/ScriptReference/ParticleSystem.html)

### v0.21

- [_] finish portals so lighting, physics, projection, clipping and geometry crossing the seam are seamless
- [_] ensure per-mesh render capabilities, global lighting render capabilities, camera lighting render capabilities, etc. compute shaders, post-processing, etc.
- [_] simplify and strip old rendering code that is not part of the node system. Everything should be in the node system.
- [_] port semi-real raytrace and path-trace as part of nodes
- [_] make demo render pipelines with semi-real raytrace and path-trace
- [_] (dynamic) ambient occulusion, screen-space, fog, atmosphere, clouds, global illumination, displacement maps (make it rendering only but not physical)
- [_] render pipeline nodes for above
- [_] plan the entire rendering system to a visual compositor system like Unity. https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html

### v0.22

- [_] build out default plugins (move all topbar tools and stuff to plugins as a "Default Studio" plugin)
- [_] build out plugin function suite (create dropdown, edit toolbar, edit viewport, edit script editors, etc)
- [_] universe shared assets folder and setup easy cdn with it (when you load the universe file, it sets up a cdn with it).
- [_] add a universe loading widget - shows cdns the universe has and asks to allow permission, also http enabled property if changed
- [_] add tabs to the universe importer: general, assets, permissions, cdn, misc with all or per-world breakdown

### v0.23

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
- [_] project demos: space engineers asteroids + planets full demo, blackhole simulator (warp space, warp visual, etc), huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot, user interface (copy bladeborne's for demo?)
- [_] datastores (sqlite, mongo, supabase, etc - make a selection with local and remote setups)
