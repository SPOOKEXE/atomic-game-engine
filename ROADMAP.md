
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
- [x] when a viewport in a side-by-side is closed, the open one should fill.
      imgui deletes a dock node whose windows have gone, but its guard is
      `window->DockId != node->ID` - and when a docked window stops being
      submitted imgui stores that same id on the window, so the two match and
      the auto-delete declines. The empty leaf kept its half of the split. The
      panel undocks itself once on the frame it closes, remembering the node in
      `DockInto` so reopening lands back beside its sibling.
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
- [x] when you upload a asset in studio, it doesn't process it for the local
      store. `ImportAssetPath` ensured the store, copied the bytes into `raw/`
      and refreshed the list - and never baked. So a dropped file was invisible
      in the viewport, silently absent from the next Publish, and on a fresh
      store made `PublishLocal` refuse everything with a message reading "bake
      before publishing - `contentimport --publish` and the studio both do",
      which was true of one of them. It bakes each imported file now, through the
      same one-file baker the asset picker's per-row button already used. A file
      that will not bake is counted rather than fatal: plenty of what a person
      drags into a content store is a licence or a `.txt`.
- [x] check cdn processes obj files. The `.obj` path itself was wired end to
      end; two real gaps sat either side of it. Dropping one in studio never
      baked, which is the item above. And **a textured `.obj` baked white with no
      error**: `bake::ReadObj` set `Submesh::Material` from `usemtl` and never
      opened the `.mtl`, so `Texture` stayed empty and `BaseColour` stayed white
      while the `.mtl` was copied into the store as an `unknown` asset nothing
      referenced. `bake` has no filesystem by design, so it records the `mtllib`
      name and `assetc` reads the library - `newmtl`, `map_Kd`, `Kd` - and fills
      the fields in *before* the existing rewrite, so an OBJ's reference goes
      through the same resolver and in-tree check as a glTF's. Measured: a mesh
      that carried a zero-length texture now carries `wood.atex` and
      `BaseColour [0.8, 0.4, 0.2, 1.0]`, exactly the `.mtl`'s `Kd`. Two tests,
      and a stale comment in the suite corrected - it said an `.obj` could not
      express a dangling reference, which stopped being true with this.
- [x] ground grid should expand way further. 40 cells was 160 studs, which ends
      well inside a baseplate. 120 cells is 480. Tripling the radius does not
      triple the cost: past the old 40-cell band only the heavy lines continue,
      one in five, so the far two thirds costs a fifth of what it would - 1808
      segments against 1296 for three times the reach, and nothing within 160
      studs looks any different.
- [x] in "Start" with 4 clients running, tons of network activity for no
      character movement. **Change detection was fine - zero rows detected at
      rest.** `mono.client/src/Replicated.cpp` registered `gui` and `script`
      classes and not `scene`'s, so every `Part` a snapshot named arrived
      untyped. `ecs.InstanceClass` crosses as a class *name*, a replica that
      cannot resolve it stores an empty one, and the anti-entropy audit then
      disputes every group it looks at - which re-arms the recovery walk to
      re-send every row of every entity every few ticks, for ever, over a
      difference no amount of re-sending can fix. Measured on `loadtest` with
      four clients and nothing moving: **91,507 B/s to 16,964, minus 81%**;
      disputes 600 to 4; untyped-class warnings 553 to 0. `mono.studio` calls it
      and the registry is process-wide, which is exactly why the editor never
      showed it. `mono.tools/loadtest` had the same gap.
- [x] in flamegraph, simulation needs more granularity, HUGE chunk missing. The
      premise was not what it looked like: `ecs::Scheduler` already opens a span
      per system and per phase, and the serial path shows all of it. What
      vanished was the **parallel** path - `Universe::Tick` dispatches worlds to
      pinned workers and `FrameGraph::Push` refuses an off-thread span, so with
      more than one world the whole of `simulation` was one opaque bar. The
      schedulers had timed themselves all along, so the fix is to report what
      they measured after the batch joins, named by world. A four-world capture
      that read `worlds (pinned workers) 23.935` now reads
      `client.world.2 · step-particles 10.653`, `client.world ·
      script-heartbeat 9.393`, and so on.
- [x] and the bug that found: nothing called `Scheduler::ClearTimings` on the
      world path. `World::Tick` calls `RunPhases` directly rather than
      `Scheduler::Tick`, and `ClearTimings`' own header warns that a caller
      splitting the frame that way has to do it itself. Nothing did, so
      `Timings()` had accumulated since process start and the studio's systems
      tab was showing a lifetime total labelled as a frame.
