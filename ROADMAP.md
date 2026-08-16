
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

### v0.16

- [_] do a stress test with 200 (headless) clients connecting to the single server and optimise everything where possible. Always produce a baseline\_flamegraph.ext, then every iteration make a iter\(N\)\_flamegraph.ext so you can compare as you develop.
- [_] setup a CDN mode that is:  1. server distributes from cdn (default) - when clients connect, the server itself connects to the cdn and streams assets as needed to the client (client has no authority for it except rate-limited retries only to the server). 2. server reroutes client - when client connects to server, it tells the clients where all the cdns are that are configured on the server
- [_] thoroughly implement all user interface elements + surfacegui + billboardgui
- [_] tutorial for setting up CDN in folder and as a server
- [_] tutorial for setting up a server with custom cdn
- [_] tutorial adding cdn to studio
- [_] tutorial for launching client to connect to a server (note: the server tells the client where the CDN is and the key needed, add a alternative mode in which the)

### v0.17

- [_] ensure we can attach shaders to (visual) assets as well to change how they render: mesh, texture, gifs, images and videos
- [_] add accessories support
- [_] pbr rendering support
- [_] animation handler
- [_] character controller + humanoid + character states + state controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.
- [_] skinning and animation - `bake` skips joints and weights and keeps the rest pose, because there are no skeletons in the engine yet
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) - untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds

### v0.18

- [_] finish portals, lighting doesnt work through it properly and in the seam the character half disappears because geometry doesn't render completey render through them properly. also fix all projections and find a way to make it completely seamless; lighting, physics, rendering, objects inbetween the seam, etc

### v0.?? (needs prototype project first)

- [x] ~/Documents/GitHub/node-graph-template
- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic)
- [_] rendering pipeline is a node system with a studio editor
- [_] surfaceapperance actually integrated with new pipeline
- [_] render pipeline is per world, not per process
- [_] render pipelines are saved in the world's export data
- [_] rendering pipeline debugger and profiler - shows a graph's per-step operations with their compute costs and wall clock, etc. also shows the images/masks/etc used for each step
- [_] rendering pipeline additions; ambient occulusion, emissive, pbr, default node setup with all these
- [_] https://www.youtube.com/watch?v=SnNm7rSSvlg (Threat Interactive Tutorial: How To Optimize Almost Every Step In Modern Game Rendering)
- [_] https://github.com/fini03/vkDuck

---

- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms.
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system

