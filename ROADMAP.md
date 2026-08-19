
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
- [_] thoroughly implement every user-interface element, including `SurfaceGui` and `BillboardGui`

- [_] add accessories support
- [_] animation handler
- [_] character controller + humanoid + character states + state controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.
- [_] skinning and animation - `bake` skips joints and weights and keeps the rest pose, because there are no skeletons in the engine yet
- [_] finish portals so lighting, physics, projection, clipping and geometry crossing the seam are seamless
- [_] port semi-real raytrace and path-trace as part of nodes
- [_] make demo render pipelines with semi-real raytrace and path-trace
- [_] render pipeline nodes for above

### FUTURE

- [_] (procedural, node-based) terrain generator (refer to discord references)
- [_] plan the entire rendering system to a visual compositor system like Unity. https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html
- [_] ensure per-mesh render capabilities, global lighting render capabilities, camera lighting render capabilities, etc. compute shaders, post-processing, etc.
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) - untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds. Will show a widget that tells you conflicts and missing classes.
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms.
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
