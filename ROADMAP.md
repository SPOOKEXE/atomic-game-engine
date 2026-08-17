
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

- [x] do a stress test with 200 (headless) clients connecting to the single server and optimise everything where possible. Always produce a baseline\_flamegraph.ext, then every iteration make a iter\(N\)\_flamegraph.ext so you can compare as you develop. `mono.tools/loadtest`, `just stress`, `scripts/flamegraph.py`, and five iterations in `.cache/stress/RESULTS.md` - p95 tick 1251 ms to 482 ms, clients joined 55 to 180.
- [x] the two hot frames `.cache/stress/RESULTS.md` leaves: the occlusion raycast in the priority score, and `Authority::BuildComponents`. Five iterations in `.cache/stress/RESULTS.md` - a walk along the ray in `spatial` (Amanatides and Woo, 10.67 ms to 12.90 us on the benchmark's fine-cell row), `Authority::SetPriorityRefinement` so occlusion is asked only about the rows in contention, `Client::Unconfirmed` as a sorted vector, and a bounded rotating recovery walk. p95 tick 416 ms to 280 ms, throughput to clients 6.6 MB/s to 12.3 MB/s, 200 of 200 clients joined.
- [x] `ShapeCast` walks the thick line of a long sweep and de-duplicates from the first centre-cell neighbourhood shared with each proxy. A measured crossover keeps the volume walk when its small envelope opens fewer cells.
- [x] setup a CDN mode that is:  1. server distributes from cdn (default) - when clients connect, the server itself connects to the cdn and streams assets as needed to the client (client has no authority for it except rate-limited retries only to the server). 2. server reroutes client - when client connects to server, it tells the clients where all the cdns are that are configured on the server
- [x] concise `RUNNING.md` tutorial for publishing a CDN store and serving it from a folder or HTTP origin
- [x] concise `RUNNING.md` tutorial for pointing a game server at a custom CDN
- [x] concise `RUNNING.md` tutorial for adding a local store or remote origin to Studio
- [x] concise `RUNNING.md` tutorial for connecting a client using the server-provided CDN, grant and publisher key, with an alternative mode that pins the client's own CDN and key
- [x] universe properties in explorer: selecting the universe root exposes its name, execution mode, catch-up cap, bus budget and world count in Properties, with authored tuning saved in the game file
- [x] all user interface elements render in the client and Studio, including interactive ScreenGui, lit and depth-tested SurfaceGui and BillboardGui collectors, and nested ViewportFrame scenes
- [x] portals use recursive off-axis projections, seam-clipped and cloned geometry, through-seam lighting and effects, and paired physics proxies and crossings so bodies and objects remain continuous at the opening
- [x] portal mouths use per-view void-side captures: each destination renders without its portal panes, keeps scene and through-portal lighting, then composites nested captures back onto the exact opening. `Portal.Enabled` disables capture, lighting, seam geometry, crossing and pane opening while retaining the authored link.
- [x] audited the declared property surface against runtime consumers: Lighting now drives per-world sun, ambient, outdoor light, fog, recursive portal views and particles; service fixture identity is read-only; light controls are class-specific; and unsupported GUI, local-shadow and selection-fill controls are no longer advertised as working properties

### v0.17

- [_] ensure we can attach shaders to (visual) assets as well to change how they render: mesh, texture, gifs, images and videos
- [_] add accessories support
- [_] pbr rendering support
- [_] animation handler
- [_] character controller + humanoid + character states + state controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.
- [_] skinning and animation - `bake` skips joints and weights and keeps the rest pose, because there are no skeletons in the engine yet
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) - untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds

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
