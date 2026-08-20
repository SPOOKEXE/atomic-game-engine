
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

- [~] a few of the portal scenes (in all the various worlds) have z-index fighting issues - **not reproduced, and the measurement is worth more than another guess.** `scripts/demos/zfight-scan.sh` photographs a scene twice from cameras a thousandth of a stud apart and reports how much of the frame changed *through* rather than at an edge - the erosion matters, because a sub-pixel shift outlines every silhouette in a room made of tiled walls and `Portals-1-world` reads 1.26% that way with not one pixel of it a fight. Eroded, all fourteen portal and mirror scenes read 0.00%. **And the control says that is a real zero rather than a blind one**: two slabs with their top faces at exactly y = 0 come out a uniform blue in this engine, one winning every pixel, with no stripe at any distance - the depth tie resolves the same way every frame and the draw order does not depend on the projection, so coplanar geometry here is *stably* wrong rather than noisily wrong. That rules out the classic reading of the report and leaves the ones a still cannot show: a sorted transparent run changing order, or 2D `ZIndex` between overlapping `ScreenGui` elements. Needs a scene name and roughly where the eye was. **And one new lead, measured rather than guessed.** `zfight-scan.sh` nudges by a thousandth of a stud, so it can only catch a flip that turns on an *infinitesimal* change of viewpoint - a depth tie, a sort comparator. It is structurally blind to anything that flips when the eye crosses a *finite* threshold: a half-space sign, a distance cut, a nearest-N selection. The PortalLightMix line below was one of those and `seam-spill-sweep.sh` had to be written to see it. There is a second in the same family still open: the renderer projects at most `MAX_SEAM_LIGHTS` = 2 doorway light fields and picks them by distance to the eye, and `Tunnels.luau` has six mouths. Walking it from x = -15 to x = 0 swaps the winning pair from mouths 0 and 1 to mouths 2 and 4 - two pools of projected light vanish and two others appear in one frame, at full strength, with nothing between. That is a viewpoint-dependent unblended flip in a portal scene, which is what the report says, and it wants a decision rather than a fix: either a crossfade at the budget boundary or a bigger budget, and both cost a texture bind. **And the scenes themselves had it, which no camera nudge could ever have shown.** Surveyed every example headlessly for slabs whose top face sits at exactly y = 0: forty-four of them, across thirty scenes, every single one flush with a template baseplate. Six of those overlapped *another* floor at the same height inside one scene - `Hallway` and `Tunnels` each lay a 200x200 ground and then set corridor floors down inside its footprint at the same height, and both are walked through holes. Every scene floor now clears zero by `GROUND_LIFT` = 0.01, and a corridor shell clears its own ground by the same again. The shells move whole rather than floor-first: their panes fill the cross-section exactly, so lifting a floor alone opens a crack along the bottom of every pane, which is the lip the shell build exists to avoid - a test pinning the absolute heights caught that and now asserts the pane against the floor and the ceiling instead. **`NonEuclidean` keeps two pairs and they are not a floor-height problem**: its hidden chambers all sit on one plane at z = -500 and each stage spreads its own wider than the 44 between stages, so `House3` [109,127] overlaps `Turn1` [102,118] and `Turn2` [146,162] overlaps `Quarter1` [150,190] - two rooms in one place, which a uniform lift moves together and cannot separate. Giving each stage its own hidden depth is the fix and it changes what every exhibit's portal frames, so it wants a look rather than a patch
- [x] PortalLightMix, when you're on the side view of the mirror, you move left the light shows up only on left side, you move right left side hides and right sides hows up, lighting culling with portals - **found, and it was neither of the two mechanisms the first pass ruled out.** Those were both in `client::CollectLights`, and both really are viewer-free: the cut to `render::MAX_SCENE_LIGHTS` needs more than sixteen lights and this scene has four, and the seam copy is gated on `SeamDistance(seam, lamp) < lamp.Range`, which mentions the viewer nowhere. There is a *second* light transport through a portal and it is in the renderer: each mouth captures its far room into a 128x128 probe and `deferred-lighting.frag`'s `SeamSpill` projects that texture back out of the doorway. Which half-space it lands in was taken from `cameraFrame.Position` - two lines copied from `subCameraFor`, where they are correct because that sub-camera genuinely is placed from the eye. Here the eye is a stand-in built out of `outward` itself, so a light probe moved when a player walked, and `SeamSpill`'s `depth <= 0.0` test dropped the whole pool on one side of the pane and painted it on the other the frame the eye crossed the plane. The block's own comment already said "viewer-independent"; the code disagreed with it. Now `outward = portal.Normal`, which is a fact about the doorway. `scripts/demos/seam-spill-sweep.sh` is the instrument: walking the eye across the near pane in even half-stud steps, the old code moves 9,195 lit pixels at the crossing against a median of 616 - 14.9x - and the new code has no step at all. The same paste had a quieter half: a mouth nowhere near the viewer took its side from the viewer too, so the far room's own doorway projected into the void behind itself for as long as anybody stood in the near room

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

