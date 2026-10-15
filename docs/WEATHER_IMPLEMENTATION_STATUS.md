# Weather implementation and verification

The [weather upgrade plan](WEATHER_SYSTEMS_UPGRADES.md) is implemented across
shared weather semantics, authoritative simulation and GPU presentation. The
approved device checks now run on Vulkan with an NVIDIA GeForce RTX 4090 and
driver 580.173.02. Canonical GPU parity, stateful evolution, sparse transition
images, lightning, portal composition and exact opaque-wall preservation pass
in both development and strict CI builds. CPU and GPU measurements are recorded
below. Other backends have not been run.

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
| Demo | Six shared weather recipes, procedural default, stateful/pause controls and ground/interior/above/valley/roof/storm views; canonical precipitation replaces the old manual rain emitters |

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

## Plan exit gates

The plan's eight stages have separate acceptance gates. Compiled shaders and
headless planning tests do not establish rendered behavior or device performance.

| Stage | Current evidence | Acceptance |
|---|---|---|
| 1. Contract and dense slice | CPU/GPU packed bytes agree, including phase-wrap and UINT64_MAX; world rays clip to depth; all six recipes and interior/above views were captured | Passed on local Vulkan |
| 2. Time and lifecycle | Clock/replication suites cover cold joins, correction, pause and save/load; actual renderer shutdown/reinitialization regenerates the field and reproduces the cold image exactly | Passed on the local Vulkan backend |
| 3. Sparse residency | Dense/compact words and per-pixel dense references agree across seams, missing pages, eviction and refill; planning covers budgets, motion, teleports and multi-view unions | Passed within declared optical/FP16 bounds |
| 4. Lazy LOD and assemblies | Totals, retained velocities, reconstruction, hysteresis and bounded maintenance pass; compact/coarse transitions and unsafe shadow fallback meet their error bounds | Passed |
| 5. Lighting and shadows | Device fixtures cover occlusion, temporal silhouettes, colored sun/night, direct versus ambient light, disabled shadows and both portal capture paths | Passed on local Vulkan |
| 6. Weather consumers | Shared events, terrain adapters, VM controls, fog/roof geometry and mixer callbacks pass; actual GPU precipitation composes, lightning branches render/expire, and roof/storm/fog captures were inspected | Passed |
| 7. Stateful evolution | CPU budgets/checkpoints and actual GPU terrain, falling-water, top escape, shared-owner and transactional-refusal fixtures pass | Passed with authoritative CPU state and cosmetic GPU evolution |
| 8. Performance and replacement | 720p/1080p device work, shadow quality and logical memory are measured; compiler optimization reduces costs; replaced manual weather effects are removed | Named local prototype budget passes; hardware limits below |

The six demo recipes cover clear sky, scattered cumulus, overcast stratus,
towering storm, wispy ice and ground fog. They use the same provider with distinct
shape, moisture, temperature and motion controls. The far clip is 6,000 metres;
interior and above-cloud cameras follow the selected layer. Actual VM button
events verify these controls, including snow without liquid rain in the ice
recipe and positive near-ground fog extinction. Canonical occupancy checks
retain scattered cumulus, broad stratus and sparse ice at three specified times.

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
| Weather demo | 598 | 2 |
| Full renderer, dev and strict CI separately | over 413,000 | 819 |
| Weather GPU, dev and strict CI separately, Vulkan | 486,878 | 16 |

Review and tests found real defects in checkpoint cache invalidation, top escape
flow, shadows-only checkpoint collection, fully blocked storm columns, thunder
age/deadlines and terrain preparation observation. Each has a concrete fixture.
Incorrect cadence, graph reuse and single-substep expectations were corrected
where the production behavior was valid. Real GPU runs exposed retained history
buffers being incorrectly treated as externally supplied buffers, and a coarse
shadow fallback bypassing its fine optical guard. Both have reproduced failures
and passing checks. Graph baseline tests now include the weather nodes and
resources. The demo test checks the actual client camera, including view requests
and free movement between requests.

Architecture and all six fixtures pass. Source scanning and its fixtures pass
with zero open ownership, crossing-pointer or serialized-name-ID findings;
public-header reports are advisory. Component documentation and generated
bindings pass their checks. All 128 Luau examples, the Luau
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

## Device verification and measurements

User approval covered real GPU fixtures, benchmarks, short headless captures and
reruns after fixes. Vulkan is the locally exercised weather backend. MSL
translation and binding checks pass for all 87 shaders, but Metal execution has
not been tested. The NEON branch has not executed on this x86 host.

The device fixtures compare canonical CPU/GPU words at large and wrapped times,
validate terrain/rain/top-boundary evolution, and reject partial stateful
publication. Renderer recreation regenerates the procedural field. Volume
fixtures compare every pixel against a dense reference across dense/compact
seams, coarse LOD, missing pages, eviction, refill and topology blending, with
the declared optical budget and FP16 allowance. Lightning images require visible
branches away from the main channel and exact restoration after expiry.

