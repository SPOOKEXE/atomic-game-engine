# Zero v0.26.1 execution record

The active scope is every item under Zero in
[NEW_ROADMAP.md](../roadmap-rework/NEW_ROADMAP.md). Remove an item there only
after its implementation, relevant tests and runtime evidence are complete.
Written code, historical checks and source inspection alone do not close it.

## Work order

1. Fix correctness and attribution: worker tick accounting, scrolling, factory
   Play orchestration, scheduler preservation and late plugin edits.
2. Remove measured repeated work: node dispatch construction, adornment geometry
   and projection, animation preparation, residency and frame-graph publication.
3. Evaluate GPU critical paths in BufferCompute, Mirrors, TornadoSim, Weather
   and Magic without silently reducing authored quality.
4. Complete the empty-scene demo, interactive menu and feature checks, current
   build-turnaround evidence, unfinished milestone evidence and the full gate.

Three measured rounds cover clock time, latency, allocation and memory, and
compute. Unchanged or rejected candidates need a recorded reason and evidence;
an arbitrary code change is not required for every measured cohort.

## Current evidence

| Work | State | Evidence and next gate |
|---|---|---|
| Every-frame Play publishing | Complete and pruned | Commit `acd86b08`; full `just check` and pre-commit passed. Shared world collection, tracked mutation evidence, conservative escaped writes and independent client delivery. |
| Worker tick housekeeping labels | Committed, demo verified | Commits `39991be9`, `029a1076`. Isolated Animation Play, Debug, 600 frames with the graph open: unlabeled driver self time under `Universe::Tick` fell from 0.116 to 0.017 ms per tick. Timing snapshots, worker timing reports, request collection and frame completion are named; index-matched timing deltas cut that bookkeeping by about 44%. `jobs.assigned.body` is reported worker busy time overlapping the per-world tree, not driver time. |
| Publish bearing survey | Committed, tests passed | `Store::LayoutRevision` advances on relocation, vacating, promotion, reparenting, clears and loads. `Authority::GatherBearing` keeps its tables while store identity, layout and component union repeat. Animation Play: CollectBearing 0.624 to 0.009 ms mean, PublishMany 1.405 to 0.411 ms, frame mean 3.28 to 2.14 ms. Existing locality refresh case and a new layout revision case pass. |
| Animation render preparation | Measured, partly rejected | Isolated Play: render preparation about 0.17 ms per frame summed over the authority and replica (evaluate-animations 0.036, collect-replicated 0.022, joint gather 0.012). Skipping evaluation between ticks was rejected: script writes and Studio edits to `Bone.Transform` are overwritten by evaluation today, and an input-keyed skip would let them persist. |
| Mirrors-1-world | Complete and pruned | Isolated Play, Debug, 600 frames at 1280x720: unlabeled `Universe::Tick` driver time about 0.027 ms per tick (reported 0.18 ms). `capture mixed surfaces` 0.070 ms mean, 0.009 ms p50: it re-captures only on frames after a tick. GPU surface capture 0.011 ms mean (reported 43.34 us), mirror overlay 0.012 ms (reported 31.74 us). |
| BufferCompute shadow and SSAO | Measured, decision needed | Isolated Play at 1280x720: GPU shadow 9 us, SSAO 10 us. Shadow is the fixed cost of one 2048 squared depth map shared by every view; reusing an unchanged map needs a map per view (16 MB each) for single-digit microseconds, rejected. SSAO now skips the normal fetch on sky depth (exact, no measurable change here because the ground covers most pixels). A further SSAO cut needs a quality choice: half resolution with depth-aware upsampling, fewer taps, or temporal accumulation. |
| Weather Play hotspots | Partly done, redesign pending | Commits `b3b5481e`, `43a6bf13`, `a65588a8`. Isolated Play, Debug, 600 frames: frame mean 8.40 to 7.54 ms, gui compile 1.013 to 0.193 ms (source filter set), replica-gui-services 0.525 to 0.289 ms per tick (PlayerGui source walk reuse), Authority::PublishMany 0.69 to 0.73 ms (reported 1.1). Remaining cost is WeatherCompute: weather-residency 3.5 ms CPU and gpu weather-integrate 3.4 ms, which wait on the approved redesign. Optimized `bench` Studio Play: busy 1.9 ms per frame; weather sparse residency plan 1.03 ms is the largest CPU item, inside the WeatherCompute redesign. After the publish survey, bearing and structure reuse: Debug frame mean 6.65 ms (from 8.40), PublishMany 0.54 ms. |
| Frame graph layout | Measured | Commit `262eec3b` names its steps. Animation Play with the graph open, every-frame publish: layout 0.30 ms (gaps 0.11, rows 0.10, copy 0.03, fit 0.03, flame 0.03). Skipping an already-sorted gap sort changed nothing and was reverted. What remains is building and placing about a thousand string-named `DiagnosticSpan` records per publish in Debug. |
| Magic Play hotspots | In progress | Commit `13367e1f`. Isolated Play, Debug, 300 frames: frame mean 23.4 to 22.1 ms, p99 65 to 53 ms; Authority::Interest p99 33.0 to 18.6 ms (memoised batched visibility). Remaining: collect-replicated rebuilds the replica draw list (about 11 ms on presenting frames; its serial loop is a recorded decision that needs a replication contract change), pack instances 3.8 ms and resident.scene.rebuild 2.4 ms, because moving objects mark object damage every frame. Residency release now skips its scan when every live entry was seen and stops early otherwise: begin instance residency 0.80 to 0.71 ms (the scan still runs on about 60% of frames because spells despawn). Optimized `bench` Studio, 600 frames: frame mean 2.88 ms, p50 2.69, p99 5.68; the one 112 ms hitch is the replica join snapshot (`replica.snapshot` 107.8 ms once). Leading steady costs: ViewRecording::Begin 0.79, pack instances 0.63, collect-replicated 0.52, Authority::PublishMany 0.48 ms. Round 2: the authority view and the Play client view shared one instance residency while keying rows by different worlds, so each view released and re-uploaded the other's 5,355 rows. Residency now follows the view's source store: optimized ViewRecording::Begin 0.72 to 0.27 ms, pack instances 0.58 to 0.13 ms, residency release 0.17 to 0.00 ms. A packed seen-frame array for the release scan measured no change and was reverted. FindBearing now keeps its sorted set while the survey repeats: 0.125 to 0.001 ms, PublishMany 0.46 to 0.35 ms. Remaining one-time cost: the replica join snapshot restore (`replica.snapshot`, 108 ms in `store.Apply`). The per-client structure diff is skipped while visibility and the known set repeat: Structure 0.144 to 0.002 ms, PublishMany 0.35 to 0.20 ms. |
| Empty scene demo | Demo added, baseline measured | `Empty.luau` is the bundled empty world. Isolated Debug: Edit 0.77 ms frame mean; Play with the default Start world and both replicas 1.70 ms. Leading fixed costs in Play: Authority::PublishMany 0.35 ms per frame (component resolution 0.07, unlabeled Steady remainder about 0.05; the rest is per-phase work), physics pipeline about 0.3 ms per tick for the one character body, build interface 0.27 ms. No single step dominates; remaining candidates are publish component resolution caching and an idle-physics fast path. Optimized `bench` Studio Play: busy 0.44 ms per frame (render 0.15, publish 0.11, interface 0.11). |
| TornadoSim | In progress | Isolated `TornadoSim-isolated.agame` Play, Debug, 400 frames: script-heartbeat 105 ms per tick (control 472), gui compile 4.3 ms (control 60), Authority::Resign::gather 11.9 ms (control 27), physics.contacts 4.4 ms, gpu clouds-compute 3.8 ms, gpu-particle-field 2.7 ms. Vendor code including Luau is already -O2 in Debug, so the script cost is the model's own Luau work and its engine bindings. Optimized `bench` Studio: script-heartbeat 12.8 ms per tick, frame mean 11.9 ms. Disabling each heartbeat pass in scratch copies: link damage about 6.8 ms, body forces 2.8, vegetation 1.0, visual field and publish under 0.2. Preparing the field once per tick and sharing part samples gave 12.8 to 12.0 ms; a per-part load memo gave nothing (joints rarely share parts) and was dropped. What remains is Luau field math and binding calls (GetComponent name and registry lookups, table building) per joint. Caching the Luau component name lookup per thread measured no change (12.05 vs 12.23 ms) and was reverted. Skipping unchanged link and vegetation writes measured 12.97 to 12.79 ms (noise) and was not applied. |
| Studio command and menu check | In progress | Headless Studio with the Animation demo, driven over `--mcp-port` through `list_command_palette` and `run_palette_command`, re-polling availability before each command and checking the log after each. Found and fixed two bugs: Run from a Play client view aborted Studio with a duplicate `script-heartbeat` schedule (`c60b9d20`), and Client Settings was never available from the client view (`f496348d`); both have a regression case in `Tools.cpp`. After the fixes all 24 palette commands and a Play, client settings, Stop, Run, Play, New Game scenario ran with no new error. An edit scenario over `world_tree`, `entity_create`, `select` and `instance_set` (create, select, set Position and Color, Duplicate, Undo, Redo, Move, Rotate and Scale tools, Play and Run with a selection, Save) ran with no crash or new error. Behaviour noted, not changed: Redo of Duplicate leaves nothing selected; Play moves focus to the client view and clears the selection; Stop rebuilds the world, so instance ids change and that world's undo history is dropped (by design). Because the rebuilt store reuses id numbers, an automation client holding a pre-Play id acts on a different instance after Stop: `entity_destroy` with the old id returned ok on another entity. Menu-bar panel toggles are covered by `--all-panels`. |
| Every-demo panel sweep | Fixes committed | `--all-panels` (`90396ec5`) opens every panel; all 81 bundled demos ran headless in Play for 150 frames. Fixed: headless ImGui abort past 65,536 vertices (`01ac5138`); shader recompiles between a Play authority and replica plus post shader refused as a material (`b92521ea`); JavaScript client scripts refused in Play replicas (`dd8b0612`); EditableImage escape making PbrTextureUniqueStress spend 1 s per frame re-hashing images, now 2.8 ms frames (`4a3b441b`); Script Profiler sampling every Luau step, Terrain 13.3 s to 1.04 s per tick with the panel open (`35168650`). Admission refusals now name the exceeded count (`f34e9361`). Open: Cube and StressPhysics exceed the uncalibrated default physics caps and fault on their first tick (decision needed); a networked client still starts Luau before it knows a JavaScript server's language; the Script Profiler's remaining cost is Luau single-step mode, which an interrupt-based sampler would avoid. |
| Preferences and dock tab scrolling | Complete and pruned | Focused DockLayout/Tools checks passed 3,155 assertions in 170 cases, including overflow, wheel navigation, page scroll retention and fixed tab strips. Final milestone visual checks remain tracked separately. |
| Built-in node dispatch | Complete and pruned | Commit `19f50dcd`. Render non-GPU suite 918 cases and GPU suite 239 cases passed (15 windowed skips). Isolated empty-scene capture no longer has a `build node table` span (whole-workspace baseline 0.389 ms mean). |
| Adornment drawing | Complete and pruned | Commit `cd46e87d`. Adornments demo in Play, Debug, 600 frames: overlay drawing 0.908 to 0.280 ms mean, adornment drawing 0.366 to 0.061 ms, geometry 0.189 to 0.111 ms. `viewport-adornments-bench` full uncached vertex, UV, colour and index oracle passed for every row; at 512 boxes static 0.254 ms vs moving 0.549 ms in bench. |
| Changed-row presentation damage | Committed, tests passed | Commit `a1bc1b64`. Damage checks read changed rows through const access, so observed presentation columns no longer escape every frame. |
| Factory Play orchestration | Committed, tests passed | Commit `bf8f947c`. Studio non-GPU suite passed 1,063 cases, including factory and Play pause interplay and an unrelated Play run keeping its VM, cadence and replicas. Targeting an actively played authority awaits the user policy choice below. |
| Scheduler preservation | Committed, tests passed | Commit `d6128093`. `SystemLifetime` Portable/WorldBound and `PreserveForWorldReplacement` cover package transactions and DataFactory restore, backward seek and intervention rollback, refusing live scheduler edits made during preparation or abort. Script and physics cadence clocks ride in the snapshot. ecs, script, world, client, studio, examples, game and server non-GPU suites pass. Full gate pending. |
| Other Zero entries | Open | No completion inferred from source inspection or the earlier publishing gate. |

