# Work handoff

Updated 2026-09-24. This records the unfinished work in the main `v0.25`
checkout and the linked worktrees. It is a working note, not a completion claim.

## Current direction

- Finish the smaller worktrees first, one at a time. Keep Pixel Composer moving
  where it has an independent path. Handle larger remaining items afterward.
- Merge a completed branch into `v0.25` when safe, as the user requested.
  Patch equivalent branches need no merge. The main checkout has extensive
  uncommitted changes, so inspect overlap before any merge.
- Preserve every dirty worktree until its changes are understood. Do not use
  `git worktree remove --force`, `git clean`, or `git reset --hard` to make a
  worktree look finished.
- The main checkout contains concurrent uncommitted work from several tasks.
  Review ownership before editing shared files or committing. In particular,
  do not commit changes made by another agent or the user.
- Follow the repository `AGENTS.md` and each touched module's `AGENTS.md`.
  Use CodeGraph first when `.codegraph/` exists. Read `RUNNING.md` before
  choosing commands. Never edit the protected instruction documents.

## Main checkout

Path: `/home/declan/Documents/GitHub/atomic-game-engine`, branch `v0.25`,
The priority phase benchmark was integrated as `5e30f523`. This handoff and
the portal test fixture fix were committed afterward. The active goal is the
six items in `/home/declan/.codex/attachments/ccb8ba64-ca25-4eaf-a502-18231242fbeb/pasted-text-1.txt`:
virtual camera lock, v0.25 render cleanup, v0.25 MCP cleanup, engine stress
and optimization audit, seamless portals, and Pixel Composer. All remain open.

### Verified work and next steps

1. **Virtual camera lock.** A free inspection viewport uses the live camera;
   culling, LOD, lighting, particles, and portal demand use the frozen behavior
   position with live orientation. This matches the user's choice. Focused
   checks passed earlier. Inspect any other camera behavior covered by the
   original broad claim, then run the relevant combined gate.
2. **Render pipeline cleanup.** Real GPU render gate previously passed
   1,678,712 assertions in 150 cases. Later history, scope, deferred host,
   supersession, and cancellation gates passed. Two real host fixtures for an
   incompatible binding and viewport removal passed after correcting fixture
   premises. A staged import refusal fixture is syntax clean but has not had
   its linked Vulkan gate. Review remaining gates in
   `docs/v025-RENDER-PIPELINE-CLEANUP.md` before closing this item.
3. **MCP cleanup.** Stages 0 through 5 and the M6 PNG bundle tool are present.
   Focused Runner, CLI, architecture, listener, bridge, and 128 cycle soak
   checks passed. Quiet paired HEAD/current release timing and broad CI are
   still open. The HEAD comparison archive is
   `/tmp/atomic-mcp-quiet-3c4i6u88`; current bench binaries need building.
   See `docs/v025-MCP-CLEANUP.md` and `docs/v025-MCP-PERF-EVIDENCE.md`.
4. **Stress work.** `docs/ENGINE_STRESS_AUDIT_2026-09-22.md` lists at least five
   optimization candidates per engine module. Exact parity checks passed for
   replication fixed width deferral, physics solver topology comparison
   removal, and MSL reserve adjustment. A matched 200 client Authority
   `RecoverRows` rerun is still needed after quiet profiling. The prior
   baseline had 200 Playing clients and 194,936 inputs, but its profiler
   dropped 73,246 scopes, so its sampled 41.06% share is partial evidence.
5. **Seamless portals.** Scene pose invalidation and local rig demand fixes
   passed focused tests. Strict product captures at 30 and 60 Hz still fail.
   The 60 Hz resident slot request is received and the first crossing commits
   at frame 75, then frame 77 lacks the predicted destination root for one
   frame. A red `Replicated` adoption test was linked; implementation was
   being compiled when agent capacity ended. Check `mono.client/src/Replicated.cpp`
   and `mono.client/tests/Replicated.cpp`, run the focused prediction adoption
   gate, then rerun the strict 30/60/144/240 matrix, network, resolution,
   profiling, and GPU gates. The 60 Hz trace is under
   `.cache/portal-profile/remote/runs/20260924-resident-slot1-60`.
