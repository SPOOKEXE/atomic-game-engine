# ME-NEXT optimization work

The serial acceptance results below are a historical snapshot. A separate
parallel correctness follow-up is in progress. The original input matrices,
physical ticks, product traffic, fault modes and exact output contracts remain
in place. `ME-NEXT.txt` is unchanged.

## Earlier serial outcomes

| Case | Source | Final outcome |
|---|---|---|
| `representative cell-size scenes preserve exact output in both orders` | `mono.engine/physics/tests/CellSizes.cpp` | PASS; `215.860 -> 211.612 s` case |
| `cell size preserves ordered contacts and body state in stress scenes` | `mono.engine/physics/tests/CellSizes.cpp` | PASS; `193.814 -> 184.487 s` case |
| `actual replication stress keeps its client camera above the moving field` | `mono.studio/tests/ReplicationStress.cpp` | PASS; demo `112.262 -> 92.679 s` |
| `all twenty thousand late movers acquire their complete creation state` | `mono.studio/tests/ReplicationStress.cpp` | PASS; late movers `92.612 -> 67.855 s` |
| `portal image handoff under round trip, jitter, loss, duplicate and reorder impairment` | `mono.client/tests/PortalWalk.cpp` | PASS; all 24 rows, `597.24 s` failed baseline to `579.50 s` final |
| `Client walks through product portal images and returns` | `mono.client/tests/PortalWalk.cpp` | PASS; fixed-before `229.81 s` to final `207.21 s` wall |
| `client product serves a saved or replicated world over its inherited driver link` | `mono.client/tests/PresentationHost.cpp` | CI PASS; 8 configurations, 531 assertions, `25.48 s` |
| `bundled GUI fonts load on text demand and render the same after delayed authoring` | `mono.client/tests/CharacterStartupRender.cpp` | PASS; `3.804 -> 2.727523 s` case |
| `an unjoined two-world capture sequence writes every image with one claim per portal` | `mono.client/tests/NonEuclideanRender.cpp` | PASS; all 24 pairs, `1.965 -> 1.925440 s` case |
| `two renderers exchange destination radiance through owned world presentation traffic` | `mono.engine/render/tests/portal/PortalImageRuntime.cpp` | PASS; 97 leaves, 8,880 assertions, final CI `26.14 s` |

These are single dev measurements under host contention. They are useful for
test-cost comparison, not statistical or release-performance claims. Release
profiling is N/A because this work makes no shipped speed claim.

## Changes and evidence

The baseline was `44f70b22`, following docs-only `63b42f4b`. Dev used GCC `-O0`,
Debug symbols and heap hooks on a Ryzen 9 9900X, RTX 4090, driver `580.173.02`,
Vulkan. The original ten-case times used eight workers with other workloads
active. `perf` sampling was unavailable at `perf_event_paranoid=4`; host settings
were not changed.

Physics changes are limited to private `Solve.cpp` AngularResponse scalar
accumulation and exact fresh reserves in `tests/fixtures/CellSizeParity.hpp`.
There is no persistent buffer reuse, and the emitter, API, axis order, float
grouping, iterations and all 4,000 bodies remain unchanged. The first case keeps
3 layouts x 3 cell variants x 23 ticks; the second keeps 4 layouts x 2 orders x
4 worlds x 23 ticks. Both final cases passed all 920 assertions. Combined wall
time changed `410.00 -> 396.41 s`, user `627.18 -> 571.99 s`, system
`11.04 -> 8.20 s`, RSS `8256052 -> 8220336 KiB`. Scalar-only and exact-reserve
cross-binary runs emitted identical 460 lines and 9,286,118 bytes of ordered
pairs, manifolds, events, bodies and warm-start impulses. Focused solver/floor
checks passed 2,489 assertions across 21 cases; solver groups passed 58,150
assertions across 24 cases. The one-off profile source is archived at
`.cache/build/dev/cell-size-profile.patch`.

