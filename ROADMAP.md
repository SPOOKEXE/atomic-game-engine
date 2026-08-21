
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

- [x] build out proper code architecture documents => `docs/CODE_ARCH.md`. The
      layer stack, tiers, the dependency rule and what crosses it, what each
      program links, where a new thing goes, and what is checked by what. It is
      the in-tree half of `repo_layout.md`, which is cited sixty-two times by
      section number from CMake, from public headers and from
      `THIRD_PARTY_NOTICES.md`, and which has never been in this repository.
- [x] DOMAIN DRIVEN DESIGN & HEXAGONAL ARCHITECTURE - `docs/CODE_ARCH.md` §8 and
      §9. A module is a bounded context; the primary port is an ECS column
      rather than an interface; the four cases where two modules claim one noun
      (`world`, `graph`, `ui`, `net`) are written down; the test of the hexagon
      is the `server` preset, which configures with no client at all.
- [x] properly make a ECS component document list - `docs/ECS_COMPONENTS.md`,
      **generated** from `ecs::Components` by `just components`. 129
      engine-registered components, each with its size, whether it is a tag,
      whether a save can carry it, whether replication has a compact form, and
      one written line saying what it is for. `just components-check` fails when
      a registered component has no purpose line, so the list cannot drift.
- [x] choose networking backend for each engine feature - `docs/CODE_ARCH.md`
      §10. Ten features, what each actually needs, and what it should be on.
      The finding: sixteen TCP connections and one shared UDP reliable window
      are both workarounds for not having streams.
- [x] the layer rule is enforced. `expected_graph.json` carries a `layer` on
      thirty modules and `CheckTargetGraph.cmake` refuses an upward edge, an
      unnamed same-layer edge and any edge into the program band. Six fixtures
      under `mono.tools/architecture/tests/` check that the check still bites.
      `AGENTS.md` rule 1 was the headline rule and was previously unchecked.
- [x] the review itself - `docs/ARCH_REVIEW.md`. Findings from a full pass, each
      marked as reproduced or as reported, so nothing here is acted on without
      being checked first.
- [_] check if we need to move files / classes / structures around. **Analysed,
      not applied** - `docs/ARCH_REVIEW.md` §C. The five with arguments:
      `nodegraph` is linked only by `studio` and belongs in it; `RenderView` is
      5,485 lines and splits by node family; `script` is 33k and should be four
      modules behind the VM-free port it already has; `mono.client` uses three
      modules it does not declare; `SurfaceCameras.cpp` is 4,108 lines that lift
      out. `mono.libraries/` should **not** be created yet - there are two
      leaves and the bar is three.
- [_] clean up all ECS components. **Analysed, not applied** -
      `docs/ARCH_REVIEW.md` §D. 35 misplaced fields, 6 merges, 12 splits, 10
      renames. `WorldTime` is saved under the compiler's spelling of its type,
      which rule 4 forbids, and three more are reported to be.
- [_] scan through all serial loops for parallel or vectorised work. **Audited,
      not applied** - `docs/ARCH_REVIEW.md` §F. The first one is not a
      parallelisation at all: the client runs eight full store walks per world
      per frame and discards the result.
- [_] update AGENTS.md in root and subdirectories. **Six done**: root,
      `mono.engine` (was a one-line stub), `mono.engine/control` (the only
      module of 29 without one), and `game`, `scene` and `render`, each of which
      asserted an invariant the code had outgrown. Nine more are listed in
      `docs/ARCH_REVIEW.md` §B as still wrong.
- [_] improve build times. **Measured, two fixes applied.** `Clock.hpp` went
      from 82,779 preprocessed lines to 422 and reaches 264 of 479 translation
      units; `Store.hpp` from 120,696 to 82,358 across 253. The ranked list of
      what is left is `docs/ARCH_REVIEW.md` §E, and the top entry is installing
      ccache, worth about 150 s of a 172.6 s clean build.
- [_] check all asynchronous and parallel points. **Audited** -
      `docs/ARCH_REVIEW.md` §A and §F. `core::FrameGraph`'s owner and dropped
      counter were racy and are fixed; `graph::NodeCatalogue` hands out pointers
      its mutex does not protect and is open.
- [_] add more MCP integrations. **One added**: `engine_components`, because
      `component_list` deliberately covers only what a game declared, so a model
      could not ask what storage the engine has. The module graph, a test
      runner, script tools and a log tool are still missing.
- [_] build out a full logging, metrics, etc so we can track what the engine is
      doing in dev builds effectively. **Audited, not built** -
      `docs/ARCH_REVIEW.md` §G. Logging is 101 lines with no categories, no
      dynamic level and no free disabled statement, and thirteen modules never
      call it. `core::Metrics` has no read side at all.