Gate status: `just test-pre-commit` passed 805 suites, 0 failed (365 s);
`just test-pr` with headless GPU cases passed 805 suites, 0 failed (817 s);
`just preset=ci check-pr` (the pre-push gate) passed on the final tree; the
`[slow]` tier passed 805 suites, 0 failed (1,327 s). A first niced slow run
under load failed one timing-bound portal session case
(`late source delivery cannot replace successor same-name assets`), which
passed alone and in the full rerun.
`format-check` and `em-dash-check` passed. GPU render cases: 239 passed,
15 windowed skips. Together these cover every part of `just check`.

## Decisions requested

- Weather redesign: approve or amend
  [the proposed horizon layer and cloud profiles](ZERO_WEATHER_REDESIGN.md)
  before that redesign is implemented. The roadmap explicitly requires this.
- Replacement of an actively played authority: keep the live run and apply the
  replacement after Stop, restart scripts and clients immediately, or retain
  the existing refusal while preserving unrelated runs. Reset currently creates
  a fresh paused store; a live VM borrows its current store. These choices have
  different observable behavior and cannot be inferred from preservation alone.

- SSAO cost: keep the full-resolution 12-tap pass, or accept half resolution
  with depth-aware upsampling, fewer taps, or temporal accumulation. Each
  changes the authored image, so it is not done silently.