The Studio visibility batch uses const Store `SelectVisibleToPlayer`, resolving
up to 10 allowed service classes and the player class per batch. It reads live
ECS hierarchy, scope, locality and owner state, retains scalar early exits, and
adds no persistent cache. The furnished branch selector and direct transient
exclusion remain; bare behavior is unchanged. The change touches Services
header/source/tests and Studio PlayLink/ReplicationStress. Focused Services
passed 702 assertions across 22 cases; replication interest/structure passed 78
across 7. Final Studio normal tests passed 60,251 assertions. Combined wall
time changed `205.26 -> 160.80 s`, user `232.76 -> 177.80 s`; 179 discarded demo
completeness scans were removed while retaining 180 ticks, 20,000 movers,
moving writes, assertions and timeouts. Focused PlayLink/GUI passed 5,873
assertions across 38 cases.

Matched late profiles preserved 260 steps, 219 moving passes, 36 convergence
passes and 60,007 assertions. Fixture time changed `90.536206 -> 67.620793 s`
(-25.3%). Interest producer work changed `42.075694 -> 20.160555 s` (-52.1%),
owner Steady `52.286769 -> 30.418348 s`; signing and delivery stayed similar.
There were 10,948 deltas, 167 structure calls and zero drops. Producer spans
represent worker work, not additional serial wall time or owner self time.

PortalWalk fixed-before and final 16-row cohorts used the same fixed children,
input matrix, physical ticks and clocks. The final binary SHA256 is
`acec4e821d06e922985dcd08db07dcb0fa20a2bc270ce460620adabdaa088412`. Dynamic
assertion totals vary with polled state; no assertions were removed. The 24-row
impairment matrix passed 510,882 assertions in `579.50 s` wall, `723.80 s` user,
`92.84 s` system and `296932 KiB` RSS, versus one failing assertion in 507,388
and `597.24 s` wall on fixed-before. The final row passed in `32.818 s` with two
adoptions, 6,780 arrivals, 369 drops, 52 duplicates, 80 reorders and 6,463
delayed deliveries; awaiting-image-body was 50 frames and first-contact 127.
A guarded projected-head fallback runs only when the old bright count is zero;
original brightness, adoption, fault and frame checks remain. The final log is
`.cache/build/dev/me-next-impairment-final.log`.

Font capture ran four cold clients for 40 frames each. The two empty-client runs
retain 60 FPS; only the two text-capture runs uncap wall-clock pacing. Capped and
cap-zero output retained all 80 BMP/JSON pairs per variant; all BMP bytes and
frame/tick/alpha values matched, with text on ticks 1 through 40. Each crop
retained full 24,320-RGB-pixel equality. The capture delta remains exactly
`1.0f / 60.0f`. JSON wall seconds may differ by design. Serial and concurrent
GPU A/B runs both passed all six cases with unchanged binary hashes. Makespan
was `34.371845 s` serial and `31.644363 s` concurrent (-7.94%), while per-case
latency increased. This single overlapping CPU/GPU run does not support
serializing GPU work. Evidence is at
`.cache/build/dev/evidence/me-next-contention-2026-10-18T13-37-28-944Z/summary.json`.

The unjoined capture still produces all 24 BMP/JSON pairs. No source change was
justified. Radiance fixture handling now normalizes BGRA UNORM to RGBA and only
applies sRGB encoding for sRGB formats. All original radiance tolerances, traffic
and ownership checks remain; 375 duplicate resident-alias registration
checks are deduplicated. The full 97-leaf, 46-section case passes 8,880
assertions. Its `26.62 -> 26.35 s` dev timing is within noise and does not
justify a production performance change.

Host cadence is unchanged; its dev process time changed `25.50 -> 25.47 s`.
The two live-replica disconnect rows exercise real network timeouts
(`10.701 s`, `10.700 s`); saved rows take about `0.63 s`. The final CI host
suite passed one Catch test case with 8 generated configurations and 531 dynamic
assertions in `25.48 s`.

