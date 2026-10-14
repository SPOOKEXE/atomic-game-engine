# Weather implementation and verification

The active objective is the complete [weather upgrade plan](WEATHER_SYSTEMS_UPGRADES.md),
including tests, benchmarks, CPU/GPU profiling, ECS dirty-state invalidation,
batched/vectorized computation and lazy maintenance. A passing first slice is
not completion of the objective.

## Work sequence

| Workflow step | State | Evidence or next action |
|---|---|---|
| Classify | Complete | Engine feature spanning shared semantics, replication and GPU presentation |
| Discover core | Complete | CodeGraph survey and current scene/render/replication source reads |
| Specify | Complete | Original plan plus contracts in owning headers and focused tests |
| Localize | Complete | `scene`, new L8 `weather`, `render`, built-in resources and host adapters |
| Prototype | Complete | CPU/shader reference fixtures and actual sparse page integration |
| Stubs/contracts | Complete | Published evaluator, clocks, consumers, solver/checkpoint and residency interfaces |
| Implement | In progress | Integrate the complete runtime and every consumer |
| Validate | In progress | Central builds, owning suites and later approved real GPU checks |
| Optimize | In progress | CPU measurements completed; real GPU timing and remaining physical transport work pending |
| Review | In progress | Independent reading found concrete fixes and three remaining plan requirements |

## Requirements and evidence

| Requirement | Implementation location | Completion evidence still required |
|---|---|---|
| Authored style and compatibility | `scene/WeatherCompute`, class inheritance from `CloudCompute`, existing `Atmosphere`/`FogEffect` | Reflection, old-world reads, transactional save/wire tests and generated bindings |
| Deterministic seeded samples | `weather/Weather` and `resources/shaders/weather-common.glsl` | Owning CPU suite plus actual shader sample parity across supported backends |
| Packed memory and batched compute | Eight-byte semantic cells and private GPU word ABI | Exhaustive encoding, scalar/batch parity, allocation accounting and measured vectorized baseline |
| Shared timeline and bounded seeking | `scene/WeatherClock`, `weather/Clock`, connector timing callback | RTT/skew/large tick/late join, pause/load, world suspension and wind-anchor continuity |
| GPU resident generation and volume rays | `render/WeatherGpu`, graph weather nodes | Real view depth/inside-cloud/disabled/failure image checks and GPU memory counters |
| Sparse near/far LOD and lazy maintenance | `weather/Residency`, `render/WeatherResidency`, page buffers | Real GPU residency consumption, no holes on motion/new views, maintenance cache and bounded overflow |
| Assemblies and deterministic splitting | `weather/Assemblies` plus residency selection | Totals, wind divergence, hysteresis, canonical regeneration and actual useful compression |
| Stateful evolution | `weather/Simulation`, `render/WeatherSimulationGpu` adapter | Actual GPU runtime, conservation/stability bounds and fixed canonical grid independent of camera |
| Checkpoints and bounded replay | `weather/Checkpoint`, stateful ECS runtime | Complete save/load/late join, admission bytes, forcing replay and transactional refusal |
| Fog and atmosphere | `weather/Consumers`, renderer composition | Runtime response application, terrain-relative fog and no double medium contribution |
| Rain/snow/hail | Shared consumer sample and generic GPU particle request adapter | Real precipitation visibility/occlusion, melting, synchronized rates and damage-once hail events |
| Lightning and thunder | `weather/Events`/`Runtime` | Stable authoritative event IDs, visual discharge, listener delay/audio and duplicate refusal |
| Cloud shadows | Optional distance-bounded GPU transmittance | Actual ground/direct-sun integration, offscreen casters, disable and update-rate tests |
| Temporal reconstruction | Per-view cloud history | Depth-aware reconstruction, disocclusion, camera cuts, timeline correction and multi-view history |
| Dirty bits and lazy systems | ECS epochs, clock updates and retained GPU generations | No-op/paused runs produce no unnecessary generation, upload or allocation; measured cache behavior |
| Tests and benchmarks | Owning module suites and `just` recipes | Dev/server/CI builds, affected/full suites as appropriate, sanitizer checks and stdout benchmark results |
| Architecture and docs | New module graph entry, purposes, source checks and public docs | Downward/headless checks, complete component purposes, generated bindings/docs and formatting |

## Validation record

