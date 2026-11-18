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
| Worker tick housekeeping labels | Committed, focused tests passed | Commit `39991be9`. World Universe/TickExchange selection passed 3,245 assertions in 63 cases. Real demo attribution and remaining optimization work still need verification. |
| Preferences and dock tab scrolling | Complete and pruned | Focused DockLayout/Tools checks passed 3,155 assertions in 170 cases, including overflow, wheel navigation, page scroll retention and fixed tab strips. Final milestone visual checks remain tracked separately. |
| Built-in node dispatch | Committed, non-GPU tests passed | Commit `19f50dcd`. Explicit per-recording context, immutable member descriptors and custom callable copy semantics. Render non-GPU suite passed 918 cases. GPU output and runtime frame checks pending. |
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