- [_] quic implementation. **Scoped, not started.** `docs/CODE_ARCH.md` §10.1
      has the seam analysis: `net::Transport` is a real port and
      `replication::Session` already holds one, so the open question is the
      layer above - `Session` owns a `Link`, a reliable pair and its cipher
      pair as members, and QUIC supplies all four itself.
- [_] think plan for future features as well listed in roadmap and plan for them
      now. **Partly** - the component gaps v0.21 and v0.23 imply are recorded in
      `docs/ARCH_REVIEW.md` §D4 (no `Skeleton`, `Bone`, `Animator`,
      `Constraint`, `Terrain`, `LevelOfDetail`, `Fog` or `Atmosphere` type
      exists), and the `persistence`/`ledger` layer problem in
      `docs/CODE_ARCH.md` §4.2 - both L5 and L6 were reserved for them and are
      now occupied.

- [_] make the four unchecked architecture rules checked. `docs/CODE_ARCH.md`
      §11 lists them in value order and they are the third category rule 6
      refuses to allow: a module keeping a private copy of data the ECS owns; a
      pointer inside anything that crosses a world boundary; a `Name` serialised
      as its `Id()` rather than its string; a header in `include/` that only its
      own module includes. The layer rule was in that list until this version
      and took an afternoon, so the estimate is not speculative.
- [_] finish the `AGENTS.md` sweep. Nine files still assert something false and
      are named with line numbers in `docs/ARCH_REVIEW.md` §B: `parallel`
      (claims `process/` and `ipc/` do not exist, they have since v0.2, and
      misidentifies the join's serialisation point), `launcher` (six decisions
      are already in `Interface.cpp`), `mono.client` (four of its headers are
      included by `mono.studio`, and its largest single piece of logic,
      `PumpContent`, is not mentioned at all), `script`, `mono.server`. Plus
      `docs/QUIC.md:55` calling `net` L2 when it is L11, `README.md:34` saying
      "four rules" when there are six, and `docgen/pages/Modules.md` listing ten
      of twenty-nine engine modules.
- [_] call `Components::Seal()` at start-up in every program. `ecs/
      Components.hpp:11-17` describes a determinism guarantee that rests on the
      table being closed before any world ticks in parallel, and the only caller
      is a test - so the guarantee is unenforced in every shipped binary. This
      is the smallest item on this list and the one with the worst failure mode.
- [_] register `WorldTime`, `PortalProxy`, `NotArchivable` and `DirtyBits` under
      explicit names. They are minted by `Components::Of<T>()` from the
      compiler's spelling of the type and they reach `.agame` files, which
      decision 21 and rule 4 both forbid: the spelling is stable within one
      build and nothing wider. A save-format break, and the repository is
      pre-release.
- [_] decide where `persistence` and `ledger` go before something needs them.
      `docs/CODE_ARCH.md` §4.2: the design reserved L5 and L6 for them, the
      built tree put `collision` and `spatial` there, and decision 7 still says
      the datastore surface is server-only. Deciding now costs a paragraph;
      deciding when the module arrives costs renumbering eleven modules.
- [_] the three small structural fixes with no argument against them. Declare
      `Engine::assets`, `Engine::graph` and `Engine::script` in
      `mono.client/CMakeLists.txt` - all three are used from public headers and
      arrive transitively today, which is the exact failure that file already
      documents happening once at `:40-45`. Move `nodegraph` into `mono.studio`
      - it is `client` tier, carries imgui and four concurrency headers, and
      exactly one target links it. State the lifetime contract on
      `NodeCatalogue::Find` that `All` already carries, and say in it that the
      mutex does not extend past the return.
- [_] split `Renderer::RenderView`. It is 5,485 lines inside a 13,517-line
      translation unit, holds its node handlers as lambdas, and calls
      `SDL_BeginGPURenderPass` inline in eighteen places - which is what makes
      those passes invisible to the render graph. Splitting it by node family,
      into files that each implement `GraphRunner` for one family, is the same
      change as fixing the build cost: that one file is 31.2 s of a 172.6 s
      build and cannot be split across cores. `D00016`.