| Check | Result |
|---|---|
| Initial `cmake --preset dev` | Passed with the new L8 shared module |
| First focused dev build | Failed: weather reflection adapter used private ECS helpers; fixed using public property types |
| Second focused dev build | Failed: test `ByteReader` declaration hit C++ most-vexing parse; changed to brace initialization |
| Procedural source/shader syntax | Agent-reported successful CPU warning checks and `glslc`; not a GPU runtime result |
| Shared dev weather suites | Passed: 9,794 assertions across 64 cases before the later ground-query adapter changes |
| Full scene suite | Passed: 527,258 assertions across 659 cases before the later inbox fixture and response controls |
| Headless render weather suites | Passed: 668 assertions across 14 cases before temporal/shadow/effect integration |
| Weather replication transport | Passed: 230 assertions, exercising final anchors over both QUIC and datagram paths |
| World suspension observers | Passed: 44 assertions across 2 cases, including queued transitions and worker affinity |
| Architecture graph | Passed: new shared L8 weather dependency and headless server closure |
| Initial optimized CPU benchmarks | Ran shared evaluator, assemblies, consumers, residency, solver and checkpoint suites; encoding/packing denominator clarified before subsequent runs |
| Checkpoint parser fuzzing | Passed 10,000 Clang AddressSanitizer/UBSan mutation runs with bounded checkpoint decoding and transactional refusal |
| Generated component catalogue | Generated with every initial weather component documented; ground-query additions require regeneration |
| Generated bindings | Corrected contradictory read-only write sets; generated manifest, Luau and TypeScript declarations |
| Full goal acceptance | Not yet established |

## Current checkpoint

The `dev` client, server and Studio programs build. The headless `server` build
and the shared `ci` targets also build. The current central checks passed:

| Suite | Assertions | Cases |
|---|---:|---:|
| Shared weather | 22,130 | 98 |
| Scene | 529,951 | 666 |
| Game | 5,140 | 108 |
| Graph | 11,933 | 255 |
| Replication | 52,540 | 312 |
| World | 34,188 | 294 |
| Weather rendering, headless | 118,100 | 47 |
| Weather thunder adapter, headless | 1,287 | 5 |
| Audio mixer | 16,743 | 36 |

The first complete headless presentation run exposed two incorrect fixtures
(a cadence boundary and an atmosphere response) and a real thunder scheduling
issue. The corrected fixtures and opt-in exact playback scheduling now pass.
The mixer now publishes its completed-block sample clock atomically for producer
reads; the headless callback/producer concurrency case passes.
The architecture graph and all six architecture fixtures pass. Source scanning
reports zero open ownership, crossing-pointer or serialized-name-ID findings.
Its 40 public-header reports are advisory and include existing forward APIs.

Accepted definitions and canonical stream names remain stable while event
backlogs drain. Named epoch high-water marks survive provider retirement and
save/load. Sequence rollover starts at the current anchor. Structural physical
edits create an explicit new physics era after candidate validation; wind and
appearance edits preserve evolved cells. Failed initialization records a local
saved diagnostic so corrective edits can recover without an invalid checkpoint.

The demo starts in procedural mode and uses canonical weather rain, wind and
lightning. Its two authored fog
volumes remain local appearance controls. Independent authored GPU particle
fields still compose with weather precipitation.

## Measured CPU work

These are five-sample `bench` preset measurements on an AMD Ryzen 9 9900X,
with other builds running concurrently. They describe these fixtures, not a
shipping frame budget. Benchmark results were printed to stdout.

| Operation | Measured time |
|---|---:|
| Scalar procedural sample | 340 ns/cell |
| Batched procedural sample | 225 ns/cell |
| Retained four-view maintenance check | 12 ns/call |
| 4,096-cell warm physical solver tick | 2.35 ms/tick |
| Complete 4,096-cell checkpoint encoding | 195 us/checkpoint |

The batched evaluator measured approximately 1.51 times scalar throughput.
The `bench-o0` fixture measured 1,576 ns/cell scalar, 1,450 ns/cell batch and
187.7 ms per warm solver tick under concurrent build load. The unoptimized
solver cost remains material; release measurements cannot stand in for it.
Packing comparisons now put a compiler barrier between encoding and decoding
so an immediate inverse operation cannot erase the measured memory passes.
Those comparisons need a fresh run after that correction.

Certified empty pages use an all-time density bound, including the sampling
halo, and retain no logical field payload. The bound passed 1,000,000 actual
reference samples, 6,000 structured worst-noise cases, 50,112 envelope intervals,
32 symbolic proof lemmas and sanitizer checks. Physical solver pages do not use
that certificate. An isolated uniform-page fixture halves field payload, while
metadata-inclusive memory falls only 5.24 percent; no broad compression ratio
is claimed for ordinary weather.

## Remaining acceptance work

The implementation audit identified three explicit plan features still being
completed: script sampling through the shared VM bridge, terrain-aware physical
boundaries with ground moisture inputs, and conservative downward transport of
precipitating water. The existing immediate precipitation sink and cosmetic
falling particles do not establish physical transport.

Real GPU parity, image fixtures, timing, memory counters and live weather-scene
verification have not run. No graphics backend is yet declared validated for
weather. The integer procedural contract and the authoritative CPU solver remain
separate from cosmetic FP32 GPU evolution. NEON compilation exists, but runtime
parity has only been exercised on this x86 machine.

GPU/live verification has not been run. The repository requires approval before
those checks. Seek that approval only after the runnable implementation and
reviewable fixtures are ready. Benchmarks print to stdout; build logs stay under
the relevant build directory.
