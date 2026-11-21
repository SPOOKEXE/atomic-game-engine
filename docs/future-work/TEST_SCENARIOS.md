# Shared test scenarios

## Objective

Run one compatible engine scenario once and let independent checks consume the
same simulation, physics, replication, rendering and scene observations. Checks
must run concurrently against immutable observations without obtaining access to
the running world. Preserve the existing test coverage, scenario variants,
source ownership, selection rules and test tiers while removing repeated setup,
unload and capture work.

This document records the shared-scenario implementation for v0.26.1: the
runner and reports, real observation adapters, and migrations of compatible
integration tests. A generic callback demonstration or a portal-only migration
does not meet the scope. Ordinary value-type unit tests do not need a world.

## Current status and evidence

The shared-scenario implementation and its in-scope migrations are complete.
The strict CI and full slow tier passed. No overall speedup is established by
the available timings. Appearance checks prove the supported cube cases only;
mesh, occlusion and object-identity claims remain outside their coverage.

| Check | Result | Evidence |
|---|---|---|
| Strict CI | `check-pr`, 781 suites, 0 failures | `.cache/shared-scenarios/strict-ci-presented-world-fix.log` |
| Full slow tier | 781 suites, 0 failures | `.cache/shared-scenarios/full-slow-presented-world-fix.log` |
| Development pre-commit | 781 suites, 0 skips, 0 failures | `.cache/shared-scenarios/portal-presented-world-fix-pre-commit.log` |
| CPU AddressSanitizer and UndefinedBehaviorSanitizer | 56 cases, 4,601 assertions, leak detection enabled | `.cache/shared-scenarios/client-observation-cpu-asan-presented-fix.log` |
| Native client scenes under AddressSanitizer and UndefinedBehaviorSanitizer | 5 GPU cases, 1,120 assertions | `.cache/shared-scenarios/client-native-scenes-asan-presented-fix.log` |
| Native parity under AddressSanitizer and UndefinedBehaviorSanitizer | 3,093 assertions | `.cache/shared-scenarios/client-file-owned-parity-asan-presented-fix.log` |
| Basic portal under AddressSanitizer and UndefinedBehaviorSanitizer | 99 assertions | `.cache/shared-scenarios/portal-basic-native-asan-presented-fix.log` |
| Shared foundation ThreadSanitizer | 18 cases, 154 assertions, run with `setarch`; scoped to the foundation | `.cache/shared-scenarios/shared-host-tsan-final.log`; this is not full client ThreadSanitizer coverage |
| Renderer GPU suite | 253 cases, 238 passed, 15 opt-in skips, 0 failures, 2,706,089 assertions | `.cache/portal-render-followup/render-gpu-after-final-fix.log` |

GPU leak detection was disabled for the native client sanitizer run because an
external library reports allocations during process exit. The foundation
ThreadSanitizer result is scoped to that target and does not establish full
client race coverage. Architecture, source and changed-file format checks passed.

### Migration and coverage inventory

| Area | Shared work and preserved coverage | Result |
|---|---|---|
| Tunnels cross-suite reuse | Three original cases use the same two sources. One 240-tick setup serves them; light-only advances one tick and wire-only advances none. | 33 cases, 18,478 assertions passed. |
| Tunnels owned frames | One setup serves 64 captures; owned GPU peak is 163,483 bytes. The helper CPU suite keeps its separate checks. | GPU case: 598 assertions. CPU helper: 3 cases, 59 assertions. |
| PortalWalk | All 14 product definitions and generated calls remain unchanged, plus four unchanged CPU checks. Six worker checks consume bounded current and previous observations. Body, head, blue-floor, shader, overlay and lens oracles remain. Property counts replace per-pixel decoder Catch checks. | Exact run: 18 cases, 60 native variants, 22,985 frames and 7,703 assertions passed. `.cache/shared-scenarios/portal-walk-stream-exact-full.log` |
| PortalSession | The real child stays active. Seven GPU probes and 15 outcome/residency oracles remain, including unobserved outcomes 0 through 9. The bounded request/summary/acknowledgement channel checks identity and budget and retains latest and latest-eligible records. | 10 cases, 17 outcomes and 2,365 assertions passed. `.cache/shared-scenarios/portal-session-owned-demand-final.log` |
| Character startup and fonts | Seven distinct native client lifecycles still run 40 frames. Five owned frame-39 captures replace successful BMP/JSON sequences. Font payload, crop, heap and teardown checks remain. | Two named GPU cases passed; original per-pixel decoder checks were removed. |
| Native angle tour | Seven authored shots run through one initialized client and retain shot identity, settling and image checks. | One setup, 112 frames, 7 captures, 1 case and 165 assertions passed. |
| Generic appearance checks | Cube appearance was checked in Tunnels and the native angle tour. CPU image and channel checks supplement those cases. Original whole-image and fingerprint oracles remain. | GPU: 3 cases, 1,006 assertions. CPU: 13 cases, 480 assertions. |