6. **Pixel Composer.** The inventory in `docs/v026-pixel-composer.md` has 990
   documented node rows. Five PXC projects and reference GIFs are at
   `/home/declan/Documents/GitHub/atomic-game-engine-hidden-docs/files/pixel-composer-com`.
   There is no licensed Pixel Composer 1.21.10 beta executable; the user chose
   GIF and documented behavior as references and requested all features.
   Native graph, Studio, PXC interchange, selected 2D nodes, Transform Image
   3D, GIF timing, audio capture, and sequence bake work is partial. Do not
   claim full product parity.

### Pixel Composer details that need ownership

- Five spatial warp nodes passed `test_imagegraph '[imagegraph]'`: 1,966
  assertions in 221 cases. Mirror, Barrel Distort, and Chromatic Aberration
  supplied PXC records still need guarded authored control import projection.
- Bounded audio capture parser passed 56 assertions in 6 cases, including
  aggregate sample cap refusal before allocation. Audio Window, WAV import,
  live device input, audio keyframes, and feedback history remain open.
- `.aseq` 257 to 4096 frame bake and exact variable GIF delays passed asset,
  bake, and assetc suites. The scene/client/private playback adapter was
  staged and syntax checked. The linked `test_scene`, `test_render`, and
  `test_client` build passed; focused sequence checks passed 51, 34, and 11
  assertions respectively. Renderer texture publication remains open. Per
  particle age playback needs separate GPU paging.
- Native Transform Image 3D records a bounded pass into the existing batch
  command. The focused Vulkan recorder case passed 19 assertions. The
  recorder preserves resources until the submit fence, including a failed
  render pass after copy commands. A later uncommitted edit now makes
  `PollSceneFrames` call `TextureTable::ReplaceAdopt` for successful completed
  generations, while older, cancelled, and failed slots are released. The
  focused GPU test now checks exact owner/name publication, replacement by a
  newer generation, and cancellation retaining the previous output. That test
  initially exposed reversed `Succeeded` assignments in the untracked live
  recorder; the assignments were corrected and the Vulkan case passed 35
  assertions. Add failed pass, owner retirement, and GPU accounting checks
  before calling the edit complete. `QueueTransformImage3D` still has no product
  caller outside tests; `client/ImageGraphRuntime.cpp` uses the synchronous
  `ExecuteTransformImage3D` adapter. Wire the resident path into graph output
  publication before claiming native graph playback. These renderer source and test files remain
  untracked alongside the previous agent's implementation, so they were not
  included in the handoff commit.
- Source ownership from the prior parallel work is no longer active because
  those agents hit a usage limit. Before resuming, inspect the live worktree
  and build processes rather than assuming a gate completed.

## Linked worktrees

The last audit found 27 linked worktrees, all dirty. All 27 have since
been closed, leaving only the main checkout. `git worktree prune --dry-run --verbose` found no stale
records. Twenty four clean idle `/tmp` worktrees had already been safely
removed. The table gives tracked and
untracked status counts, not an assessment of the changes. Start with the
smallest effective diffs and classify build or submodule pointer noise before
changing source.

An initial classification found 21 worktrees with no top level source changes,
only `mono.vendor/*` status. Several vendor checkouts contain staged deletions
of their own tracked files, while others only point at a different submodule
commit. Do not assume these are clean from the top level diff count. `git
cherry v0.25 HEAD` showed the authored transmission branch's five commits
were patch equivalent to the main branch. Its sole top level difference was
a clean `ngtcp2` checkout at a different commit. Its worktree was removed;
the `feature/authored-transmission` branch remains. `studio-qa`, `tornado-world`,
`material-capture`, and `lighting-effects` were also removed after confirming
no top level source changes, clean nested vendor repositories, and no commits
unique by patch to `v0.25`. Ten further vendor-only worktrees were removed
after confirming their nested status contained only staged deletions of
missing vendor files, and their commits were patch equivalent to `v0.25`.
Their branch refs remain. The other branches need individual review before
removal or integration.