## Historical CI verification snapshot

The final `just preset=ci check` exited 0: all 774 suites passed, with zero
skipped and zero failed. The check covered formatting, em-dash policy, build
profile, compiler cache, build, tests, architecture, source, shader, bindings,
typecheck/editor, determinism, replay, client smoke and orphan checks. Log:
`.cache/build/ci/me-next-check.log`.

Final CI GPU runs passed: fonts 47 assertions in `3.06 s`, unjoined capture 199
assertions in `2.26 s` with all 24 pairs, and radiance 8,880 assertions in
`26.14 s` across all 97 leaves. Logs are under `.cache/build/ci/` as
`me-next-final-gpu-{fonts,unjoined,radiance,presentation}.log`. At that snapshot,
the code was frozen and targeted matrices were green.

## Parallel correctness follow-up

The user dropped the under-60-second target and selected three complete groups
concurrently. They ran using dev `-O0`; all three passed in a
combined `333.4828 s`:

| Group | Result | Wall time |
|---|---|---:|
| Physics | 920 assertions across 2 cases | `333.4215 s` |
| Movers | 60,251 assertions across 2 cases | `126.2510 s` |
| Portal | 122,851 assertions across 1 case, 16 rows | `181.9890 s` |

Evidence: `.cache/build/dev/parallel-groups/summary.json` and
`.cache/build/dev/parallel-groups/portal-coverage.json`.

The portal case covered exactly 16 configurations, each producing 600 BMP and
JSON pairs, 9,600 pairs total. Each row ended in `server.world`; no row needed
extra frames. The fixture uses original frame budgets as minimums, a 180-second
wall guard and a capture
ceiling of `180 * captureFPS`. It checks every actual frame and requires 32
settled native return frames, all three camera/mode/input milestones, and stage
2 movement of at least 13. It fails on incomplete runs. Product deadlines remain
1 and 5 seconds. Physics and mover cases have no speed assertions. Predicate,
skin and steering tests passed 41 assertions across 3 cases.

These parallel timings are observations, not a controlled speed comparison to
the serial snapshot. Host load differed and the product client was updated.
Portal producer diagnostics were enabled during this run. A prior four-lane
`65.99 s` prototype covered all 16 rows but failed 23 assertions, so it does
not validate variant parallelism or prove a timeout fix.

The remaining 44 shared portal rows finished 13 cases: 12 passed and 1 failed,
with 780,798 of 780,799 assertions passing in `833.48 s`. They covered 44 unique
configurations and 43,140 BMP/JSON pairs. Every row met its exact original frame
minimum without extensions; each ended with 32 native `server.world` frames and
no handoff. Coverage is recorded in
`.cache/build/dev/portal-shared-remaining44-coverage.json`. All 24 impairment
rows passed.

The sole failure is unresolved lens rendering/oracle behavior. The captured
check expects the blue channel to exceed twice the red channel for the portal
view at frame 144, but saw gray RGB `(177, 177, 177)`. An isolated
current-binary rerun failed the same check at frame 87. Captures and
counterfactual evidence are under
`.cache/build/dev/portal-lens-counterfactual/`. An ABBA comparison used the
same 600-frame artifact, path, working directory, children and hash. Legacy
stopping failed once at frame 155 and passed once. One new-stopping run failed
at frames 158 and 159; the other passed. Both policies can pass and fail, so new
stopping logic is not necessary for the failure, and historical presence is
unproved.

The assertion and captures are retained. No product fix or tolerance change was
made. The temporary diagnostic flag is absent and final source was restored
byte for byte. The final `test_client` hash matches the original measured
binary: `75e23a2b5308c9e57b246757bdfd81ff030187743292575ac79dd81dcd08d656`.
Children were unchanged, the final build passed, and predicate, skin and
steering checks passed 41 assertions across 3 cases. The diff check is clean.
The historical full CI snapshot above was not rerun for this fixture follow-up.