PortalWalk retains the exact raw seven-pose identity check, without a fuzzy
comparison or epsilon. First-failure and terminal image pairs share a 32 MiB
artifact budget. Two leased frames and an independent terminal copy fit within
the combined cap. Terminal-only failures now retain a pair. The successful BMP
and JSON sequences are gone; generated artifact cleanup retained representative
evidence.

The wrong-world presentation defect was fixed by following the head seed,
settling at the return endpoint's coarse paired mouth, guarding with the
entry-plane normal rather than oblique motion, and removing fully crossed body
rows from the finite prism while retaining plane clipping. All 14 product
definitions and 60 native variants passed the full slow run after the final
fix.

The generic appearance helper currently covers supported cube appearance in
two real scenes. Meshes, near clipping and the old foreign-capture geometry are
explicitly unsupported; it makes no general occlusion or object-identity claim.

The old `FrameObservation` stale-origin-to-eye mapping is rejected as
`MissingWorldMapping`. Direct-candidate behavior, exact ROI and `SourceWorld`
provenance remain, and a valid same-world portal route is covered on CPU.
PortalSession outcome 16 previously reproduced an old/new source-bundle mismatch
(154 expected, 155 observed). The fix suppresses only known no-return previous
content demand when the ordinary eye is tracked. Held worlds, one-frame
`HoldDisplayEye`, unknown and third-world routes remain covered; untracked
outcome 9 still expects 1. The focused outcome-16 run passed all 166 assertions.

The PortalWalk, Tunnels and PortalSession migrations, selected native scene
checks, strict CI and full slow tier are complete for this work. Remaining
technical limits are the explicitly unsupported generic image cases above and
the scoped nature of ThreadSanitizer coverage. No broad timing improvement is
claimed.

### Character startup and font coverage

Character startup retains three named startup variants and four font variants.
All seven clients run 40 frames. The three startup variants use distinct
automatic, disabled and preloaded character fixtures at 160x120 with
`Uncapped=true` and `MaximumFrameRate=60`. Luau checks that the disabled
character is absent. Active characters must have one matching humanoid, the
expected camera subject, root X above 20
and camera distance below 30; the preloaded character must retain its exact
name. Frame 39 must cover more than half the image with the expected color:
automatic and preloaded require green above 140 with red below 80 and blue
below 100; disabled requires blue above 140 with red below 80 and green below
100. Three final owned captures replace their BMP/JSON sequences.

The four 320x160 font clients cover empty, immediate text, delayed text and a
separate empty teardown run. Text runs retain fixed 1/60 capture intervals;
empty runs retain 60 FPS wall pacing and do not subscribe to capture. All four
font payload files are checked. Heap samples are taken before construction,
after initialization, after the run and after explicit shutdown. Empty runs
require unchanged allocation and residency. Each text run bounds allocation
below three payload sizes, live residency at or above one payload, and released
bytes at or above one payload. Glyph checks use the 8,8-origin 304x80 crop,
count a pixel when any RGB channel exceeds 8, require more than 100 nonblack
pixels, and require immediate and delayed RGB crops to match exactly. Two final
owned captures replace their BMP/JSON sequences. Compact glyph RGB crops
remain available.

The seven lifecycles still initialize and advance 280 frames. The five capture
clients subscribe to owned frame-39 observations; the two empty font runs have
no observation subscription and keep separate teardown coverage. The original
200 image writes and 200 metadata writes became five native readbacks.
Assertions validate complete, tightly packed canonical RGBA planes and use the
same copied final facts for identity, color and glyph checks. Missing frame 39,
failed capture completion, mismatched provenance or retained image bytes fail
the run.

The recorded seed-1 SceneTick and PortalLighting baseline at `c1dc0e30` is 33
cases, 18,460 assertions and 5.46 seconds. Character startup measured 5.64
seconds before and 5.84 seconds after in single concurrent runs. These timings
show no measured speedup; the reduced readback and disk work is the concrete
change.

## Layer 1: reusable scenario execution

`Engine::testing` lives in `mono.engine/testing` at L3, shared tier, and depends
only on `Engine::core` and `Engine::parallel`. It owns typed scenario execution,
session reuse, observation bounds and independent named check results. It does
not name a store, renderer, authority, product class or Catch type. CPU,
replication and GPU tests use the same headless library without pulling runtime
adapters into each other's dependency closures.

The existing suite declarations remain in `mono.build/testmain`. That layer
adapts Catch and suite reporting to the engine runner. The module is registered
in `mono.engine/CMakeLists.txt` and the architecture graph with its shared tier,
layer and exact dependency closure. Its public behavioural headers have focused
suites and module invariants are documented. No lower layer depends on render
or a program.

The public contract has these parts:

- A complete `ScenarioKey` identifying the scene/content revision, initial
  configuration, deterministic seed, tick and frame schedules, ordered actions,
  transport, faults, capture configuration and observation schema.
- A typed driver owned by the calling adapter. It loads and advances the real
  runtime once for that key and submits completed immutable observations.
- Named checks with explicit observation requirements and source-suite identity.
  They return value findings and compact accumulated results.
- A scenario session with a declared observation and byte budget. Reuse is local
  to the selected process/run and the complete key, never a global promise that
  similarly named scenes are interchangeable.

Use stable strings in persisted identities. Do not persist `Name::Id()`, entity
numbers without their world identity, function addresses or pointer values.
Reject conflicting reuse of a key with incompatible configuration. Release
sessions and captured bytes after the group finishes, including failure paths.

The owner applies actions and advances the existing runtime in deterministic
order. It then publishes a completed observation batch and calls blocking
`Jobs::For` across the eligible checks. Each check owns its own result slot.
Join before the next mutation or destruction. The runner does not start or stop
the process-wide pool; that remains the test host's responsibility.

Retain only the bounded observations needed by the declared checks. Consume
frame checks as a stream and retain compact history or explicit checkpoints
for temporal checks. Do not replace repeated loading with an unbounded archive
of full-size BMPs. Expose actual setup, capture and check-dispatch counts and
timings so reuse can be demonstrated.

## Layer 2: execution groups, selection and reports

Suite metadata supports opt-in `TEST_SCENARIO_GROUP` declarations. Compatible
selected suites in the same binary may run in one child process to reuse
scenario sessions. Suites without the declaration preserve their current
execution model. A group is a scheduling hint; the complete scenario key still
determines reuse within it.

Keep every suite's identifier, source/header closure, `TEST_DEPENDS` and tier.
`TEST_DEPENDS` cascades through suite signatures for invalidation; it does not
order test execution. Do not merge source ownership merely because tests share
a runtime. Respect user filters, GPU exclusion and slow-case exclusion.
Selecting one suite must not silently run an unselected expensive peer or mark
that peer as passed. Different binaries cannot share a live runtime.

MonoReport v3 gives per-suite attribution for each case/check in a group and
continues to read legacy v2 reports. Malformed, missing or ambiguous
attribution is rejected. Suite totals come from cases that actually ran, while
shared setup cost is reported once.

Keep suite signatures and tier/GPU/slow cache distinctions correct. A relevant
driver, shared observation schema or check change must invalidate the selected
consumers that depend on it. An interrupted, failed or incompletely reported
group cannot create a green cache entry for its absent members. Preserve
selective reruns and failure diagnostics for individual suites.

## Layer 3: owned observations from the real engine

Adapters live beside the existing runtime owners or tests. They use the same
drivers that the product already runs. Do not introduce another simulation,
replication or rendering loop to make the harness easier to call.

