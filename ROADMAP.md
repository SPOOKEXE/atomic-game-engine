
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

### v0.19

- [x] explore the idea of having the active scene entities resident on the gpu always and we just have a compute timer on the gpu 24/7. this way, when the scene changes, we tell the gpu what changed. also doing a 2-way sync is easy with signatures/hashes with cpu-gpu, this way we have no swapchain waiting, we just compute at a given interval. sort of like "replication" to the gpu. same for particles, ui, entites, studio, etc.
  - **Only transform and visual state is replicated to the GPU.** A part's `CFrame`, size, tint, transparency, mesh and material are what a frame draws, and they are the whole payload. Physics movers, constraints, controllers, humanoid state, scripts and anything else that *produces* a transform stay on the CPU and stay authoritative there - the GPU is given the result, never the mechanism. Two reasons and both are load-bearing: a simulation split across two processors has to be reconciled every frame, which is more traffic than it saves; and physics is what the server replicates to clients, so a GPU copy of it would be a second authority for state that already has one. Every item below that says "feed the GPU-resident system" means the visual half of that feature only.
  - What was explored and built at v0.19, so the rest is not re-derived:
    - `--frames-in-flight N` already exists on the client and the studio and already defaults to 1, which is the blocking submit the "no swapchain waiting" half of this item is about. It is a flag, not a rewrite - measure with it before building anything.
    - The instance row is quantised: `render::GpuInstance` is 36 bytes rather than 96 (float3 position, a unit quaternion as four snorm16, float3 scale, RGBA8 colour). Measured drift is 6.11e-5 metres per metre of radius, so a one-metre part moves six hundredths of a millimetre - about sixteen times inside a 0.001 tolerance. `render/src/InstancePacking.hpp` and its tests carry the arithmetic. This is what any GPU-resident row should be built on; a delta upload of a 96-byte row would have been paying three times over.
    - A temporary signer measured draw lists in blocks of 256 rows *in upload order*, with `FrameResult::InstanceChunksDirty` reporting how many changed. That ratio decided whether a delta upload was worth writing at all. It was deleted after choosing stable slots, because keeping two instance-change paths would make the measurement scaffold production machinery.
    - GPU pass timings now reach `FrameGraph` as `ProfileCategory::Gpu` reported spans, so a flamegraph shows device execution beside CPU command recording instead of showing only the latter.
  - **Measured, and the answer is: do not build a delta upload on the current row layout.** `--headless --frames 300` over the staged scenes, reading the client's new "instance chunk(s) rewritten" line:
    - Hallway 0.3%, Meshes 0.3%, Lighting 0.3%, Arena 0.7% - a still scene with a still camera dirties one chunk on the first frame and nothing after.
    - MeshGrid 100%, the default cube scene 100%.
    - MeshGrid's geometry is *not* moving. It grows its list on `Heartbeat` as meshes stream in, and that alone rewrites everything: a chunk is a range of row indices, so one instance appearing or leaving shifts every row behind it. The experiment signed the culled list, so a single part crossing the frustum edge did the same, and a camera in motion did it most frames.
    - So the split is not "static scene versus moving scene", it is "fixed membership versus changing membership", and real play is the second. A delta keyed on draw-order rows would save the whole upload in a screenshot and nothing at all in a game.
  - **What to build instead, and it is this item's own idea.** Key the resident rows by a stable per-entity slot the way `replication::Authority` keys a component, and make visibility and draw order a separate index list the GPU reads rather than a permutation the CPU bakes into the rows. Then culling and sorting stop touching the rows at all, a moving part dirties one slot, and the delta finally has something stable to be a delta against. The occlusion cull already does this in miniature when it compacts survivors into the late buffer by index. Do that before any delta protocol, not after.
  - **Built next at v0.19.** Each logical world now owns a resident packed-row pool keyed by world name, source entity and synthetic variant. Scene order, camera order and visibility upload as one `uint` per draw; only new or changed 36-byte rows cross again. Portal halves use the pane as their variant, cross-world rows carry the source world's name, and occlusion compacts resident indices instead of copying packed rows. `FrameResult::InstanceChunksDirty` now counts the chunks this delta actually touched.
  - **The wider visual-state path is built too.** Cameras of one world share those rows and only keep target-local index streams; `Renderer::Render` batches every world and pipeline group into one submission. Particles retain device-side state, parameter and curve tables, and generated vertices per logical world, so block indices from two worlds cannot collide and one camera advances a world's pool while its other cameras reuse it. `gui::Compiled::Signature` now reaches `InterfacePass`, which keeps unchanged vertices and indices resident while still resolving image dimensions and flipbook cells each frame. There is deliberately no second CPU/GPU authority or independent timer: the CPU remains authoritative for visual inputs, changed state is copied at the render boundary, and GPU particle simulation advances only by the caller's explicit frame delta.
  - **Measured again after stable slots, in release over 300 captured frames.** Hallway, Meshes, Lighting and Arena rewrite 0.3% of resident rows, the first frame only. MeshGrid falls from the old 100% whole-list rewrite to 35.9% while its content streams in; Particles rewrites 11.4%; the deliberately moving default scene remains 100%. The interface scenes upload geometry twice while images resolve and reuse it for the other 298 frames. Chunk occupancy is still reported beside rows because it says how scattered the copies are, while the row ratio says how many 36-byte payloads actually crossed.

### v0.20

