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
| Weather Play hotspots | Partly done, redesign pending | Commits `b3b5481e`, `43a6bf13`, `a65588a8`. Isolated Play, Debug, 600 frames: frame mean 8.40 to 7.54 ms, gui compile 1.013 to 0.193 ms (source filter set), replica-gui-services 0.525 to 0.289 ms per tick (PlayerGui source walk reuse), Authority::PublishMany 0.69 to 0.73 ms (reported 1.1). Remaining cost is WeatherCompute: weather-residency 3.5 ms CPU and gpu weather-integrate 3.4 ms, which wait on the approved redesign. |
| Frame graph layout | Measured | Commit `262eec3b` names its steps. Animation Play with the graph open, every-frame publish: layout 0.30 ms (gaps 0.11, rows 0.10, copy 0.03, fit 0.03, flame 0.03). Skipping an already-sorted gap sort changed nothing and was reverted. What remains is building and placing about a thousand string-named `DiagnosticSpan` records per publish in Debug. |
| Preferences and dock tab scrolling | Complete and pruned | Focused DockLayout/Tools checks passed 3,155 assertions in 170 cases, including overflow, wheel navigation, page scroll retention and fixed tab strips. Final milestone visual checks remain tracked separately. |
| Built-in node dispatch | Complete and pruned | Commit `19f50dcd`. Render non-GPU suite 918 cases and GPU suite 239 cases passed (15 windowed skips). Isolated empty-scene capture no longer has a `build node table` span (whole-workspace baseline 0.389 ms mean). |
| Adornment drawing | Complete and pruned | Commit `cd46e87d`. Adornments demo in Play, Debug, 600 frames: overlay drawing 0.908 to 0.280 ms mean, adornment drawing 0.366 to 0.061 ms, geometry 0.189 to 0.111 ms. `viewport-adornments-bench` full uncached vertex, UV, colour and index oracle passed for every row; at 512 boxes static 0.254 ms vs moving 0.549 ms in bench. |
| Changed-row presentation damage | Committed, tests passed | Commit `a1bc1b64`. Damage checks read changed rows through const access, so observed presentation columns no longer escape every frame. |
| Factory Play orchestration | Committed, tests passed | Commit `bf8f947c`. Studio non-GPU suite passed 1,063 cases, including factory and Play pause interplay and an unrelated Play run keeping its VM, cadence and replicas. Targeting an actively played authority awaits the user policy choice below. |
| Scheduler preservation | Committed, tests passed | Commit `d6128093`. `SystemLifetime` Portable/WorldBound and `PreserveForWorldReplacement` cover package transactions and DataFactory restore, backward seek and intervention rollback, refusing live scheduler edits made during preparation or abort. Script and physics cadence clocks ride in the snapshot. ecs, script, world, client, studio, examples, game and server non-GPU suites pass. Full gate pending. |
| Other Zero entries | Open | No completion inferred from source inspection or the earlier publishing gate. |

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
