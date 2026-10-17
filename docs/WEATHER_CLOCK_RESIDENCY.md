# Resident weather time and client synchronization

The residency audit found two gaps: procedural shaders received time only
through CPU uniforms, and network clock refresh depended on ordinary component
replication. The implementation now retains an integer GPU counter and sends
independent reliable clock anchors every second.

## Ownership and transfers

| Data | Owner and residency |
|---|---|
| Packed cells, assemblies, page tables and summaries | Retained GPU buffers |
| Cloud history, reconstructed images and shadow maps | Retained GPU textures |
| Stateful cells, falling water, terrain flags and solver scratch | Retained GPU buffers; checkpoint decode is transient |
| Phase counter and displacement | One retained 32-byte GPU buffer per active world/pipeline generation |
| Authoritative definitions, anchors and events | Shared ECS and copied network messages |
| Client clock estimate and correction debt | Local ECS, driven by monotonic time |
| Visibility, allocation plans and cache signatures | Bounded CPU control metadata |

Normal rendering neither mirrors the volume on the CPU nor reads back the GPU
clock. A changed logical sample uploads one 32-byte clock command. Other uploads
include view/appearance uniforms, changed page descriptors and explicit stateful
checkpoint/terrain input. Stateful validation still downloads grouped
diagnostics, `ceil(cellCount / 64) * 32` bytes per staged transition. This is
separate from a physical-volume mirror or clock readback.

## GPU progression

[WeatherGpuClock.cpp](../mono.engine/render/src/WeatherGpuClock.cpp) plans
absolute anchors or increments. [weather-clock.comp](../mono.engine/resources/shaders/weather-clock.comp)
adds the phase delta using two uint32 words with carry, then adds displacement
deltas modulo uint32. Integer arithmetic preserves large ticks and wrapped
wind coordinates without accumulating floating-point timer drift.

Generation, summaries, cloud integration and shadows read the resident result.
Their ordinary time uniforms are zeroed when the resident path is active.
Portal and mirror captures reuse that generation's counter. Uniform evaluation
remains available for standalone fixtures and explicit fallback paths.

The host supplies logical tick deltas from the smoothed clock, rather than
incrementing once per rendered frame. Repeated views and unchanged ticks do no
clock dispatch or upload. A missed render advances directly to the current
sample without replaying previous procedural generations. The GPU does not run
an autonomous background timer while no work is submitted.

The counter receives an absolute anchor initially, every five source-clock
seconds, and after identity, revision, rate, backward-time or large-seek changes.
An ordinary periodic anchor agrees with the already accumulated counter.
Command cancellation invalidates its publication and forces an absolute retry.
The allocation remains resident until its generation/world is released.

## Synchronization and smooth correction

The server and Studio Play links prepare fresh samples every second, including
when ordinary replication is slower. Reliable admission respects entity interest.
Per-recipient publication receipts prevent a blocked client from causing repeated
sends to healthy clients. Retries retain the latest accepted anchor. Publication
uses the bounded current layer set, excluding retired providers' leftover clocks.
Pause, accepted edits and automatic fault suspension request immediate anchors.
Heartbeat ordering advances the sample sequence, without stepping weather time.

Clients already estimate receipt time using local monotonic time and filtered
RTT/2, capped at half a second of delay compensation. Small corrections change
the local base rate within 90% to 110%:

- A client ahead of the server slows down.
- A client behind the server speeds up.
- Fresh anchors measure error against the server's advancing timeline.

This closes the correction debt in finite time for consistent samples. At the
10% limit, a one-second offset takes about ten seconds to close. A fixed midpoint
would leave a residual offset; the moving server estimate remains the target.
The GPU receives the resulting integer deltas, preserving the same correction.
Authoritative gameplay time is never adjusted to follow a client.

Existing discontinuity policy remains explicit: new epochs, accepted timeline
changes and errors exceeding two seconds reconstruct and invalidate history.
Without a fresh sample, the local estimate extrapolates for at most two seconds,
then holds. Heartbeats are scheduled every second; transport backpressure and
loss can still delay them. RTT/2 assumes symmetric delay and does not measure
sender queue age, so exact simultaneous display time is not guaranteed.
Stateful fields retain their existing authoritative checkpoint reconciliation.
Clock agreement alone does not establish identical floating-point solver grids
across devices.

## Device measurements

Compared with `ce69e58d`, using the optimized `bench` preset, RTX 4090, Vulkan,
driver 580.173.02, 1920x1080, one procedural view and a 256x256 shadow cascade.
Two alternating runs at each starting tick 0, 128 and 250 requested three
benchmark samples each. The six run medians for the new clock pass range from
5.376 to 5.632 us, with median **5.504 us**.

The comparison checks equal field/summary/ray/shadow work and exactly **32 extra
logical GPU bytes** in each phase. Eight advancing frames add eight clock
dispatches and 256 uniform bytes. Eight same-tick frames record eight clock
cache hits with zero clock dispatches and zero clock uniform bytes. All frame
compute-dispatch totals include the clock pass. These are serialized pass costs
with completion waits, not FPS or queue-overlap measurements. Other desktop GPU
work was present. Benchmark output stays on stdout.

```console
MONO_WEATHER_GPU_BENCH_TICK=128 just weather-gpu-bench 3 bench 1920 1080 256
```

## Verification

GPU readback tests prove exact carry, modular displacement, pause/reuse,
cancellation, reanchoring and stable allocation. Deliberately incorrect uniform
time still produces CPU-reference cells and unsafe compact-page summaries when
the resident counter is bound. Normal rendering does not use those readbacks.

The affected GPU selection passes 950,950 assertions in 23 cases in development
and strict CI, including portal/mirror weather, seams, shadows and stateful
checks. Headless renderer checks pass 830 cases in each preset. All 88 shader
contracts and MSL translations pass; device execution was checked on Vulkan.

Clock tests exercise moving-target convergence at 30 and 144 FPS, unequal RTTs,
both skew directions, large ticks, stale samples and the extrapolation cutoff.
Heartbeat fixtures cover reliable capacity pressure, interested and excluded
clients, current membership, retry deduplication, faults, save/load and Studio
delivery. These shared suites pass in development, strict CI and the headless
server preset:

| Suite | Cases | Assertions |
|---|---:|---:|
| ECS | 474 | 135,706 |
| Scene | 670 | 534,378 |
| Weather | 115 | 22,574 |
| Replication | 314 | 52,808 |

Studio Play-link checks pass 36 cases and 5,785 assertions in development and
strict CI. Client component/property invariants pass three cases and 147
assertions in each. The targeted server fault case passes 12 assertions in
all three presets.

The complete headless-server suite passes 106 cases and 3,692 assertions.
Server component/property, weather and MCP checks pass seven cases and 274
assertions in development and strict CI. Client, server and Studio programs
build in both presets; the headless server builds without staged shaders.
Architecture, source scanning, changed-file formatting and diff checks pass.

Broader server validation exposed an outdated weather-resource manifest and
two mismatches in unchanged weather code. The property audit now permits
provably clean no-op writes while still detecting unmarked property/backing
mutations. Default stateful component envelopes round-trip as uninitialized
data, without becoming valid physical checkpoints. Initialized and malformed
physical checkpoint validation remains intact.

The earlier [high-FPS portal handoff findings](WEATHER_VISUAL_VALIDATION.md)
remain separate and unresolved.