Portal comparisons cover mixed and separate captures, two camera poses and
both compute and pending-stateful fragment fallback. They keep the existing
two-display-code tolerance and verify cold scratch allocations and warm reuse.
These checks exposed missing capture weather, padded-depth sampling and
oblique fallback rays crossing infinity. Capture weather now uses stored depth,
its own camera, bounded bank-owned scratch and no shared temporal history.
Raw opaque-wall captures proved exact zero scattering, unit transmission and
zero medium hits. Weighted reconstruction normalization still introduced an
FP16 ULP at some pixels. An exact-neutral branch now preserves the original
scene, with unchanged exact comparisons in both pinned and ordinary graph
variants. Non-neutral and absorbing samples retain the original integration.

Inspected 1280x720 captures include the six recipes, cloud interiors, above-cloud
views, valley fog, roof and storm controls with the sunset environment. Artifacts
are under `.cache/build/dev/weather-acceptance/`. Cumulus was nearly dry in the
original recipe. Its moisture now produces both clouds and clear columns at
ticks 0, 100 and 500. Stratus retains at least 75 percent occupied columns at
those times; ice remains sparse and deliberately faint. Actual VM tests sample
the canonical field rather than checking only the authored parameter values.

The optimized `bench` fixture uses 1920x1080 output, 960x540 cloud images,
one layer/view/opaque receiver, 64-metre cells, 512-page budget, maximum LOD 4,
12 authored cloud steps and one shadow cascade. The kernel takes four samples
per authored step. Completed GPU queries are sampled every fourth frame;
the cold submission has no sampled device timing. Recording time excludes the
explicit device-idle wait. Eleven printed phase rows from a three-sample job,
including harness warmups, supply each median below.

| Prototype comparison | Before | After |
|---|---:|---:|
| Same-tick cloud integration, 32x32 shadows | 6.22 ms | 2.35 ms |
| Advancing cloud integration, 32x32 shadows | 8.93 ms | 3.75 ms |
| Shadow map, 32x32 | 6.45 ms | 1.80 ms |

This comparison changes shader compiler optimization while retaining identical
work counters and residency. The two weather kernels use `-O -g`; debug names
are needed for the SPIR-V/MSL binding check. Their unused cloud colour sampler
was removed and bindings renumbered. Extra page-lookup and raw-corner caches
were measured and removed because their gains did not survive the final gate.
Power-of-two page masks replace signed division; negative-coordinate behavior
follows the bit-preserving integer conversions in the
[GLSL specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html).

The subsequent shadow-quality sweep, without the raw-corner cache, measured
the following medians. Other desktop/GPU work was present, including a resident
19,608 MiB model server. Variance prevents ranking the close quality settings or
claiming an isolated maximum frame time.

| Shadow resolution | Advancing cloud | Shadow map | Approximate sum of measured weather passes | Logical live GPU bytes |
|---|---:|---:|---:|---:|
| 64x64 | 5.92 ms | 1.97 ms | 8.03 ms | 265,057,176 |
| 128x128 | 5.98 ms | 3.93 ms | 10.06 ms | 265,155,480 |
| 256x256 | 5.37 ms | 2.96 ms | 8.47 ms | 265,548,696 |

Final runs after the depth/neutral-reconstruction fixes retain the authored
256x256 single-cascade default:

| Output | Same-tick cloud | Advancing cloud | Shadow map | Sum of measured weather passes | Logical live GPU bytes |
|---|---:|---:|---:|---:|---:|
| 1280x720 | 2.00 ms | 3.00 ms | 2.48 ms | 5.60 ms | 127,297,176 |
| 1920x1080 | 2.39 ms | 3.73 ms | 2.41 ms | 6.28 ms | 265,548,696 |

At 1080p, CPU command recording measured 0.75 ms per same-tick frame and
1.03 ms per advancing frame; the cold recording measured 9.58 ms. These are
recording durations, not total frame or first-use shader compilation costs.

The local prototype target is at most 12 ms for these measured weather passes;
these sampled medians pass it. This is not a full-frame, cold-start or
cross-hardware guarantee. The sum includes field generation, summary,
integration and reconstruction; it does not imply queue overlap. Logical live
bytes cover the renderer's tracked resources, not just weather cells and not
driver heap commitment. Advancing frames generate 536,576 cells over eight
generations; same-tick frames generate none, record eight field-cache hits and
still integrate eight views. Refinement and history warmup allocate resources,
so these short phases do not establish allocation-free steady state.

Run `just weather-gpu-bench 3 bench 1920 1080 256`. Width/height are bounded to
3840x2160; shadow resolution is a power of two in 32..2048. Invalid configuration
is refused before SDL initialization. Only `MONO_WEATHER_GPU_BENCH=1` enters the
device path. The benchmark requires positive generation, summary, ray and shadow
work, plus actual same-tick reuse, so an empty/refused workload cannot report
success. Results print to stdout. Disabled-harness timings are not GPU evidence.

## Uncapped presentation and follow-up performance, 2026-10-15