- Escaped-component signatures between ticks: TornadoSim spends 11.9 ms per
  presented frame re-hashing components a mutable pointer escaped from
  (physics `Transform`). Keep that conservative rule, or reuse a signature
  between ticks while the mutation version and a new escape epoch both
  repeat, which trusts that no pointer retained from a tick writes later.

- Physics caps for stress demos: `Cube.luau` (10,000 blocks) and
  `StressPhysics.luau` exceed the uncalibrated default work and row caps and
  fault on their first tick. Raise the caps for those demos, change the
  defaults, or keep them as budget demonstrations.

- Build turnaround: approve or amend
  [the measured plan](ZERO_BUILD_TURNAROUND.md). A `Store.hpp` edit
  recompiles 762 of 3,008 objects; a forward header is the proposed first step.

- Instance ids across Stop: Stop rebuilds the world from its snapshot, and
  the new store reuses id numbers, so a control client's pre-Play id silently
  names a different instance afterwards. Keep it, make Stop restore the
  original ids, or have the control tools refuse ids from a previous store
  incarnation.

Independent work continues while these decisions are pending.

## Profiling provenance

Initial captures use the pre-change `dev` Studio, Vulkan on RTX 4090, 1280x720,
360 uncapped presentations, isolated configuration directories, and the normal
default workspace. Demo captures add their named world and start Play.

These initial captures include default Rings, MeshGrid and Hallway worlds and
replicas. The files labelled `empty-before` do not establish a truly empty
workspace, and same-named span aggregates do not isolate Magic or any one world.
They are diagnostic whole-workspace captures, not accepted isolated baselines
or shipping-speed claims. GPU sample counts in the last five-second window are
too small for tail acceptance. Use isolated scene fixtures and matching state
for subsequent acceptance measurements.

An additional Magic control uses `defaultWorlds: ["magic"]`, without `--demo`,
and 480 presentations. This selects one authored authority and its Play replica.
Its late-window diagnostic identifies CPU collection and residency as major
work; its small GPU sample count still does not establish tail acceptance.

Diagnostic profiles and test logs are under `.cache/build/dev/zero-v0261/`.
Benchmark results stay on stdout. No benchmark result files are added.

## Unfinished milestone documents

The generic unfinished-work item includes reconciliation of
`V0261_IMPLEMENTATION_PLAN.md`, not just the individually named Zero hotspots.
Later evidence supersedes historical statements about B0 and missing GPU wait
proof. Full optimization pass 2 and pass 3 coverage, current platform and visual
evidence, and final acceptance boxes still need current-state verification.
Do not blindly resume an old controller after schema or pacing changes.