- [x] the compiler cache. `MONO_CCACHE` finds `ccache` or `sccache` and sets
      both launchers before the first `add_subdirectory`, so the vendor tree is
      covered - that is 2373 of 4206 CPU-seconds. Measured on one machine, same
      build directory: a clean build went from **3508 CPU-seconds to 197**, on
      1894 of 1894 direct hits, with all 43 suites passing on a build where
      every object came from the cache. A sixth of the calls were being skipped
      silently because SDL and glslang carry precompiled headers and ccache
      declines those unless sloppiness permits it, so the launcher passes
      `CCACHE_SLOPPINESS` through `cmake -E env` rather than leaving it to each
      developer's config. The lookup is `NO_CACHE`, so installing ccache after
      a build directory already exists is picked up by the next configure rather
      than needing a wipe - `find_program` caching its own "not found" would
      have made the advice in the status message wrong for the exact person it
      is written for. CI keeps its cache between runs on Linux and macOS.
- [_] the rest of the measured build wins, in `docs/ARCH_REVIEW.md` §E2's order.
      `UNITY_BUILD` for `release` and `ci` is 51 to 73% of first-party compile
      CPU and is blocked by about nine anonymous-namespace collisions. `-g1` is
      36% off the heaviest translation units. Taking `spdlog/spdlog.h` out of
      `core/Log.hpp` is the same fix `Store.hpp` and `Clock.hpp` just had, one
      level down, and twenty-nine of its includers use no log macro at all.
      Precompiled headers measured at only 11%, so they rank below unity builds
      rather than above - which is the opposite of the usual intuition and the
      reason it was measured.
- [_] the four correctness findings that are open. `docs/ARCH_REVIEW.md` §A3 to
      §A5: an unbounded `JS_ExecutePendingJob` loop that hangs the host inside
      one tick and is not caught by the step budget because both interrupt
      handlers zero their counter on trip; `Client`'s copy of
      `scene::InputState`'s mouse behaviour, which is last-write-wins across
      worlds and is rule 2 exactly; and fifteen call sites that discard
      `CommandQueue::Post`'s result while recording the state as landed, so a
      dropped `Open` is a permanently silent voice.
- [_] the eight-walks-per-world-per-frame content scan.
      `mono.client/src/Client.cpp:495-498` runs `CollectWantedContent`
      unconditionally every frame, which is eight full store passes per world,
      and discards essentially all of it in the steady state. Live on every
      default run. Not a parallelisation - work that should not happen.
- [_] a component catalogue and a module graph in the MCP surface.
      `engine_components` landed this version; the module graph, the layer table
      and a test runner are the next three a model working on this engine
      obviously wants and cannot get. Also fix the three port mismatches, one of
      which has `studio --mcp-port` bare defaulting to 8720 while its own help
      says 8738, and give `mcpbridge` a suite - it has none.

---

- [_] fix viewport image size stretch fix
- [_] in MipProbe scene, when you fly camera around the mesh is being projected incorrectly (windows) [RELATED TO VIEWPORT IMAGE SIZE FIX, VERIFIED ISSUE ON LINUX]
      **Not the projection, and that is checked rather than assumed.** MipProbe
      was captured headless at 1600x400, 800x800 and 400x1600 and the red box
      measures 9.50% of width at 1:1 and 2.12% at 4:1, with its height fraction
      unchanged at 16-17%. That is the 4x narrowing a correctly widened
      horizontal field of view gives, so the client's static projection is
      right. What is left to look at is the studio path: `ActiveCamera` is one
      resource per *world* and `SetViewportSize` writes it per *panel*
      (`Editor.cpp:1844`), while the studio round-robins one panel per frame -
      so two views of one scene at different sizes take turns owning the aspect
      that culling and mirror fitting run against. `Renderer::Render` projects
      from `SceneTarget` and is unaffected, which is why a still picture cannot
      show it and flying the camera can.
- [_] when a viewport in a side-by-side is closed, the open one should fill
- [x] make the ground grid static. The line *positions* were already pinned by
      `SnapDown`; what slid was which lines were drawn heavy. `major` came from
      the loop index, and the loop runs outward from a camera-snapped origin, so
      the heavy lines moved one cell every time the camera crossed one.
      `IsMajorLine` now decides from the world coordinate. Measured over 20 m of
      camera travel the old rule produced five different sets of heavy world
      lines and the new one produces a single set.
- [x] make the ground grid fade off in the distance. The fade existed and could
      not work: `ImDrawList::AddLine` takes one colour, so a line had one alpha
      along its whole length and the grid stopped at a hard rectangle whatever
      that alpha was. Each line is now drawn in eight pieces, each faded by the
      radial distance from the camera to its own middle rather than by how far
      sideways the line sat. Roughly 1,300 segments a panel rather than 13,000.
