# Wrapped text profile, 2026-10-06

The `just imagegraphfont-wrapped-text-bench 5` run completed with its single
workload test case and all 28 assertions passing. It used the `bench` preset,
optimized first-party code at `-O3` (`MONO_OPTIMISE_LEVEL=3` in the CMake
cache), GCC 13.3.0, and an AMD Ryzen 9 9900X. The run was headless and CPU-only,
with heap profiling and Tracy enabled.

Each workload had eight warmups followed by five measured samples. Owner times
are medians; ranges show the minimum and maximum of those five samples.
Paragraph input repeats `A A\n` 128 times; spaces input contains 256 spaces.
The batch submits 64 `A A` requests with widths `12 + row / 128`. Work units,
workspace and candidate bytes below are native measurement counters, including
for the trimmed-text case; they are not whole-evaluation totals.

| Workload | Output dimensions | Work units | Workspace bytes | Candidate bytes | Owner median, us | Sample range, us |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Paragraph | 8 x 2560 | 17,440 | 8,216 | 600 | 5.410 | 5.340 to 6.683 |
| Spaces | 1016 x 1270 | 139,296 | 4,120 | 344 | 25.648 | 25.618 to 25.669 |
| Batch | 64 distinct fractional-width requests, each 8 x 20 | 24,704 | 72 | 5,080 | 4.088 | 4.078 to 4.509 |
| Trimmed text | 8 x 2560 RGBA8, 81,920 bytes | 17,440 | 8,216 | 600 | 444.525 | 437.602 to 453.342 |

The trimmed-text case retained 256 bytes from 512 input bytes while
preserving full image height. Its native measurement self time was 5.119 us,
trim self time was 0.722 us, and text self time was 300.184 us. The other native
measurement self medians were 5.270 us for paragraph, 25.518 us for spaces,
and 3.958 us for batch.

Native measurement allocation deltas were 9,129 bytes across 7 blocks for
paragraph, 4,777 bytes across 7 blocks for spaces, and 4,504 bytes across 6
blocks for batch. Trimmed-text native measurement allocated 9,345 bytes across
5 blocks; image allocation added 81,920 bytes in one block. These are allocation deltas for the named scopes. Whole-frame allocation
deltas also include collector and untagged overhead; they are not
workload-only allocation figures.

The benchmark wrapper medians were 196.379 us for paragraph, 217.529 us for
spaces, 196.299 us for batch, and 701.618 us for trimmed text. These include
verification and snapshot recording; owner medians above are the workload
timings.

The first three workloads used already-decoded BDF input. Constructor decoding,
configuration, native document roundtrip, compile, and `Verify` happened outside
the measured frame. Only the trimmed-text workload measured `Bind` plus
`Evaluate`. The source-compatible space endpoint swap remains in place. Its
unusual dimensions are recorded here rather than treated as a defect to fix.

Repeated hashes are deterministic checks against the native fixture only. They
do not establish licensed-font, platform-font, or GPU parity. The tiny-cap
trimmed case refuses during `Bind`; it does not reach the evaluator. Thirteen
retained readings each reserve `MAXIMUM_NODES` buffers before and after the
profiled call, which explains the observed process live-byte growth. Reported live bytes, peak bytes and profiler overhead are absolute process
readings, not per-operation deltas. This run does not measure a leak slope.

This is one warm-cache process with five samples per workload and profiling
overhead enabled. It does not generalize to other fonts or scales and does not
establish shipped cost. The measurements justify no runtime change by
themselves.

## Provenance

The profiled revision was `3345d07f`, with the workload changes present in the
working tree. The profiling job exited successfully. The final compile command
placed `-O3` after `-O2`, so `-O3` was effective.

| Artifact | SHA-256 |
| --- | --- |
| `WrappedText.cpp` | `e8101a25c9fd2893b100fdd124a5de500ea45396d2da40d045da7626187a886f` |
| `WrappedTextBoundary.hpp` | `6a33d381854b1073214783f7e2abdea1d415712982b3bf0e4dd4a77763702982` |
| Benchmark binary | `fea1bbf65441811faa205432a18098a51f57955d4c69d933581a4746ac47217b` |