Client presentation now defaults to immediate mode with no FPS cap. `--vsync`
restores display pacing, and `--max-fps N` sets an explicit presentation rate.
The fixed simulation rate remains independent. Headless data-factory and
inherited presentation services retain their bounded service cadence. See
[client frame pacing](../mono.client/docs/FramePacing.md).

Studio also defaults to uncapped presentation. Its Preferences page now exposes
an **Uncapped presentation** toggle that restores the saved active, idle,
focused and unfocused limits when disabled. Explicit saved caps still load;
`--uncapped` remains a temporary override. See
[Studio frame pacing](../mono.studio/docs/FramePacing.md). The local user profile
was switched to uncapped without changing its other values; the previous file
is backed up under `.cache/build/dev/weather-uncapped/studio-preferences-before.json`.

Three sequential 180-frame native window checks at 640x360 reported 464 FPS
by default, 60 FPS with VSync and 95 FPS with a 100 FPS cap, including startup.
These empty-scene checks verify pacing choices rather than weather throughput.

Changing sunlight previously changed the exact shadow-demand radius every
frame, repeatedly restarting sparse residency maintenance. Planning now expands
requested ranges upward to a world-brick boundary and uses that same range for
both coverage and invalidation. Crossing the expanded boundary still rebuilds
immediately. Tests exercise a camera between the raw and expanded limits, plus
160 actual animated-sun shadow demands completing bounded maintenance.

Two sequential 600-frame `dev` runs used the same final shaders, 1280x720,
`Weather.luau`, no FPS cap and an RTX 4090 Vulkan backend. The retained executable
for the first run predates the CPU residency fix. The before snapshot retains
396 frames in its five-second window; the after snapshot contains all 600.

| Development frame-graph measurement | Before | After |
|---|---:|---:|
| Residency planning median | 6.969 ms | 0.011 ms |
| Residency planning mean | 6.944 ms | 0.085 ms |
| Whole-frame median | 12.409 ms | 4.738 ms |

These are unoptimized CPU measurements with concurrent desktop/CPU work, not
shipping performance guarantees. Startup and maintenance spikes remain: the
after run includes a 2.622-second first-use frame. Snapshots and runtime logs
are under `.cache/build/dev/weather-uncapped/`. The optimized `bench` residency
suite measures the new 180-frame animated-sun scenario at 16.867 microseconds
per frame, including its initial bounded maintenance. Run
`just weather-residency-bench 3 bench`.

Cloud noise now skips an epoch when its integer interpolation weight is exactly
zero. Two call sites retain the original integer results and avoid the interior
regression measured with a larger four-call-site variant. Shadows retain their
original branchless interpolation because endpoint branching regressed their
interior timings. Packed-cell GPU parity covers the branch boundaries, extreme
signed coordinates, feature shifts and epoch wrap.

The final 1920x1080, 256x256-shadow GPU comparison alternated original and final
shader binaries twice. Each value below is the range of two medians, each from
11 printed phase rows of a three-sample job. Work counts and tracked residency
were identical.

| Initial canonical tick | Original paused cloud integration | Final paused cloud integration |
|---|---:|---:|
| 0 | 2.356 to 2.372 ms | 1.763 to 1.807 ms |
| 128 | 2.362 to 2.398 ms | 2.282 to 2.301 ms |
| 250 | 2.439 to 2.482 ms | 1.786 to 1.800 ms |

The roughly 24 to 27 percent endpoint gain applies to endpoint phases, which
cover 13 of 256 phase codes. It is not a whole-cycle speedup. Interior cloud
timings did not regress in these comparisons; shadow timings remained within
the observed run variance. Set `MONO_WEATHER_GPU_BENCH_TICK=128` for an interior
phase. Invalid, malformed and overflowing tick settings are refused before
device initialization. Device timestamps remain sampled and cold GPU time is
unavailable, as described above.

Validation passed in both `dev` and strict `ci`: six pacing/settings cases
(48 assertions), all 821 headless renderer cases (414,448 assertions), and
16 actual GPU weather cases (801,390 assertions). All 87 shader SPIR-V/MSL
contracts, changed C++ formatting and changed-file em-dash checks pass. Metal
execution remains unverified; these device results are local Vulkan evidence.

Broader Studio checks exposed a pipeline-editor compatibility gap: weather's
storage-image output could not connect to a colour-image reader when saving.
The adapter now accepts that read edge while retaining buffer/image and
depth/colour rejection. Tests round-trip all seven weather nodes, their pins,
resource descriptors and edited execution settings through save, build and
schedule compilation.

All 722 Studio headless cases pass in both presets (76,701 assertions each).
The two MCP contract cases launch a real GPU-backed Studio, so they now carry
the GPU tag and use isolated configuration directories. Device/world startup
has a separate 60-second allowance after a measured 24-second cold start;
individual protocol replies retain their five-second deadline. Both manifests
were missing only the new weather module's resource entry. The strict updated
contracts pass in both presets (493 assertions each). Source checks and the
architecture check also pass. Studio's new Preferences control has behavioral
test coverage; its visual interaction was not exercised.