- [_] add a batch moveto/setcframe system (e.g. skygrid to move them all at once)
- [_] do similar for batched moveto/setcframe in other systems
- [_] `/home/declan/Documents/GitHub/BLADEBORNE_UNIFIED/game` port and also studio place `/home/declan/Documents/Bladeborne Floor 0.rbxl`. Turn this into a demo file.

---

- [_] add dev build and release builds to github release (two separate tags Release and Dev -O1 but keep things like heap profiler), then you have per-version ones as well
- [_] ground grid should expand way further
- [_] in "Start" with 4 clients running, tons of network activity for no character movement, quickhash / caching / signature not working properly or other bug
- [_] in flamegraph, simulation needs more granularity, HUGE chunk missing.
- [_] in flamegraph, when average over 250ms is selected, the flamegraph bars can over-expand into other bars.
- [_] content needs more granularity, big chunk is not listed (optimise when you find)
- [_] pump events lags sometimes for some reason (e.g. user inputs, changing window size, etc). make pump events more granular by adding per-event-name.
- [_] mouse movement seems to also cause pump events to increase lots
- [_] in flamegraph, add a "Event Scheduler" where you can setup a rule that auto-pauses the flamegraph when conditions are met (e.g. when pump events hit >2ms, i can force a pause on that flamegraph to see the cause).
- [_] in discord presence tab, add a list of templating replacement words (e.g. {world} {instances}), etc.
- [_] when setting keybinds, disable input after keybind sets (it runs immediately after)
- [_] keybinds do not set properly (changes other keybinds)
- [_] add (selectable) text in the control (mcp) that tells you how to add as a mcp (and a section to tell models how to add it). add claude/codex/prompt tabs to hold these.
- [_] add a spawn location that forces character to spawn at it (SpawnLocationComponent with a enabled flag too).
- [_] change Terrain demo to be procedural infinite generation, also spawn character on top of terrain
- [_] optimise Authority::DetectRows and missing gap post Authority::Publish. ReplicationStress in release build.
- [_] add a "bidirection" bool mode for portals, when enabled, you can enter from both sides, when disabled, you can only enter from entry side
- [_] add a "Play Here" button (spawns character at camera position)
- [_] input textbox not working
- [x] allow creating worlds even when scene is running in studio. The refusal
      was correct when written and its reason had since gone: it said "the
      snapshot Stop restores was taken before the run began", which was true of
      `Universe::Save`. `WorldRun::Snapshot` is a *world document* now and
      `StopWorld` destroys and rebuilds exactly the world it names, so a scene
      created during a run is in no snapshot. Also safe against the tick -
      `Universe::Tick` blocks and the button is pressed while the interface
      draws. Removal keeps its own guard.
- [_] when character sits in middle of portal, teleporting between both sides
- [x] live instances listed items get cutoff in list. The table was
      `SizingStretchProp` and the action column took 0.22 of the panel while
      holding `View`, `+ Player` and `Stop` side by side - a proportional width
      cannot be right for a cell whose content has a fixed size. It is
      `WidthFixed` now, measured from the labels rather than a pixel guess.
- [x] when play is pressed, ensure a viewport is opened. Nothing in
      `SetRunMode`/`BeginRun` opened one, so Play with every panel closed
      started a server and a client and drew nothing. It now calls
      `ShowWorldInViewport`, the same path the Live Instances "View" button
      uses, so it reuses an open panel or reopens the main one and only makes a
      new panel when every one is spoken for.
- [_] NonEuclidean.luau spawn is wrong spot
- [_] NonEuclidean.luau has multiple overlapping things
- [_] selection boxes are misaligned
- [x] when pressing CTRL and scaling a part, scale both sides at same time.
      `ScaleSide::Both` already existed as a *preference*, so the only way to
      reach it was to go and change a setting. Ctrl now inverts it at the grab.
      `BothHalf` rather than `Both`, because that is the one whose resulting
      *size* lands on the snap step. Read once, at the grab, so a modifier
      cannot change what a drag means half way through it.
- [x] add a (ACTIVE) scene_name. Viewport tabs read the scene they show, and
      the one being edited is marked. imgui keys a window on its title, which is
      why they were fixed strings; `###` splits the two, so the text can change
      every frame while the window, its dock node and the saved layout stay the
      same panel. **One-time cost:** the stored ini key changes, so existing
      saved layouts undock these panels once. `ViewportIdentity` is what
      `SetWindowFocus` and `FindWindowByName` must now be given.
