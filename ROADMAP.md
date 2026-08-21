
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
- [x] ensure we benchmark critical systems (parallel, workers, async files, network, cdn, mesh send/receive, gpu alloc, submit and management, , multiplayer, physics, particles, etc) thoroughly (stress tests). Seven new suites over the paths that had none, chosen by auditing what was already covered rather than by writing a file per module: `net`, `replication`, `physics`, `ecs`, `assets`, `effects`, `spatial`, `scene`, `core`, `audio`, `gui`, `input`, `script`, `world`, `delivery`, `game` and `studio` already had real suites and were left alone. **`engine.parallel.bench.contention`** puts the same million rows of *real* work through every arrangement, where `dispatch` measured an empty `For` and therefore only the floor: serial 2.98 ms against 260 us on twenty-three workers, the grain sweep confirming 4096 (256 is a quarter worse and 65536 a fifth), and an imbalanced body at 1.21 ms because `For` splits by index count and cannot know what an index costs. The row worth keeping is the last: **the same million rows as sixty-four dispatches of sixteen thousand lands exactly on the serial figure, because sixteen thousand is under `DEFAULT_GRAIN * MINIMUM_GRAINS` and every one of the sixty-four calls ran inline** - a system that batches per chunk instead of per tick gets no pool at all and nothing reports it. **`engine.parallel.bench.channel`** covers the framed queue including the refusal paths a host in trouble actually runs: 69 ns for a 64-byte round trip, 10 ns to be refused by a full channel, 1 ns to refuse an oversized frame, and 605 ns per 4 KiB frame across two threads against 135 ns uncontended. **`network.bench.discovery`** found the one defect in the set: a tagged advert costs 384 ns against the key that verifies and **20.1 us against a ring of sixty-four that do not - a 52x amplification an attacker gets for free by broadcasting noise at a browser**, because `Decode` must try every key it holds. Public adverts are unaffected (73 ns whatever the ring size), rubbish is refused in 3 ns and a truncated advert in 1 ns. **`cdn.bench.grouping`** and **`cdn.bench.admission`** cover a module that had nothing: grouping is n log n (1k 101 us, 10k 1.57 ms, 50k 8.64 ms), and an admission costs what its *token* costs to verify rather than what the decision costs - 869 ns at two bundles, 22.2 us at a thousand - which makes grant width a budget on the server rather than something to optimise at the origin. A forged token costs the same as an accepted one, which is the right answer: a forgery is only detectable by verifying. **`engine.render.bench.meshes`** covers the host half of mesh admission and the deferred free list, and found that **admission is superlinear in the bytes but not in the meshes**: ten times the 256-vertex meshes costs fifty-seven times as much (691 us to 39.2 ms) while ten times the 32-vertex meshes costs eleven (157 us to 1.79 ms), so the per-mesh bookkeeping is flat and eighty megabytes of host vertices grown by `resize` is what costs - a table told its size up front would not pay it. Reclaim works (456 us replacing in place with flushes against 1.46 ms with the clock stopped) and a fragmented free list costs nothing extra. **`engine.graph.bench.submission`** covers frame planning, which is the GPU submission work measurable without a device, and it is linear on all three axes - `CompileSchedule` 10.1 us at eight passes to 76.5 us at sixty-four, `PlanFrame` 3.3 us at one view to 38.0 us at sixteen. **`engine.assets.bench.store`** puts the disk back into a module whose suite had only ever measured buffers: a 64 KiB chunk costs 36.7 us to write against 9.9 us for the same bytes inside one file, so **the per-chunk file tax is 27 us and dedup is what pays for it** (re-writing a chunk already held is 7.2 us). There is no asynchronous file path anywhere in `assets`, `delivery`, `mono.client` or `mono.server` - nothing reads a file off the calling thread - and the suite says so beside the numbers that would justify one. **`unified.bench.crossing`** is the multiplayer row nothing else could produce, because a tick spans every module and no module links enough of the others to run one: 52.4 us direct at 64 entities against 68.0 us over a real `net` link, so **the wire is 15.6 us of a tick**, while content and discovery each land inside the measurement's own noise - and all three together are 81.4 us, which is 13 us more than the lossy link alone and therefore an interaction rather than a sum. It also found a latent teardown hazard on the way in: `Crossing`'s destructor stops the job pool, and a crossing held in a static constructed *before* the pool's own state is destroyed after it, so the `Stop` joins workers whose pool is gone and the process hangs at exit with every row already printed. Touching the pool before the static orders it correctly, and the same trap is open to any program holding a job-using object in a static. **A second pass covered textures, interface rendering, physics stepping and the client's own frame.** `engine.assets.bench.textures` puts numbers on the path a game's gigabytes take: a mip chain grows about with its pixels (512 898 us, 1024 5.10 ms, 2048 25.4 ms), an `R8` mask costs a quarter of an `RGBA8` image of the same dimensions so the filter tracks bytes rather than pixels, and the refusal rows confirm the header check is a comparison rather than an allocation - a wrong magic is 2 ns, a truncated file 22 ns, and a header claiming 16384 on a side with no pixels behind it 16 ns, five orders of magnitude under the read it is pretending to be. **It also found a live one: `ResizeImage` has no early-out for a target equal to its source, so resampling a 2048x2048 image to 2048x2048 costs 32.4 ms** - more than building that image's entire mip chain and six times what resizing it down costs - and `bakegraph`'s `Resize` node passes an authored target size, so a pipeline that normalises every texture pays it for every texture already at that size. Left as a row rather than patched, because a box filter at a scale of exactly one is not guaranteed to reproduce its input byte for byte and short-circuiting to a copy changes what comes out. `engine.render.bench.interface` covers `InterfaceMesh::Build`, which is the half of the UI `engine.gui.bench.interface` does not reach and the half that runs at the *display's* rate: 1000 plain rectangles 11.5 us and 10,000 of them 115 us, so it scales; a rounded rectangle is 7.6x a square one at 87 us; **a rounded outline is 211 us, well over the rounded row plus the outline row, so the two compound rather than add**; and **1000 short text runs are 218 us - eighteen times a rectangle and the most expensive thing an interface does** - with wrapping doubling it to 442 us. Two results contradicted the guesses the rows were written to test: **scissor batching is free on the CPU** (a thousand elements each clipped to themselves builds in the same time as a thousand sharing one clip, so a thousand-batch interface costs nothing extra to *build* and a CPU profile would show nothing at all), and `GlyphAtlas::Build` is glyph-count bound rather than area bound - 1.75 ms at 16 px against 2.38 ms at 32 px, a third dearer for four times the area. **That suite also taught something about benchmark hygiene that cost two other suites their figures.** Its images, mip chains and encoded forms are cached for the life of the process so no row pays to build its own input, and at 4096 that was over half a gigabyte permanently resident - which evicted its neighbours in the same binary from cache and made `Content.cpp`'s `Hasher::Of · 16 MiB` read 229 ns/KiB where it reads 131 alone. A benchmark that makes its neighbours slower is reporting its own working set in their figures, so the sizes were capped at 2048 and the ratio came back to 104 against 84. The interface suite needed a fix of its own: a benchmark binary is not a staged program and has no fonts beside it, so the atlas built empty and every text row silently measured the no-glyph path and came out *faster* than plain rectangles; it now borrows the fonts staged beside a sibling program. `engine.physics.bench.stepping` runs the whole tick `Pipeline.cpp` composes rather than one phase, over scenes that are shaped wrong on purpose. At constant density it is sublinear (1000 bodies 1.15 ms, 4000 4.89 ms, 16,000 15.1 ms); **4000 bodies in one broad-phase cell cost 57.5 ms against 4.89 ms for the same 4000 in stacks - 11.8x the cost off 7.4x the contacts**, which is the number to quote at anybody asking why the cell size is a parameter. The headline is the sleep pair: **a 16,000-body world in which every single body is asleep ticks at 16.2 ms, against 13.9 ms with the resting list emptied before every tick - sleeping saves nothing.** That is not a bug in the resting list, it is what the resting list is: `Solve.cpp` says at the line that uses it that "Asleep is immovable for this tick", so an asleep body is pinned rather than removed and its contacts are still generated and still swept by every solver iteration. The four phase rows added to prove it say where a settled tick goes - Solve 9.08 ms, BroadPhase 1.64 ms, SyncBroadphase 0.58 ms, NarrowPhase 0.53 ms - so the saving available is skipping a contact whose two bodies are both asleep, and nothing takes it. `client.bench.frame` is the first benchmark `mono.client` has ever had, over the per-frame CPU work that runs in `Phase::PreRender` at the display's rate: the draw list is linear at 1.75 ns a part (10,000 parts 19.1 us, 100,000 parts 175 us), and **`CollectParticleBatches` is 0.84 ns an emitter and stays linear at ten times the emitters**, which confirms the v0.17 redesign's whole claim - the CPU's share of particles is the emitter column and not the half million particles the device now owns. `ParticleFrame::Detach` at 26 us for 10,000 emitters is what the studio pays for rendering outside the tick and the client does not.

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
