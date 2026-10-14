# Weather implementation and verification

The [weather upgrade plan](WEATHER_SYSTEMS_UPGRADES.md) is implemented across
shared weather semantics, authoritative simulation and GPU presentation. Final
acceptance remains open for real-device parity, image checks and GPU profiling.
Those checks require repository approval and have not run.

## Implemented behavior

| Area | Implementation |
|---|---|
| Authoring | `WeatherCompute` inherits `CloudCompute` appearance and composes with existing `Atmosphere` and `FogEffect`; reflection, save/wire codecs and both script declarations are generated |
| Procedural field | Versioned seed, canonical integer noise, eight-byte semantic cells, bounded arbitrary-time seeking, retained wind anchors and four-lane SSE2/NEON evaluation |
| Synchronization | Accepted definitions, canonical clocks, receipt estimates, final pause anchors, named saved epoch high-water marks and bounded late-join inboxes |
| Stateful field | Fixed camera-independent FP32 grid, pressure projection, condensation, source/boundary budgets, terrain solids, surface moisture flux and conservative falling-water transport |
| Checkpoints | Complete format 3 state, bounded replay and transactional refusal; replicas query validated bytes without another CPU solver grid |
| Script sampling | Owned `WeatherCompute:Sample(position[, groundHeight])` results in Luau and JavaScript, including exact decimal tick/epoch metadata |
| Sparse volumes | Coarse coverage, dense/compact/certified-empty pages, bounded per-frame preparation, shared multi-view requests and immediate teleport invalidation |
| Assemblies | Exact encoded totals, separate child velocities, hysteresis, canonical reconstruction and a final optical error guard |
| Rendering | Packed GPU generation/evolution, half-resolution volume integration, depth-aware temporal reconstruction and optional distance-bounded cloud shadows |
| Fallback | Bounded fragment clouds for Tier B, missing graph products and refused compute resources; unavailable shadows use unity transmittance |
| Consumers | Atmosphere response, terrain-relative fog, rain/snow/hail particles, roof clipping, authoritative lightning/hail events, ribbons and delayed thunder |
| Lazy work | ECS write/membership epochs, retained terrain snapshots, bounded maintenance, cached descriptors, completed-publication reuse and retry gates for unchanged failures |
| Demo | Procedural default, stateful/pause controls and ground/interior/above/roof/storm views; canonical precipitation replaces the old manual rain emitters |

Physical cells occupy 32 bytes plus four bytes of falling water. Terrain heights,
solid bits, scratch, forcing, checkpoints and renderer targets are additional
storage. GPU stateful buffers currently require 260 bytes per cell plus grouped
diagnostics. The eight-byte procedural payload is not a complete physical-state
memory estimate.

GPU publication swaps physical cells, falling water and terrain flags only after
completed validation. Compatible forcing and appearance edits retain the same
physical owner across graph pipelines. A changed terrain payload requires a new
accepted terrain revision. Stateful checkpoints remain available when visible
clouds are hidden but ground shadows are active.

## Headless validation

The development client, server and Studio programs, the headless server preset
and strict CI programs build. These completed central runs include the final
review fixes.

| Suite | Assertions | Cases |
|---|---:|---:|
| Shared weather | 22,474 | 108 |
| Scene | 530,090 | 669 |
| Game | 5,243 | 111 |
| Script sampling, both VMs | 214 | 3 |
| Full script host | 9,043 | 450 |
| Shared script API | 14,886 | 142 |
| Graph | 11,933 | 255 |
| Replication | 52,540 | 312 |
| World | 34,188 | 294 |
| Weather rendering, headless | 118,158 | 53 |
| World view collection | 938 | 15 |
| Thunder, headless | 4,225 | 12 |
| Audio mixer | 16,743 | 36 |
| Studio weather | 21 | 3 |
| Weather demo | 102 | 2 |

Review and tests found real defects in checkpoint cache invalidation, top escape
flow, shadows-only checkpoint collection, fully blocked storm columns, thunder
age/deadlines and terrain preparation observation. Each has a concrete fixture.
Incorrect cadence, graph reuse and single-substep expectations were corrected
where the production behavior was valid.