- [x] in flamegraph, when average over 250ms is selected, the bars over-expand
      into other bars. The average summed spans keyed on (depth, name) and
      divided by the **frame count**, but a span can open several times in one
      frame - a world owing two ticks, N worlds at one depth - so a span seen k
      times published k times its width with a left edge k times too far along.
      Duration still divides by frames, because what a span costs *per frame* is
      the useful figure; the start divides by occurrences. Plus a clamp in the
      layout, because an averaged span can still exceed an averaged frame and a
      bar drawn past the right edge lands on its neighbours.
- [x] when setting keybinds, disable input after keybind sets. Binding is a key
      press and `IsKeyPressed(key, false)` is true for the whole frame, so the
      press that assigned Ctrl+D was still true when the dispatcher ran later in
      the same frame and duplicated the selection. `Fired` refuses on the frame a
      binding changed - the whole dispatcher, because the press may also be
      somebody else's chord, and a frame in which no shortcut fires is not one
      anybody notices.
- [x] keybinds do not set properly (changes other keybinds). The `Action` enum
      and the `DEFAULTS` table are two lists of the same things in two places and
      they disagreed: the enum puts `ShowStatistics/ShowFrameGraph/ShowHeap`
      before the four tools, the table puts the tools first. `IndexOf` cast the
      enum to a subscript, so seven rows were cross-wired and binding "Select
      Tool" wrote onto the frame graph's row. The counts matched, so it compiled
      clean, and the suite missed it because it only covers the prefix where the
      two orders agree. `IndexOf` builds its map from what each row says it
      binds, so the two can never disagree again. Two regression tests, proved to
      fail against the old code.
- [x] add a spawn location that forces character to spawn at it.
      `SpawnLocation::Forced`, checked before team matching and before tree
      order, and still subject to the `Enabled` flag that was already there so
      turning one off gives the ordinary rules back. Taken from the struct's
      explicit `Reserved` padding, so the row is the same size and every pad
      written before this reads back with it false. Bound as a script property.
- [x] add a "Play Here" button. Beside Play in the transport. **A forced pad
      rather than a teleport after the fact:** a character is built by
      `LoadCharacter` from whatever `FindSpawn` answers, so moving it afterwards
      is a visible frame in the wrong place plus a race with the joining client.
      The pad is one reused instance named `PlayHere` and is `NotArchivable`, so
      pressing it twenty times leaves one pad and a saved `.agame` never carries
      somebody's old one.
- [x] input textbox not working. `gui::Type` was called by `mono.client` and by
      nothing in `mono.studio`, so a `TextBox` in a studio viewport took focus
      from a click, showed a caret and then ignored the keyboard - the editor was
      the one place a text box could be focused and not typed into. The overlay
      pass now builds a `gui::Typing` from `ImGuiIO` (which is already decoded
      text, so no key-code table), gated on the panel being in front, the
      keyboard actually being in it, and imgui not wanting the keys for a field
      of its own.
- [x] allow creating worlds even when scene is running in studio. The refusal
      was correct when written and its reason had since gone: it said "the
      snapshot Stop restores was taken before the run began", which was true of
      `Universe::Save`. `WorldRun::Snapshot` is a *world document* now and
      `StopWorld` destroys and rebuilds exactly the world it names, so a scene
      created during a run is in no snapshot. Also safe against the tick -
      `Universe::Tick` blocks and the button is pressed while the interface
      draws. Removal keeps its own guard.
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
- [x] NonEuclidean.luau spawn is wrong spot. The file had no spawn at all, so
      `scene::FindSpawn` returned a default `CFrame` and started everybody at the
      origin - which here is the middle of exhibit 1's tunnel mouth, on
      `LongMouth`'s own plane. A pad now stands on the plate in front of the row.
- [x] NonEuclidean.luau has multiple overlapping things. Every exhibit built
      its hidden space at the same depth, so exhibit 3's third room stood inside
      exhibit 4's first chamber (9 studs of x, 16 of z) and exhibit 4's second
      stood 12 studs inside exhibit 6's first. A hole cannot show that as a
      mistake - it shows one room with another room's wall through it, which
      reads as the *portal* being wrong. `away(n)` gives each exhibit a depth
      lane, so an exhibit may be as wide as its trick needs.
- [x] selection boxes are misaligned. `ProjectionFor` read the *live* camera and
      its comment said that was "the camera `PresentWorld` is about to render
      with" - true of exactly one panel per frame. `Renderer::Render` owns the
      whole frame, so the studio draws one panel and round-robins, and every
      other panel shows a texture from an earlier frame while its overlay was
      projected from the camera as it stands now. About 26 pixels of skew on a
      1600-pixel panel at 90 degrees a second, which is a gizmo that shakes while
      the camera moves and settles when it stops. `PresentWorld` records the
      camera and lens it actually rendered each panel with; the overlay projects
      from that. A replica still overrides it, because a replica renders through
      its own `ActiveCamera`.
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
- [x] some studio ui stretches instead of scales with aspect. The real
      distortion was the node-graph previews. `PreviewImage` is square by
      contract - "Pixels a side. Square, because a node's thumbnail slot is" -
      and the studio's render-pipeline previews go through the same sink with a
      texture of the renderer's own, which keeps the *resource's* shape: a
      full-screen target's preview is as wide as the screen and was squashed into
      the square slot by about 1.78. The canvas takes an optional aspect now and
      fits the picture inside the slot rather than stretching it across.
      Optional, so every caller that honours `PreviewImage` is unchanged.
