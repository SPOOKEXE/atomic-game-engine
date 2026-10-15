# Weather GPU compute optimisation, 2026-10-15

The page-summary kernel now uses one 64-lane workgroup per page. Each lane
reads eight cells, then shared-memory integer reductions combine maximum,
minimum, total density and sample count. Previously, one invocation read all
512 cells serially. The host dispatch changes with the shader; these two parts
must be deployed together.

Summary output remains exact. Clipped cells contribute neutral extrema and
zero count. Page exits are uniform across a workgroup, and every remaining
lane reaches every barrier. The largest total is 130,560, within uint32.
Shared scratch is 1,024 bytes per workgroup. Certified-empty pages still
produce `(0, 0, 0, 512)`.

Density sampling also avoids redundant work:

- Interior LOD-zero samples reuse the selected page for emptiness and density
  sampling. Existing margins guarantee that all eight interpolation corners
  belong to that page. Boundary and transition samples retain their full checks.
- Compact density reads use the first eight bytes of an assembly, containing
  the exact density sum and unsafe marker. Other channels retain their existing
  decoding path.
- Density-only procedural fallback returns zero immediately when the vertical
  envelope is zero. Full semantic cell generation remains unchanged.

Unsafe assemblies retain canonical evaluation. Sampling resolution, integration
steps, optical error guards and visual quality settings are unchanged. The
reduction uses ordinary workgroup barriers without subgroup extensions.

## Device measurements

Measured on an NVIDIA GeForce RTX 4090, Vulkan, driver 580.173.02, using the
optimized `bench` preset at 1920x1080 with one 256x256 shadow cascade.
The baseline is commit `8261ac23`. The candidate includes density sampling
commit `e0dfc3ed` and parallel summary commit `f8cf937e`. Each uses its
corresponding summary dispatch code and compiled shaders.

For each starting tick, two alternating baseline/candidate runs each requested
three benchmark samples. The harness emitted 11 phase rows per run, including
warmup rows. Each entry below is the median of the two run medians, measured
with completed device timestamp queries. Times are microseconds.

| Starting tick | Summary before | Summary after | Same-tick clouds before / after | Advancing clouds before / after | Shadows before / after |
|---|---:|---:|---:|---:|---:|
| 0 | 76.032 | 5.120 | 1,849 / 1,738 | 3,399 / 3,388 | 2,348 / 2,342 |
| 128 | 73.728 | 5.120 | 2,302 / 2,219 | 4,286 / 3,606 | 2,364 / 2,256 |
| 250 | 75.008 | 5.440 | 1,801 / 1,812 | 2,791 / 2,891 | 2,388 / 2,374 |

Across all six run medians, summary time falls from **74.752 to 5.120 us**,
about **14.6 times faster**, a **93% reduction**. Individual summary medians
range from 73.728 to 76.288 us before and 5.120 to 5.632 us after.
Cloud and shadow deltas are mixed, including a slower advancing-cloud result
at tick 250. Desktop GPU activity was present. These results establish the
summary improvement on this device, not a general frame-rate improvement.

All compared phases retain identical dispatch counts, generated cell counts,
ray pixels, shadow texels and logical live GPU bytes. Advancing phases generate
536,576 cells across eight field generations, integrate 4,147,200 pixels and
evaluate 196,608 shadow texels. Logical residency is 265,548,696 bytes.
The summary's workgroup geometry intentionally changes while its page count
and 512 logical input samples per full page remain the same.

The fixture waits for device completion between frames. These are serialized
pass costs, not throughput, queue overlap or end-to-end FPS measurements.
The performance fixture uses one procedural view. Stateful and multi-view
behavior are covered by correctness tests, without new performance claims.

```console
MONO_WEATHER_GPU_BENCH_TICK=128 just weather-gpu-bench 3 bench 1920 1080 256
```

The comparison keeps raw benchmark output in memory and prints results to
stdout. No benchmark output files are added.

## Verification

Development and strict CI client and renderer-test builds pass. Both presets
also pass all 87 shader resource contracts and MSL translation checks. Metal
and other physical devices have not been run.

Four new GPU cases exercise the optimized code directly:

- Compact density decoding covers every legal density sum, every child and
  both unsafe-marker states, with unrelated packed channels varied.
- Density-only evaluation matches full packed GPU evaluation and independent
  canonical CPU samples at boundaries, negative and extreme coordinates,
  phase endpoints, intermediate ticks and UINT64_MAX.
- Summary readback matches independent CPU accumulation for complete and
  partial dense grids, including the maximum total and thin edge pages.
- Sparse summary readback covers negative origins, dense and compact pages,
  certified-empty pages, entirely unsafe pages and mixed unsafe assemblies.

The final affected GPU selection passes **941,447 assertions in 21 cases** in
each preset. It includes canonical field parity, seams, eviction/refill,
shadows, portal and mirror composition, precipitation, lightning and stateful
GPU checks. Full headless renderer checks pass **822 cases** in each preset
(414,469 development assertions and 414,470 strict CI assertions). Formatting
and diff checks pass.

```console
.cache/build/dev/tests/test_render '[gpu][weather],[gpu][weather-stateful],[cloud-seams]' --reporter compact
.cache/build/ci/tests/test_render '[gpu][weather],[gpu][weather-stateful],[cloud-seams]' --reporter compact
.cache/build/dev/tests/test_render '~[gpu]' --reporter compact
.cache/build/ci/tests/test_render '~[gpu]' --reporter compact
```

The earlier intermittent high-FPS portal-crossing and handoff sample-count
findings remain open in [visual validation](WEATHER_VISUAL_VALIDATION.md).
These compute changes do not resolve those findings.