- [_] some studio ui stretches - more vscode-ey
- [_] physics bugs with the character (in playground steps, you phase through blocks, doesn't do bounds properly)
- [_] character physics bugs and movement in weird directions and stuff
- [_] check character physics with portals, i think normal objects are fine but the character
- [_] when character sits in portal, character split in half
- [_] add moving cubes in tunnels for lighting test too
- [_] sometimes when starting playground, the baseplate is rotated 45 degrees?
- [_] crossworldseam demo is not setup properly
- [x] surfacecamera lighting is really dark. **Measured: a mirrored floor read
      0.21 of the same floor seen directly.** The main view is deferred and ends
      at the `tonemap` node; a mirror is forward - `mirror-capture` runs
      `opaque.frag` straight into the surface texture and nothing encoded it,
      because there is no `mirror-tonemap` node the way there is a
      `portal-tonemap` one. The pane then read that linear value back as a
      display colour and the frame's tonemap encoded it again, and a linear
      value through that round trip comes out about a fifth as bright.
      Encoding at capture makes the round trip an identity: **0.21 to 0.93**,
      the residue being the SSAO and seam spill a surface pass genuinely does
      not get. Flagged per draw rather than done unconditionally, because
      `portal-capture` runs the same shader and already has its own tonemap
      node - portal captures are byte-compared and visually identical.
- [_] ground grid "enables always on top" when moving/scaling something,
      otherwise its not "always on top". **Diagnosed, and it is bigger than it
      looks.** `ShowGrid` is a plain preference and nothing touches it during a
      drag, so the grid draws identically either way. What is true is that the
      grid is an imgui overlay and an overlay has no depth buffer to test
      against - `AdornmentView.cpp:18` states exactly that - so it draws over
      geometry always, and a drag is simply when that gets noticed. Making it
      occlude means the grid becomes a depth-tested node in the render graph
      rather than a draw list, which is real work and not a flag.

### v0.20

- [x] thoroughly implement every user-interface element, including `SurfaceGui` and `BillboardGui` - `SurfaceGui` gains `ZOffset`, `MaxDistance`, `ClipsDescendants` and `Active`, and `BillboardGui` gains `Active`, `Brightness`, `ClipsDescendants`, `CurrentDistance`, `DistanceStep`, `ExtentsOffsetWorldSpace`, `SizeOffset` and `PlayerToHideFrom`; new classes `UIGradient`, `UITableLayout`, `UIPageLayout` and `UIDragDetector`; `ScrollingFrame` completed with `ScrollingEnabled`, `AutomaticCanvasSize`, the two `ScrollBarInset`s, `VerticalScrollBarPosition`, `ElasticBehavior`, the three bar images and `AbsoluteCanvasSize`/`AbsoluteWindowSize`, plus wheel and thumb-drag input; `RichText`, `MaxVisibleGraphemes`, `ContentText`, `TextBounds` and `TextFits` on every text class; `Interactable`, the four `NextSelection*`, `SelectionOrder` and `SelectionImageObject` on `GuiObject`; `HoverImage`, `PressedImage` and `ResampleMode` on the image classes; `Enabled` and `ApplyStrokeMode` on `UIStroke`. Laid out, drawn by both backends, saved, replicated, bound and in the Properties panel. `D00129` carries the members that need a subsystem this engine has not got (filed as `D00120`, renumbered at v0.17 - that number was already a retired entry)
- [_] build out all remaining roblox surfaces with available underlying surface
- [_] port many particle features from unity to here (https://docs.unity3d.com/6000.5/Documentation/ScriptReference/ParticleSystem.html)

### v0.21

- [_] find a way to (easily) and thoroughly test rendering steps and ensure they produce the right image with right projections
- [_] finish portals so lighting, physics, projection, clipping and geometry crossing the seam are seamless
- [_] ensure per-mesh render capabilities, global lighting render capabilities, camera lighting render capabilities, etc. compute shaders, post-processing, etc.
- [_] simplify and strip old rendering code that is not part of the node system. Everything should be in the node system.
- [_] port semi-real raytrace and path-trace as part of nodes
- [_] make demo render pipelines with semi-real raytrace and path-trace
- [_] (dynamic) ambient occulusion, screen-space, fog, atmosphere, clouds, global illumination, displacement maps (make it rendering only but not physical)
- [_] render pipeline nodes for above
- [_] plan the entire rendering system to a visual compositor system like Unity. https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html
- [_] viewport indictator direction gizmo (select and lock to certain directions)
- [_] 3d cursor and camera orbit options under gizmo
- [_] ensure full parallel/vectorised (i.e. get all active scenes => build entity list => update gpu resident => batch render all cameras in every scene)

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
- [_] html-based ui creation (html-script?)
- [_] import blender files in asset explorer