- [x] physics bugs with the character (in playground steps, you phase through
      blocks, doesn't do bounds properly). `scene::StepCharacters` hard-assigns
      `Motion::Linear.X/Z` every `PreSimulation` - right for responsiveness, and
      it throws away the solver's contact impulse unintegrated. The only
      resistance left is position correction, capped at 3 m/s, and a default
      `WalkSpeed` of 16 beats it better than five to one, so a character leans
      through a wall over twenty ticks. The continuous sweep never catches it
      because that asks the *tunnelling* question - a 0.267 step against a 0.5
      half-extent never qualifies. `physics::ClipCharacterVelocity` now runs
      immediately after the controller and takes the into-surface component out
      of the intent before the integrator sees it. **Measured: walks to the block
      face at z -14.400 and stays; before, it passed clean through all four.**
      It stops rather than climbs - there is no step-up in the controller, and
      that is a feature rather than part of this.
- [x] add moving cubes in tunnels for lighting test too. A lit drifter pair
      down the west side, at a third of the fixed lamps' brightness over a 16
      stud range - a lantern at eye height on the open plain floods the ground at
      the lamps' own output and the stripes stop reading. They add the one
      lighting case a fixed lamp cannot make: a pane re-places the far side's
      lights every frame, and a lamp that does not move never says whether it
      re-placed them correctly.
- [x] sometimes when starting playground, the baseplate is rotated 45 degrees.
      **The scene is not nondeterministic** - captures are byte-identical, and it
      has no rotation, no random source and no hash-derived transform. What turns
      is the client's placeholder camera, which orbits at 0.12 rad/s and so is a
      quarter turn round within seven seconds; "sometimes" is how much wall clock
      had passed before anybody looked. That camera now holds a fixed angle and
      height for any world with a spawn pad - a world meant to be stood in is not
      one to orbit - and the height is proportional to the scene, because a fixed
      one is a grazing look at a baseplate. Verified: identical captures at 60,
      200 and 400 frames, where before all three differed.
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
- [x] content needs more granularity. Three stages on the client's content path
      had no span and therefore landed in `content`'s self time with nothing to
      name them: `OfferPublishedContent`, which walks every catalogue entry -
      nearly two thousand on a filled store - and enters every world to ask what
      each wants; the mesh upload, which had none while its texture sibling did;
      and the material decode, the only decode on that path without one.
- [x] pump events lags sometimes. Made granular by event kind, in both the
      client and the studio: a window resize is a synchronous round trip to the
      window system and a keystroke is not, and one bar covering both cannot say
      which one cost the frame. SDL exposes no name API, so it is a switch
      returning literals - `ENGINE_PROFILE_DYNAMIC_STABLE` keeps the pointer
      rather than copying, so a name built per event would dangle.
- [x] mouse movement seems to also cause pump events to increase lots. The
      studio read `Clock::Seconds()` **once per input event** to stamp
      `LastInputSeconds`, and a mouse dragged across the window delivers a motion
      event per sampled position - dozens a frame, each paying a clock read to
      record the same instant. Nothing reads that value at a resolution finer
      than a frame: it decides whether the editor may drop to its idle rate. The
      loop sets a flag and the clock is read once per pump. The per-event spans
      above are what will say whether anything else remains.
- [x] in flamegraph, add an "Event Scheduler" that auto-pauses on a rule.
      **The design decision is where a rule runs.** A rule tested by the panel
      would sample four times a second at the shortest interval it offers, and
      the frame it is written for is one frame long - which is the exact failure
      the feature exists to fix. So a rule is evaluated in
      `FrameGraph::EndFrame`, where the tree is still in hand, and it latches:
      a reader that asked "is anything wrong now" would answer no on the frame
      after every hit worth catching. One comparison per rule and no expression
      language, because every rule anybody has asked for is "this number got too
      big". An `under` rule waits for its span rather than firing on a frame
      that never ran it. Rules persist in the preferences file, spelled in the
      same words the panel shows, and are armed at start-up whether or not the
      panel is open.
- [x] in discord presence tab, add a list of templating replacement words. Five
      tokens, each with what it means and **what it says right now**, because a
      name and a description leave somebody guessing whether `{instances}`
      counts the world or the selection and the answer is one function away. A
      row copies itself. The names are checked against `Editor::DiscordFacts`
      by hand for now: `Fill` resolves an unknown token to nothing rather than
      to itself, so a listed-but-unpublished token would offer a word that
      silently deletes itself.
- [x] add selectable text in the control (mcp) panel saying how to attach, with
      claude/codex/prompt tabs. Read-only inputs rather than labels, because the
      job is dragging over them, and a config block that has to be retyped gets
      retyped wrong. The Prompt tab is the "tell a model" half: it names the
      port, the bridge and the tool count, and says not to start the editor.
      **And it fixed the port.** There were three defaults and they disagreed -
      the editor's help said 8738, its panel offered 8720, `mcpbridge` dialled
      8730 - so following the help got a bridge talking to a closed port. One
      `engine::control::DEFAULT_PORT` now, at 8738, which is what `.mcp.json`
      and `RUNNING.md` already said. Paths inside the JSON and TOML blocks are
      backslash-escaped, or a Windows path would paste as a parse error.
- [_] change Terrain demo to be procedural infinite generation, also spawn character on top of terrain
- [_] optimise Authority::DetectRows and missing gap post Authority::Publish. ReplicationStress in release build.
- [_] add a "bidirection" bool mode for portals, when enabled, you can enter from both sides, when disabled, you can only enter from entry side
- [_] when character sits in middle of portal, teleporting between both sides
- [_] character physics bugs and movement in weird directions and stuff.
      **Partly.** The same overwrite is behind it and the clip above fixes the
      walking-into-things half. What is not addressed is direction: a walk
      intent is world-space and nothing turns it, which is the portal item
      below. Reopen with a specific case if it still misbehaves on flat ground.
- [_] check character physics with portals, i think normal objects are fine but the character
- [_] when character sits in portal, character split in half
- [x] add dev and release builds to the github release. Two archives per
      platform per version rather than two release pages: the shipped one from
      `release` (-O3, no heap profiler) and `atomic-<version>-<platform>-dev`
      from the new `dist-dev` preset (-O1, frame pointer kept,
      `MONO_HEAP_PROFILE=ON`). **The diagnostics cannot be a runtime switch** -
      a block allocated with no profiler header and freed through the tracking
      `operator delete` reads four words of somebody else's memory - so the
      shipped build answers `--heap-report` with "this build has no allocator
      hooks", and until now the only answer to "it leaks" was "build the engine
      yourself". `-O1` because a build nobody can play is a build nobody
      reports from. `MONO_OPTIMISE` grew a level rather than a second boolean
      beside it. The dev flavour builds for a tag only, and the matrix is
      emitted by the `version` job because `jobs.<id>.if` cannot read the
      `matrix` context. Verified end to end on Linux: the dev archive unpacks,
      runs, and writes a 123-tag heap report the shipped binary refuses.
- [_] crossworldseam demo is not setup properly. **Half done.** The command in
      the file's own header omitted `--view-spacing 0`, so following it
      composited the two worlds 40 apart: two 80-stud floors z-fighting in a
      coplanar band and the camera aimed down the middle at neither pane. That is
      fixed, and the flag is now argued rather than just present. What is left is
      a mirror control in the same scene that draws no image at all in any of
      eight variants tried, while `MirrorCorridor` works in the same build -
      that one did not reduce to a scene cause and needs an engine look.
- [_] ground grid "enables always on top" when moving/scaling something,
      otherwise its not "always on top". **Diagnosed, and it is bigger than it
      looks.** `ShowGrid` is a plain preference and nothing touches it during a
      drag, so the grid draws identically either way. What is true is that the
      grid is an imgui overlay and an overlay has no depth buffer to test
      against - `AdornmentView.cpp:18` states exactly that - so it draws over
      geometry always, and a drag is simply when that gets noticed. Making it
      occlude means the grid becomes a depth-tested node in the render graph
      rather than a drawlist, which is real work and not a flag.
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

### v0.19.5

- [_] add a batch moveto/setcframe system (e.g. skygrid to move them all at once)
- [_] do similar for batched moveto/setcframe in other systems
- [_] `/home/declan/Documents/GitHub/BLADEBORNE_UNIFIED/game` port and also studio place `/home/declan/Documents/Bladeborne Floor 0.rbxl`. Turn this into a demo file.
- [_] explore the idea of having the active scene entities resident on the gpu always and we just have a compute timer on the gpu 24/7. this way, when the scene changes, we tell the gpu what changed. also doing a 2-way sync is easy with signatures/hashes with cpu-gpu, this way we have no swapchain waiting, we just compute at a given interval. sort of like "replication" to the gpu. same for particles, ui, entites, studio, etc.
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
- [_] concept idea: setup a public mcp repository in python, add .mcp.json in project folder that loads it, it watches forums channels in the discord server for new/existing bugs. agent writes a message in the channel stating you're fixing it, other agents work on other bugs. agents can write that "this bug is a big rewrite" in the channel too which could be helpful.
