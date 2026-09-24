# Work handoff

Updated 2026-09-25. This records the unfinished work in the main `v0.25`
checkout. The worktree cleanup is complete; only the main checkout remains.
The detailed worktree history below is archival. This is a working note, not a
completion claim.

## Latest verified status

- The virtual camera lock has a passing combined live Studio capture at
  `.cache/build/dev/studio-virtual-camera-capture-20260924-201453`.
  `just studio-virtual-camera-capture` exercised the View menu under Xvfb,
  moved the inspection camera with fixed orientation, retained the behavior
  signature for culling, Auto LOD, light, particles, and portal demand, then
  checked recapture and unlock. The viewport image changed as expected.
- The final dev Vulkan render gate on the current tree failed 2
  `PortalImageHost` cases, with 8 failed assertions out of 1,679,140. The run is
  recorded in `/tmp/atomic-render-final-current.log`; a focused fix is underway.
- MCP real Vulkan capture drain passed 94 assertions. Three paired release
  control and GPU runs were measured under normal desktop load with Brave,
  Discord, Spotify, and Steam open, as the user requested. The results and
  timing spread are in `docs/v025-MCP-PERF-EVIDENCE.md`. Server and CDN
  headless artifact checks passed.
- On 2026-09-25 the uncommitted work was committed in topic order, portal
  first. The committed tree passed every `just preset=ci check` step except
  `studio.renderpipelinegraph`, which read the node catalogue before
  registering it and failed when run alone or first. `6a468a42` fixes it. One
  full broad CI rerun on the final tree still closes the MCP item.
- The portal acceptance matrix now runs end to end (`f28827fe`):
  `just portal-product-acceptance` covers product walks at 144 and 240 FPS, a
  variable frame and stall schedule, the 24-cell RTT, jitter, loss, duplicate
  and reorder grid (`PORTAL_IMPAIRMENT_ROW=rtt,jitter,loss` reruns cells),
  matched seam captures and uncaptured `--frame-timings` series. The old
  60.58 FPS reading was capture I/O; uncaptured, the seam scene holds
  240.0 FPS at 1080p.
- `676df971` fixes a latency livelock. A camera route that started before the
  replica learned its authored name kept the local name, lost its world every
  frame once the identity arrived, and churned eye slot 2 so no eye reply
  could land at 150 ms RTT or more. Portal views now report
  `PortalPresentation::AwaitingImage` when a demanded image has not arrived.
- Open blocker: the product walks are flaky with or without impairment. The
  strict 30/60 handoff failed 3 of 3 runs on the tree from before the
  2026-09-25 portal changes, so the 2026-09-24 pass does not reproduce. The
  clearest failure is a third crossing: after the return the body rests
  behind the source pane and is transferred back about 44 frames later
  without moving. Diagnostic logging hides it. The v0.26 roadmap item tracks
  the trace and fix. After the livelock fix, every 150 ms grid cell adopts,
  but most cells still fail on black frames, single adoptions or camera
  checks.
- Portal transport now carries explicit request-scoped transfer-eye priority.
  The server selects urgent per-endpoint heads ahead of routine replies and
  limits routine stream backlog. Focused world, server, and render runtime
  tests pass.
- `ROADMAP.md` tracks these completed and open gates. The Pixel Composer Audio
  Window static slice is in place; the larger M0 to M7 work remains open. The
  five PXC projects and GIFs are under
  `/home/declan/Documents/GitHub/atomic-game-engine-hidden-docs/files/pixel-composer-com`.
  No licensed executable is available, and the user directed GIF-based
  reconstruction of all features.

## Current direction

- Finish the camera, render, MCP, and portal gates before claiming their
  current work complete. Keep the larger stress and Pixel Composer items open.
- Merge a completed branch into `v0.25` when safe, as the user requested.
  Only the main worktree remains; its work was committed on 2026-09-25.
- Review ownership before editing shared files or committing. Do not commit
  changes made by another agent or the user.
- Follow the repository `AGENTS.md` and each touched module's `AGENTS.md`.
  Use CodeGraph first when `.codegraph/` exists. Read `RUNNING.md` before
  choosing commands. Never edit the protected instruction documents.

## Main checkout

Path: `/home/declan/Documents/GitHub/atomic-game-engine`, branch `v0.25`,
The priority phase benchmark was integrated as `5e30f523`. This handoff and
the portal test fixture fix were committed afterward. Current task status is
in the latest verified status above and `ROADMAP.md`.

### Prior checkpoint and historical next steps

The list below records an earlier checkpoint. Use the latest verified status
above for present gate results.

1. **Virtual camera lock.** A free inspection viewport uses the live camera;
   culling, LOD, lighting, particles, and portal demand use the frozen behavior
   position with live orientation. This matches the user's choice. Focused
   checks passed earlier. Inspect any other camera behavior covered by the
   original broad claim, then run the relevant combined gate.
2. **Render pipeline cleanup.** Real GPU render gate previously passed
   1,678,712 assertions in 150 cases. Later history, scope, deferred host,
   supersession, and cancellation gates passed. Two real host fixtures for an
   incompatible binding and viewport removal passed after correcting fixture
   premises. The attempted staged layer import refusal fixture failed its
   pending upload premise because an invalid layer is rejected before upload.
   It now checks that preflight refusal and last image retention; the focused
   Vulkan case passed 20 assertions. An actual failure after staging is still
   untested. Review remaining gates in
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
- Commits in this handoff session include `5e30f523`, which preserved the
  original benchmark commit's author and message and contains only
  `mono.engine/replication/benchmarks/PriorityRefinement.cpp`; `0598d7ce`,
  which contains this handoff and the portal lighting test fixture cleanup;
  and `4d2d8175`, which updates the handoff after worktree cleanup and GPU
  validation. Later handoff edits may be uncommitted.
- Web reference for worktree removal semantics:
  <https://git-scm.com/docs/git-worktree>.