- [x] thoroughly implement every user-interface element, including `SurfaceGui` and `BillboardGui` - `SurfaceGui` gains `ZOffset`, `MaxDistance`, `ClipsDescendants` and `Active`, and `BillboardGui` gains `Active`, `Brightness`, `ClipsDescendants`, `CurrentDistance`, `DistanceStep`, `ExtentsOffsetWorldSpace`, `SizeOffset` and `PlayerToHideFrom`; new classes `UIGradient`, `UITableLayout`, `UIPageLayout` and `UIDragDetector`; `ScrollingFrame` completed with `ScrollingEnabled`, `AutomaticCanvasSize`, the two `ScrollBarInset`s, `VerticalScrollBarPosition`, `ElasticBehavior`, the three bar images and `AbsoluteCanvasSize`/`AbsoluteWindowSize`, plus wheel and thumb-drag input; `RichText`, `MaxVisibleGraphemes`, `ContentText`, `TextBounds` and `TextFits` on every text class; `Interactable`, the four `NextSelection*`, `SelectionOrder` and `SelectionImageObject` on `GuiObject`; `HoverImage`, `PressedImage` and `ResampleMode` on the image classes; `Enabled` and `ApplyStrokeMode` on `UIStroke`. Laid out, drawn by both backends, saved, replicated, bound and in the Properties panel. `D00129` carries the members that need a subsystem this engine has not got (filed as `D00120`, renumbered at v0.17 - that number was already a retired entry)
- [_] build out all remaining roblox surfaces with available underlying surface - surface appearance is visual state, so it joins the GPU-resident instance row rather than being resolved per draw call on the CPU
- [_] port many particle features from unity to here (https://docs.unity3d.com/6000.5/Documentation/ScriptReference/ParticleSystem.html) - every new per-particle field lands in the GPU-resident set and is quantised like `ParticleInstance` already is, not added as a float4; particles are already stepped by `particle-step.comp` and are the subsystem furthest along this road
- [_] `~/Documents/GitHub/BLADEBORNE_UNIFIED/game` port and also studio place `~/Documents/Bladeborne Floor 0.rbxl`. Turn this into a demo file.

### v0.21

- [_] find a way to (easily) and thoroughly test rendering steps and ensure they produce the right image with right projections
- [_] finish portals so lighting, physics, projection, clipping and geometry crossing the seam are seamless
- [_] ensure per-mesh render capabilities, global lighting render capabilities, camera lighting render capabilities, etc. compute shaders, post-processing, etc. - per-mesh capability flags are per-instance visual state and belong in the GPU-resident row, so a compute pass can branch on them without a CPU readback
- [_] simplify and strip old rendering code that is not part of the node system. Everything should be in the node system. - the residency and delta upload are a node too, so the sweep and the GPU-resident work are the same refactor rather than two passes over the same files
- [_] port semi-real raytrace and path-trace as part of nodes
- [_] make demo render pipelines with semi-real raytrace and path-trace
- [_] (dynamic) ambient occulusion, screen-space, fog, atmosphere, clouds, global illumination, displacement maps (make it rendering only but not physical) - "rendering only but not physical" is exactly the transform/visual split the GPU-resident set draws, so all of this is GPU-side state with no CPU mirror to keep in step
- [_] render pipeline nodes for above
- [_] plan the entire rendering system to a visual compositor system like Unity. https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html
- [_] viewport indictator direction gizmo (select and lock to certain directions)
- [_] 3d cursor and camera orbit options under gizmo
- [_] ensure full parallel/vectorised (i.e. get all active scenes => build entity list => update gpu resident => batch render all cameras in every scene) - stable entity slots, per-world particle pools and batched camera submission are built. The remaining work is the product-side active-scene collector and parallel presentation walk; every camera can already read its world's buffers without re-uploading them.

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
- [_] skinning and animation - `bake` skips joints and weights and keeps the rest pose, because there are no skeletons in the engine yet - joint palettes are visual transform state and go GPU-resident beside the instance rows; the animation *controller* that produces them stays on the CPU, per the split above
- [_] add accessories support

### FUTURE

- [_] unity porting tools / unity shop
- [_] roblox porting tools (rbxl) - in the widget that pops up, show all asset ids and make a assets selector so you can click which asset id points to which file asset (same for animations and whatnot where possible).
- [_] (procedural, node-based) terrain generator (refer to discord references) - generated chunks feed the GPU-resident set as ordinary visual state; the collision mesh they also produce stays CPU-side, per the split above
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) - untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds. Will show a widget that tells you conflicts and missing classes.
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms.
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
- [_] embedded whiteboxing tools (planning)
- [_] full procedural terrain studio tools
- [_] full ui features
- [_] level-of-details (4 different meshes version, auto-decimate version, smart-triangle-reduction-version thinking of nanite triangle surface area) - LOD selection is a per-instance visual decision and belongs in the GPU-resident set beside the occlusion cull that already runs there, so a level change costs no CPU round trip
- [_] project demos: space engineers asteroids + planets full demo, blackhole simulator (warp space, warp visual, etc), huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot, user interface (copy bladeborne's for demo?)
- [_] datastores (sqlite, mongo, supabase, etc - make a selection with local and remote setups)
- [_] html-based ui creation (html-script?)
- [_] import blender files in asset explorer
- [_] concept idea: setup a public mcp repository in python, add .mcp.json in project folder that loads it, it watches forums channels in the discord server for new/existing bugs. agent writes a message in the channel stating you're fixing it, other agents work on other bugs. agents can write that "this bug is a big rewrite" in the channel too which could be helpful.
- [_] smart platform backend where only "admitted keys" can connect to a given server - i.e. whitelist-based servers (press play on website => generate play key => platform tells server user is connecting with key => send key + server to user => user connects to server using key and info => join)
- [_] rpg maker port tool