The scoring worktree's unique priority phase benchmark commit was integrated
into `v0.25` as `5e30f523`. `bench_replication` built and its priority
refinement suite ran once, printing per-phase samples and a benchmark row.
Its remaining commits were patch equivalent, so its worktree was removed.
The CDN grouper optimization's source and test were byte identical to `v0.25`
and its focused suite passed 56 assertions in 14 cases. Its attempted matched
baseline build failed at CMake regeneration because that worktree's SDL vendor
checkout was missing. Its source was restored afterward. Both CDN worktrees
were removed after confirming the optimized one contained only duplicate
source and vendor symlinks, and the baseline one contained only vendor file
deletions. A matched timing comparison remains unavailable.

The tornado GPU particle authoring and storm structure damage branches were
reviewed against the current implementation, which contains their behavior
and tests. The rebuilt scene registration suite passed 319 assertions in 13
cases; the rebuilt physics storm suite passed 30 assertions in 7 cases. The
tornado visual parity branch's 16 volume cap and layered stack test are also
in `v0.25`. Its older example camera and panel values were superseded by a
newer TornadoSim scene; current volume and example tests passed 33 and 143
assertions. These three worktrees were removed, with branch refs kept.
Debug panel close controls and click routing are in `v0.25`; its panel suite
passed 122 assertions in 43 cases. The detached debug worktree was removed,
and `codex/archived-debug-panel` now preserves its unique commit.
The lighting capability branch's selection logic, shader cap, release stress
target, and tests are present in `v0.25`; its worktree was removed with the
branch ref kept. The remote local play worktree contained only formatting
changes beyond its patch equivalent commit, so it was also removed with the
branch ref kept.

The local light branch's per-light response planes, control bridge, and tests
are present in `v0.25`, which also has shadow visibility capture. The focused
render data capture suite passed 134,223 assertions in 45 cases. Its 295 MB
untracked vendor backup was moved to `/tmp/atomic-local-light-vendor-backup`,
then the worktree was removed with its branch ref kept. The tornado cap worktree contains an
older all-parcel shader experiment, while `v0.25` now uses bounded depth-sliced
condensation. Preserve its capture script and shader diff until the current
visual parity result is assessed. The portal worktree's host-only Ready fence
change weakens the replica check and was not merged. The main checkout has a
newer source/destination fence condition and a destination prediction seed;
the focused portal adoption test passed 12 assertions in one case.
The detached portal worktree's source diff was saved at
`/tmp/atomic-head-portal-source.patch`, its base commit is preserved by
`codex/archived-head-portal`, and its worktree was removed. The older harness
may still offer useful real transfer coverage, but needs porting to the
current test and cannot justify the host-only fence change.
The tornado cap worktree's source and binary world diff is saved at
`/tmp/atomic-tornado-isolated-cap-fix.patch`, and its capture script is at
`/tmp/atomic-capture-tornado-isolated.sh`. Its branch ref remains. The current
TornadoSim example and GPU particle tests passed 143 and 126 assertions. The
worktree's all-parcel shader and 2.1 million triangle capture threshold do not
fit the newer bounded depth-sliced path, so its uncommitted experiment was not
merged. Visual parity remains open.
The broader `test_client '[client][portal]'` selector initially exited 42:
six cases passed and two GPU cases failed during client initialization because
`PortalLighting.cpp` left a process wide assets override set to
`.cache/build/dev/assets`, where shaders are not staged. The fixture now
restores its prior assets path. After rebuilding, the same selector passed
422,745 assertions in eight cases. The strict 30/60/144/240 product matrix
remains open.

The primary checkout's unfinished product work continues below. Archived
patches are reference material, not accepted code.

## Last verification and environment

- `git diff --check` in the main checkout and `git diff --cached --check` for
  the handoff and portal fixture fix passed at 09:23 UTC. The staged priority
  benchmark passed `git diff --cached --check` before integration.
- At 09:14 UTC, the filesystem had about 114 GiB free. Avoid duplicating full
  build trees without checking space first.
- Three commits were created in this handoff session. `5e30f523` preserved the
  original benchmark commit's author and message and contains only
  `mono.engine/replication/benchmarks/PriorityRefinement.cpp`. The second
  contains this handoff and the portal lighting test fixture cleanup. The third
  updates the handoff after worktree cleanup and GPU validation.
- Web reference for worktree removal semantics:
  <https://git-scm.com/docs/git-worktree>.