- [_] optimise full particles (try reach 5 million rendering particles) - **and `particles.spawn` is where the next of it is.** Profiled with `client --profile-snapshot --force-serial-compute` against `StressParticles.luau` (1,024 hosts, 5,120 emitters, 512,000 particles), `dev`, per tick: `ecs.systems` 50.4, of which `step particles` 45.7 - `particles.age` 39.0 and `particles.spawn` 7.0 - and `refresh emitters` 4.6. The age half came down 37% by hoisting the block's own terms and resolving one curve cursor for four curves; the spawn half did not move the same way, and the measurements say why. Setting every rate to zero drops `particles.spawn` to 0.145 ms, so the archetype walk is not the cost - it is the body, at about 4 µs for each of the ~1,700 particles born per tick. Setting `SpreadAngle` to zero drops it from 7.1 to 5.3, so a quarter of it is the two random draws and the `CFrame::Angles` the spread cone needs. What is left is roughly seven `Unit` draws, two square roots and two frame transforms per particle, every one of them an out-of-line call in a build that inlines nothing. Three safe passes are already in - the per-emitter constants lifted out of the per-particle loop, `MagnitudeSquared` where the guard only asks whether a vector is non-zero rather than how long it is, and the spread rotation applied with `VectorToWorldSpace` instead of composing and discarding a second `CFrame` - and together they are about 4%. Past that it wants the structural version this line is for: a per-emitter spawn plan resolved once so the shape switch, the emission normal and the spread angles leave the inner loop entirely, and an RNG that does not cost a call per draw. **Deliberately not ticked** - the Unity-side system recreation lands against this line too
- [_] optimise physics (physics demo, -O0 and in studio)
- [_] optimise mirrors (try varying bounce levels to see bottlenecks in demo scene)

- [_] quic implementation

### v0.19

- [x] thoroughly implement every user-interface element, including `SurfaceGui` and `BillboardGui` - `SurfaceGui` gains `ZOffset`, `MaxDistance`, `ClipsDescendants` and `Active`, and `BillboardGui` gains `Active`, `Brightness`, `ClipsDescendants`, `CurrentDistance`, `DistanceStep`, `ExtentsOffsetWorldSpace`, `SizeOffset` and `PlayerToHideFrom`; new classes `UIGradient`, `UITableLayout`, `UIPageLayout` and `UIDragDetector`; `ScrollingFrame` completed with `ScrollingEnabled`, `AutomaticCanvasSize`, the two `ScrollBarInset`s, `VerticalScrollBarPosition`, `ElasticBehavior`, the three bar images and `AbsoluteCanvasSize`/`AbsoluteWindowSize`, plus wheel and thumb-drag input; `RichText`, `MaxVisibleGraphemes`, `ContentText`, `TextBounds` and `TextFits` on every text class; `Interactable`, the four `NextSelection*`, `SelectionOrder` and `SelectionImageObject` on `GuiObject`; `HoverImage`, `PressedImage` and `ResampleMode` on the image classes; `Enabled` and `ApplyStrokeMode` on `UIStroke`. Laid out, drawn by both backends, saved, replicated, bound and in the Properties panel. `D00129` carries the members that need a subsystem this engine has not got (filed as `D00120`, renumbered at v0.17 - that number was already a retired entry)
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
- [_] project demos: space engineers asteroids + planets full demo, blackhole simulator (warp space, warp visual, etc), huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot
