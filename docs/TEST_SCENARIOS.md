# Shared test scenarios

## Objective

Run one compatible engine scenario once and let independent checks consume the
same simulation, physics, replication, rendering and scene observations. Checks
must run concurrently against immutable observations without obtaining access to
the running world. Preserve the existing test coverage, scenario variants,
source ownership, selection rules and test tiers while removing repeated setup,
unload and capture work.

This is the execution plan for the two shared-scenario items in v0.26.1. The
deliverable includes the runner, its integration with suite execution and
reports, real observation adapters, and migration of the existing compatible
integration tests. A generic callback demonstration or a portal-only migration
does not complete the work. Ordinary value-type unit tests do not need a world.

## Baseline and evidence

At the start of the review on 2026-10-21, `VERSION` was `0.26.1`. The
recorded pre-change `dev` baseline at revision `c1dc0e30`, seed 1, ran the
SceneTick and PortalLighting suites: 33 cases, 18,460 assertions, 5.46 seconds.
The current tree has passed the scenario foundation suites on `dev` and
`server` (18 cases, 154 assertions) and the runner suites on `dev` (84 cases,
693 assertions). The architecture check passed with 55 modules, 6 programs,
41 layered edges and 6 fixtures; source check passed for 2,623 files. The CPU
scenario suites passed (33 cases, 18,475 assertions), and unified tests passed
for 8 non-slow cases (265 assertions).

`just test-pre-commit --no-report` passed 779 suites with zero skips or failures.
The displayed 364.10 seconds, and a later displayed 367.56 seconds, are
aggregate suite durations, not wall time; wall time was not independently
measured. The run used `dev` with eight workers, excluding GPU and slow cases.
Architecture and source checks passed again. The native angle-tour passed after
the zero-time heartbeat guard fix: 1 case, 165 assertions, 1 client setup, 112
frames and 7 captures. Native memory and file parity passed with 3,093
assertions in 1 GPU case. Renderer-owned capture passed with 139 assertions in
1 GPU case, including one shared-readback counter. Frame observations passed
with 32 assertions across 5 cases. The full renderer GPU run passed 237 of 252
cases, with 15 opt-in skips, zero failures and 2,705,902 assertions. The 24
private-queue and real-window cases passed in both serial and parallel modes.

These runs do not establish a speedup: suite durations are not wall-time
measurements and some runs overlapped with other work. A wrong-world rendering
defect was reproduced in fresh warm 30/60 runs. Following the head seed and
settling at the return endpoint makes the regression pass at both warm rates
(12,696 assertions in 1 case), after 49 assertions failed before that change.
This does not resolve the rendering issue: the completed PortalWalk matrix had
18 cases, 14 passed and 4 failed, with 72 failed assertions among 434,297
assertions. The cold-return and 240 FPS cases still fail and remain under
investigation. The full renderer GPU result above remains 237 passed, 15
opt-in skips and zero failures. Native observation adapters are implemented
and under review. Generic checks, larger migrations, and the strict and
sanitizer gates remain pending; the full objective is incomplete.
The native API and angle-tour changes may be reviewed and committed separately;
that would not complete the broader scenario migrations.

| Existing path | Repeated work or coupling to replace |
|---|---|
| `mono.client/tests/SceneTick.cpp` | Its local `Session` loads Rings and starts/stops the process-wide job pool for many independent cases. Several checks need only different milestones of the same run. |
| `mono.client/tests/PortalLighting.cpp` | Separate `LitScene` instances load PortalLightMix for authored-light and portal-disable checks. |
| `mono.client/tests/NonEuclideanRender.cpp` | The angle-tour loop constructs seven clients, one for each selected authored shot. |
| `mono.unified_tests/tests/Crossing.cpp` | The matrix, relayed-content and discovery cases repeat compatible 90-tick arrangements. Some longer or mutating cases require separate milestones or keys. |
| `mono.client/tests/PortalWalk.cpp` | One product run already serves many assertions per parameter tuple, but driving, live mutable state, disk captures and checks are coupled in `RunPortalWalk`. |

`RUNNING.md` records an earlier warm `dev` run with eight runner workers and
777 suites: 8 seconds for fast, 73 seconds for pre-commit, 91 seconds for PR,
and 22 minutes for all suites including slow cases. Those retained historical
numbers are context only. They are not a fresh baseline for this change.

Before implementation timings are compared, record the current revision,
preset, selected suites and cases, runner workers, GPU backend, cache state,
scenario setup count, ticks/frames, captures and wall time. Measure the same
workload after migration. Preserve failed or skipped baseline checks explicitly.
Do not label a cache hit as a faster execution of the tests. Print benchmark
measurements to stdout, following the repository benchmark policy.

## Layer 1: reusable scenario execution

Add `Engine::testing` under `mono.engine/testing`, at L3, shared tier, depending
only on `Engine::core` and `Engine::parallel`. It owns typed scenario execution,
session reuse, observation bounds and independent named check results. It must
not name a store, renderer, authority, product class or Catch type. The new
module earns its place because the same headless library is used by CPU,
replication and GPU integration tests without pulling their runtime adapters
into each other's dependency closures.

The existing suite declarations remain in `mono.build/testmain`. That layer
adapts Catch and suite reporting to the engine runner. Add the module to
`mono.engine/CMakeLists.txt` and the architecture graph, including its shared
tier, layer and exact dependency closure. Give each public behavioural header a
focused suite and document its invariants through the repository's permitted
new-module workflow. Do not widen an existing lower layer to depend on render
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

Add an opt-in `TEST_SCENARIO_GROUP` declaration to suite metadata. Compatible
selected suites in the same binary may run in one child process so their
scenario sessions can be reused. Suites without the declaration preserve their
current execution model. A group is a scheduling hint; the complete scenario
key still determines reuse within it.

Keep every suite's identifier, source/header closure, `TEST_DEPENDS` and tier.
`TEST_DEPENDS` cascades through suite signatures for invalidation; it does not
order test execution. Do not merge source ownership merely because tests share
a runtime. Respect user filters, GPU exclusion and slow-case exclusion.
Selecting one suite must not silently run an unselected expensive peer or mark
that peer as passed. Different binaries cannot share a live runtime.

Introduce MonoReport v3 with per-suite attribution for each case/check in a
group. Continue reading legacy v2 reports. Reject malformed, missing or
ambiguous attribution. Aggregate suite totals from the cases that actually ran;
do not copy a group's totals into every member. Shared setup cost should be
reported once, with attribution that does not imply it was paid several times.

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
| Rendering | Owned `DataCapturePoll` planes, camera/projection, snapshot/frame/world identity and object labels; copied `VisibilitySnapshot` where applicable. |
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

## Layer 4: general scene and portal image checks

Build checks around captured scene facts. A scene supplies object identities,
placement, bounds, material/visual facts and relevant portal topology. An image
observation supplies its accepted camera, projection, world, view, frame and
capture provenance. A reusable check projects the actual object through the
declared portal mapping, then tests the corresponding image region, object IDs,
depth or other requested channels.

Replace fixture-specific world coordinates, default-head dimensions and fixed
room-color assumptions in the migrated portal checks with these inputs. Keep
explicit tolerances for the property being checked. A mere nonblack image or a
capture handle is not proof that the expected scene object was rendered.

Preserve first-person visibility, trailing-source views, clipping, native and
foreign eyes, opaque/complete-world capture scope, content/shader ownership,
spatial overlays and lenses. Use synthetic observations only for the check's
own geometry and failure branches. Demonstrate the same reusable checker on
multiple real scenes and on supported object forms rather than encoding portal
walk names into it.

## Layer 5: migrate existing scenarios

Complete these migrations and remove their replaced setup/capture paths:

