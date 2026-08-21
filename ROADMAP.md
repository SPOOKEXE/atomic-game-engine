
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
- [x] add heap profiler for dev builds and in flamegraph with graph visualisation. `core::HeapProfile` at L0 - a replacement `operator new`/`delete` pair behind `MONO_HEAP_PROFILE`, on in every preset but `release`. Each block carries a 24-byte header naming the tag tree node it was charged to, so a free is a pointer subtraction and two atomic decrements rather than a locked map lookup. Live bytes fold into the same `root;child;leaf <value>` format `FrameGraph` already writes for time, so one flamegraph renderer draws both. The F5 overlay gains a `heap` tab - live bytes over time as a plot, then the tag tree - and the editor gains a `Heap` panel beside its frame graph. `--heap-report PATH` on the client, server and editor writes the totals, the heaviest tags and a least-squares growth fit per tag. It found and closed two bounded-but-unchecked climbs on its first run: `FrameGraph`'s retained window kept a `std::vector` per retained frame, so twenty thousand heap blocks grew towards twenty thousand times the busiest frame ever recorded - a headless client reached 10 MiB across 20,249 blocks and was still climbing after forty seconds, for a panel nobody had open - and `render::FrameStatistics` held 2 MiB of frame times at an uncapped frame rate. One shared reading ring (`MAXIMUM_HISTORY_READINGS`, 2 MiB allocated once) and one capped sample ring (`MAXIMUM_SAMPLES`) later, the same scene measures **15.90 MiB in 12,117 live blocks growing 1.7 KiB/s at a fit of 0.13, against 25.07 MiB in 32,121 blocks growing 957 KiB/s**
- [x] make the heap profiler super granular throughout code. `ENGINE_PROFILE` opens a heap tag as well as a Tracy zone and a `FrameGraph` scope, so every scope already placed where the work is became a tag - about 130 distinct tag paths in a client run, with no second family of macros to keep in step. The dynamic form tags with its fallback literal rather than its runtime name, because a tag tree node is never removed. `ENGINE_HEAP_SCOPE` covers what allocates and is not worth timing: the job pool's worker threads, SDL's audio thread, and each phase of the client's frame
- [x] add a automatic test/benchmark that picks up long-term runaway allocations via heap profiler (simulate 5 minutes of running different scenes). `just heap-soak` runs five scenes for a minute each - a minute of wall clock is a quarter of a million headless frames and a real 60 Hz of ticks - and the client exits 3 naming any tag whose live bytes both climb faster than the limit and fit a line, `HeapProfile::RUNAWAY_FIT`. The fit is what separates a leak from a level load: a step has a slope and does not fit one. Not in `just check`, for `client-smoke`'s reason - it needs a GPU
- [x] mono.launcher: a window with Play, Join, Host, Studio and Serve content. Each mode is a form over every option and setting its program declared, read from the program itself via a new `--describe` on `core::Arguments` rather than transcribed. Spawns the staged binary through `parallel::Capture`/`Process` and links none of them. Host and cdn stay supervised; play, join and studio hand over.
- [x] rename mono.unified_server_client to mono.unified_tests which imports all the mono reports into the code so it can fully test all features with all variations between clients, servers, cdn, networking, engine, etc. This one focused more on CROSS communication and management, NOT per-mono specific systems and such. The module was one arrangement of two halves; it is now the cross product of three axes - `Transport{direct, loopback, lossy}`, `Content{none, relayed}`, `Discovery{none, advertised}` - and `unified::Harness` is `Arrangement{}`, so the twelve existing cases about the bisection kept passing through the refactor unchanged. `unified/Reports.hpp` **imports every module's report type rather than re-declaring its fields**: a `server::ContentRelayStatistics` here is the one `mono.server` defines, so a field renamed on the other side breaks this build where a hand-copied mirror would keep reporting a number nothing produces. What that buys is `CrossCheck` - the claims that span two modules, which is precisely the set no module's own suite can make, because the tier system stops `mono.server` linking `mono.client`, stops `Engine::replication` linking either, and is right to. Every contradiction names two modules and moving one that names only one is the entry rule. The four-module flow is the one worth having: `cdn::Publish` writes a store, `delivery::MakeRouteFetcher` walks it, `server::ContentRelay` rations routes onto the link, `client::ContentLink` reassembles - each with a suite of its own and none able to check the seam to the next. It caught its own first bug on the way in: the relay's rate limiter counts a refused ask under `Dropped` rather than `Requests`, so "client asked 22, server saw 21" read as a lost message and was a token bucket working, and the fix made the invariant stronger - `Requests + Dropped` must equal what the client asked. Content over a wire crosses wrapped in a `replication::User` message, because `Session::Send` refuses a payload it cannot name a channel for, which is what `Connector::SendUser` exists to do. Thirty-two cases across `unified.arrangement`, `unified.crossing` and `unified.reports`; the last of those feeds `CrossCheck` reports built to contain a known disagreement, because a checker that has only ever seen correct input is a checker nobody has tested
- [x] add heap profiler to mono.unified_tests. `Crossing::Step` opens a tag per stage - `unified.server.simulate`, `unified.server.publish`, `unified.carry.direct`/`unified.carry.wire`, `unified.content`, `unified.discovery`, `unified.client.record`, `unified.client.draw` - and a case asserts those tags exist after a run, so a stage that stops pushing one is a failing test rather than a future soak quietly blaming `untagged`. `--heap-report`, `--heap-growth-limit` and `--heap-warmup` match the client's, and `--seconds` is the one place this program reads a clock: a slope is bytes per *second* and cannot be fitted to a tick count, so the exception is stated rather than smuggled in. `just unified-soak` runs **one process per arrangement** - twelve histories rather than one history with twelve workloads in it, since a slope fitted across the changeovers is fitted across nothing. All twelve reach a steady state; the heaviest, `lossy+relayed+advertised` at 32 entities, holds 0.3 MiB live in 3,045 blocks and does not climb at 1 B/s. The suite carries the same check at unit speed: two crossings built and destroyed, the first as the warm-up, and what the second does not give back is what it kept

- [_] ensure we benchmark critical systems (parallel, workers, async files, network, cdn, mesh send/receive, gpu alloc, submit and management, , multiplayer, physics, particles, etc) thoroughly (stress tests)

---

### v0.19

- [_] build out proper code architecture documents (AGENTS.md, docs/CODE_FORMAT.md, docs/CODE_QUALITY.md) => CODE_ARCH.md.
- [_] DOMAIN DRIVEN DESIGN & HEXAGONAL ARCHITECTURE.
- [_] check if we need to move files / classes / structures around in the codebase to properly fit (mainly focus on engine).
- [_] properly make a ECS component document list so i can see all components and what they're for.
- [_] clean up all ECS components that exist and find better ways to represent stuff (e.g. merge, split, rename).
- [_] think plan for future features as well listed in roadmap and plan for them now.
- [_] improve build times (flamegraph => optimise).
- [_] check all asynchronous points, all parallel points, etc and ensure they are implemented nicely and are good
- [_] scan through all seriel loops and see if we can improve any with parallel / vectorised
- [_] update AGENTS.md in root and subdirectories
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