| Owner | Existing mechanism to reuse |
|---|---|
| World | `Universe::Enter` after tick work joins; copied clock, scene and lifecycle facts. Existing data-factory stepping and snapshot barriers where that lifecycle is already under test. |
| Physics | `CopyPhysicsObservationRecords` and `OverwrittenPhysicsObservationRecords`. |
| Portal simulation | `CopyPortalObservations`, including stamps, trace identity and overwrite counters. |
| Replication | `Authority::SetObservations`, `Replica::SetObservations` and one owner draining `ReplicationObservations::Poll`. |
| Rendering | Ordinary native captures use `Renderer::RequestSceneCapture` and `SceneCaptureResult`, privately adapted through `FrameObservation`. Lifecycles already using `DataFactory` use owned `DataCapturePoll` planes. Copy camera/projection, snapshot/frame/world identity and object labels; copy `VisibilitySnapshot` where applicable. |
| Unified arrangements | Existing `Crossing`, `Harness` and imported module report types. |

`Enter` lends storage only while nothing ticks. Workers cannot retain a store,
scheduler, authority, renderer, borrowed span, device handle or mutation
callback. Replication's observation queue has one consumer: drain it once and
fan out the owned records. Preserve the original physics boundary and timing
identity rather than pretending all observations happened at one instant.

Required hooks must have explicit availability and completeness. Missing hooks,
unexpected drops or overwrite gaps, empty required samples, failed readbacks,
stale snapshot identities and incomplete coverage fail the dependent checks.
Optional or inapplicable features need an explicit declared outcome; absence is
not a successful observation. One check's exception or failed requirement must
not prevent unrelated checks from producing their findings.

Report findings through Catch on the calling thread after worker joins. The
scenario library itself contains no assertion framework calls.

## Layer 4: real-image checks and limits

Image observations carry the accepted camera, projection, world, view, frame
and capture provenance. The reusable appearance helper is limited to supported
cubes and has been exercised in Tunnels and the native angle tour. Existing
whole-image and fingerprint oracles remain in place.

Meshes, near clipping and the old foreign-capture geometry are explicitly
unsupported. The helper makes no general occlusion or object-identity claim.
These limits are part of the result; a nonblack image or a capture handle alone
does not prove that a particular object was rendered.

## Layer 5: migrated scenario coverage

The migration inventory preserves existing named suites and their variants:

1. **SceneTick and PortalLighting:** retained in the CPU scene suites.
   Compatible scene runs share observations; independent worlds, initial
   states and worker-parity comparisons keep separate scenario keys.
2. **NonEuclideanRender:** seven authored angle-tour shots run in one initialized
   client, preserving shot identity, settling and image checks. The two-world
   capture scenario remains separate.
3. **Unified arrangements:** all 20 `AllArrangements` variants across five
   transports remain in the full slow test set.
4. **PortalWalk:** all 14 product definitions and generated calls remain, with
   body, head, floor, shader, overlay, lens, fault and impairment coverage.
   Exact run metrics and bounded capture behavior are recorded above.
5. **PortalSession:** the live child, seven GPU probes, outcome/residency
   checks and bounded demand channel are recorded above.
6. **TunnelsRender and CharacterStartupRender:** Tunnels shares its compatible
   setup and streams owned frames. Character startup and font tests retain
   distinct native lifecycles, frame-39 observations, payload, crop, heap and
   teardown checks as listed above.

Coverage was assessed by named cases, variants, observations and retained
oracles, not case counts alone. Slow, GPU, fault, camera and arrangement
variants remain in the validation set.

## Completion contract

The implementation is complete only when all of these are demonstrated:

- One setup per compatible complete scenario key, with independent checks
  consuming the same observation identity and bytes.
- Serial and parallel check evaluation produce the same ordered findings.
  Checks cannot mutate the runtime through their observation API.
- Observation and session memory stay within declared bounds. Failure,
  cancellation and teardown release retained bytes and runtime resources.
- Missing observations, loss, stale identities, bad requirements and check
  exceptions produce attributed failures without suppressing other checks.
- Group selection, dependency failures, partial reports and cached reruns keep
  per-suite attribution and tier semantics. Legacy v2 report parsing still
  works and malformed v3 reports fail clearly.
- Migrated real scenes retain the full coverage inventory. Incompatible keys
  do not share a run. The unified arrangement cross product remains complete.
- Before/after workload timings and setup/capture counters show what work was
  removed. Parallel speed claims include measured serial comparisons in an
  optimized preset, with the actual backend and workload named.

The strict CI and full slow gates passed, as did development pre-commit,
architecture, source, server, GPU and the scoped sanitizer checks listed above.
The completion scope does not establish generic mesh appearance, occlusion or
object identity, and ThreadSanitizer coverage is limited to the shared
foundation. No overall speedup is claimed.