1. **SceneTick:** share the compatible Rings instance-count, resource/state,
   bounded draw-list, movement, camera, interpolation and previous-transform
   checks. Capture their distinct milestones in one run. Preserve separate
   identities for independent-world, timestep, altered initial state and worker
   parity comparisons. Cover additional compatible authored scenes through the
   same owner adapter.
2. **PortalLighting:** load each authored scene once. PortalLightMix publishes
   baseline observations, applies its planned disable action, and publishes the
   disabled state. Independent checks consume the appropriate milestone.
3. **NonEuclideanRender:** run the seven angle-tour shots as authored actions in
   one initialized client. Preserve shot identities, settling requirements and
   image checks. Keep the different two-world capture scenario distinct.
4. **Unified arrangements:** keep all 20 `AllArrangements` variants across
   the five transports.
   Share compatible matrix/content/discovery reports at their existing logical
   milestones. A 150-tick case can consume a later checkpoint when the initial
   state and action history match; mutating or differently configured cases
   remain separate. Keep every contradiction tied to the modules it compares
   and import their report types directly.
5. **PortalWalk:** preserve every generated rate, camera, input, content,
   shader, impairment and fault variant. Separate scenario driving and bounded
   in-memory capture from named camera, movement, replication, physics,
   handoff, content and image checks. Share real hook observations and use the
   general object-projection checker. Runtime distinctions remain in the key.
6. **Other compatible product tests:** apply the same observation path to
   `PortalSession.cpp`, `TunnelsRender.cpp` and `CharacterStartupRender.cpp`.
   Reuse setup where their scene, initial state and action history agree, and
   retain separate runs where those preconditions differ. Do not replace the
   broad migration with helper-only adoption that still reloads every check.

Keep a before/after coverage inventory of named checks and scenario variants.
Grouping cases may change case counts, so counts alone do not prove parity.
Explain every removed assertion or changed oracle in that inventory. Do not
prune slow, GPU, fault, camera or arrangement variants to improve timings.

## Validation and completion evidence

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

Use the commands documented in `RUNNING.md`. During construction run the
focused suites and `just test-fast`; after a coherent migration run
`just test-pre-commit`. Configure and build both `dev` and `server` to prove the
new shared module has no presentation dependency:

```sh
just preset=dev configure
just preset=dev build
just preset=server configure
just preset=server build
just preset=server test-pre-commit
just test-architecture
just source-check
just check-server-is-headless
just format-check
just em-dash-check
```

Existing authorization covers GPU and live presentation checks. At final
verification, run the full headless GPU suite through `just test-pr`, the
strict gate through `just preset=ci check-pr`, and `just test-slow` for the
migrated long scenarios and milestone completion gate. Run the complete
required set rather than reporting only a focused pass. If a check cannot run,
record exactly what was skipped and why; do not describe it as passed.

Run sanitizers appropriate to the new lifetime and concurrency paths, keeping
ThreadSanitizer separate from AddressSanitizer. Report what actually ran and
any environment limitation. Test behaviour and important failure paths, not
large collections of smoke tests that mirror the helper implementation.

Record implementation and validation results here as they become available.
The initial entries below are intentionally incomplete:

| Evidence | Initial status |
|---|---|
| Read-only review and source baseline | Complete; seeded SceneTick and PortalLighting baseline recorded above |
| Scenario module and runner/report integration | Foundation, runner and pre-commit suites pass as recorded above; implementation under review |
| Real observation adapters and reusable image checks | Focused checks pass; wrong-world regression passes locally at warm rates, while cold-return and 240 FPS PortalWalk cases remain failing under investigation |
| Existing scenario migrations and coverage inventory | CPU and unified results listed above; generic checks and larger migrations remain incomplete; no completed migration claim |
| Fresh baseline and after-change measurements | Baseline recorded; current timings are inconclusive and show no established speedup |
| Build, CPU, GPU, slow and sanitizer gates | Pre-commit, architecture, source and focused GPU checks recorded above; strict, slow and sanitizer gates remain |