Architecture and all six fixtures pass. Source scanning and its fixtures pass
with zero open ownership, crossing-pointer or serialized-name-ID findings;
public-header reports are advisory. Component documentation covers all 254
registered components. Generated bindings, all 128 Luau examples, the Luau
language server and Studio TypeScript checks pass. All 87 staged shaders pass
resource-binding checks and MSL translation. Public documentation coverage
passes after documenting the new API. Formatting and em-dash checks pass.
Checkpoint format 3 decoding passes 10,000 libFuzzer runs under AddressSanitizer
and UBSan, including valid terrain/rain and escaped-top seed checkpoints.
Raw build/test logs and derived artifacts live under the build tree.

## CPU measurements

Five-sample benchmark runs on an AMD Ryzen 9 9900X, with the named benchmark
presets. Other programs loaded the machine, including a game using about eleven
CPU cores. Variance is substantial, especially in optimized solver results.
These figures describe the fixtures and do not establish shipping frame budgets.
Benchmark output is printed to stdout.

| Operation | `bench` optimized | `bench-o0` |
|---|---:|---:|
| Scalar procedural sample | 339 ns/cell | 1,422 ns/cell |
| Batched procedural sample | 260 ns/cell | 1,371 ns/cell |
| Four-view shared maintenance check | 17 ns/call | 407 ns/call |
| 4,096-cell cold solver tick | 2.11 ms | 29.70 ms |
| 4,096-cell warm solver tick | 1.95 ms | 25.19 ms |
| 4,096-cell terrain/rain/source tick | 2.04 ms | 26.08 ms |
| Complete 4,096-cell checkpoint encoding | 298 us | 4.16 ms |
| 4,096-cell presentation packing | 225 us | 1.45 ms |

Cached adjacency, pressure-neighbor counts and direct typed rows remove wrapper
work from Jacobi iteration while preserving the original summation order.
The previous warm O0 fixture measured 187.7 ms under concurrent build load;
different host loads prevent treating the difference as a controlled speedup.
O0 selects scalar evaluation because its intrinsic wrappers previously measured
slower than the scalar path. Optimized builds use four-lane integer arithmetic.

Actual render residency benchmarks measured 219 ns for warm four-view reuse,
109 us for first one-view coverage, 171 us for first four-view coverage and
151 us for teleport invalidation. Complete four-view maintenance measured
19.4 ms across the bounded preparation steps, not in one presentation frame.
Descriptor preparation measured 324 ns; temporal keys for 32 views measured
560 ns and four-cascade shadow planning measured 304 ns. Weather emission
against 10,000 bounds measured 76.8 us unchanged and 287 us cold.

Words and byte planes round-trip every legal codec byte exactly. Software E4M3
is lossy, bounds error to eight unsigned/four signed codes, retains the same
eight-byte cell payload and adds 1 KiB of lookup tables. Packing fixtures consume
encoded data before decoding so the compiler cannot erase an immediate inverse.
Sub-nanosecond reported word costs are below the report's resolution and do not
justify a storage decision.

Certified empty procedural intervals retain no field payload and include the
sampling halo. The conservative bound passes one million evaluator samples,
6,000 worst-noise cases, 50,112 interval checks, 32 symbolic lemmas and sanitizer
checks. Stateful history does not use that certificate. An isolated uniform-page
fixture halves field payload but reduces metadata-inclusive memory by only
5.24 percent; no general compression ratio is claimed.

## Pending device acceptance

Real GPU procedural/stateful parity, fallback and lighting images, sparse page
consumption, inside/above/roof views, shadow images, delayed timestamp results,
actual memory counters and weather-demo captures have not run. The opt-in GPU
benchmark's disabled harness timing is not a GPU measurement. No backend is yet
declared validated for weather, and no GPU speedup is claimed from shader source.
The NEON branch is compiled conditionally but has not executed on this x86 host.

The existing approval request covers GPU fixtures, profiling, short headless
captures and reruns after fixes. [AGENTS.md](../AGENTS.md) requires approval before
compute/live verification. The authoritative CPU field and gameplay events stay
independent of cosmetic GPU evolution.
