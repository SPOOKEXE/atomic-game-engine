# v0.25 MCP performance evidence

Run in the `bench` preset. CMake reported `RelWithDebInfo`, `-O3`, and
`MONO_HEAP_PROFILE=ON`. This is release-optimised profiling.

## MCP control profile

Commands:

```sh
just mcp-control-profile
just mcp-capture-hook-soak 128
```

The benchmark uses a local `control::Surface` and installs the production
capture rows through `Surface::ActivateHook`. It measures `tools/list`,
validation dispatch with empty arguments, successful `capture` submission with
a valid world revision and unique operation ID, hook activation, and empty-ticket
drain. The successful submission result is checked for a queued ticket, then
the synthetic bridge ticket is cancelled and released outside the timed call.
Each operation has eight warmups and sixteen measured calls per invocation.
With `--samples 1`, the runner invokes each benchmark nine times, for 144
samples per operation. Allocation counts are per-call allocation events from
`HeapProfile::Totals().TotalBlocks`; times are nanoseconds.

Three paired control runs used the same benchmark source in detached HEAD
`97c2a250c3b4f43190c2155dce1e886c36aa426e` and the aggregate worktree.
The source hashes matched. A compile-time capability check registers four
cleanup tool names on current `HookRegistration`; HEAD has no such API and
skips that registration. Active tool rows, request bytes, successful queueing,
bridge cleanup, and the empty-ticket drain workload are otherwise the same.
Activation includes the current cleanup registration cost. Each range below
spans three runs of 144 samples; worst is the largest observed per-run maximum.

| Operation | HEAD p50 ns | Current p50 ns | HEAD p99 ns | Current p99 ns | Worst HEAD/current ns | Allocation blocks HEAD/current |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `tools_list` | 154059-154440 | 154580-159780 | 162976-219201 | 171072-225874 | 225052 / 242655 | 2480 / 2480 |
| `tool_dispatch` | 5841-5861 | 5881-6031 | 5901-6362 | 6372-8566 | 7855 / 8576 | 104 / 104 |
| `capture_submit` | 16562-16602 | 16781-17092 | 19337-21580 | 27993-32551 | 23674 / 75992 | p50 256 / 256; max 258 / 258 |
| `hook_activate` | 6653-6683 | 6723-6873 | 6733-7014 | 8656-9538 | 8726 / 68839 | 148 / 151 |
| `hook_drain` | 2665-2705 | 2705-2765 | 2776-3106 | 2765-3557 | 5640 / 62628 | 10 / 10 |

The current-only soak repeatedly activates and removes the capture hook around
a synthetic bridge ticket retaining 65,536 bytes. The bridge requires three
drain pumps, reports a terminal ticket, then releases its buffer. Each cycle
verifies that new submissions are refused, `release_capture` remains visible
through terminal completion, the ticket is released once, and no ticket buffer
remains. In 128 cycles it refused 128 submissions, released 128 tickets, and
returned from 582,947 to 582,947 tracked live bytes, with zero fitted slope.
Terminal release p50/p99/max was 3,036/4,609/30,607 ns with 11 allocation
blocks. This synthetic fixture does not establish renderer, host shutdown, or
GPU resource ownership. HEAD cannot perform an equivalent draining cleanup
soak because its closed hook hides those tools.

The client control gate also exercises the real `ScriptDataCaptureBridge`
capacity during close. Across 16 hook activations, each cycle queues six
tickets, verifies a seventh is refused both before and after close, then polls
and releases all six terminal tickets through MCP while the hook drains. The
capture submission row disappears on close, cleanup rows remain until the last
release, and every cycle ends with no outstanding ticket. The focused case
passed 803 assertions. The full `[client][control]` filter passed 926
assertions in 12 cases in the development preset. This is a queue and lifetime
stress gate, not a render payload measurement.

`just data-capture-hook-bench 5` also ran the renderer hook dispatch fixture
without opening a device. Its five-sample `connect arm call cancel` workload
over 64 lifecycles reported 35.23 microseconds per lifecycle with a 5% spread.
That earlier result was current-only under a busy host.

## Renderer capture profile

Command:

```sh
just data-capture-gpu-profile
```

This profile uses the bench preset, Vulkan, one emissive plane, 64x64 output,
and the `rgb_linear_hdr` channel. It performed 270 captures after warmup. It
reported 32,768 record bytes per capture, CPU heap block counts p50/p99/max of
295/295/299, roundtrip CPU nanoseconds p50/p99/max of
1,722,987/4,654,243/4,863,295, readback latency nanoseconds p50/p99/max of
1,105,437/3,845,765/4,316,428, readback polls p50/max of 2/3, GPU time p50 of
5,888 ns, zero dropped captures, and tracked GPU allocation counts of 0/0/0.
The host/device staging payload was 65,536 bytes. These were current-only
measurements.

### Matched fixture runs

The following runs used the same profile fixture, command, bench preset,
Vulkan backend, single emissive plane, 64x64 resolution, `rgb_linear_hdr`
channel, five warmups, and 270 measured captures. Baseline source was detached
HEAD `97c2a250c3b4f43190c2155dce1e886c36aa426e`; only the profile fixture was
copied into that checkout. The current run used the aggregate dirty worktree,
so this is not an isolated Stage 4 code delta.

Command in each checkout:

```sh
MONO_DATA_CAPTURE_PROFILE=1 MONO_DATA_CAPTURE_PROFILE_CAPTURES=30 timeout --foreground --kill-after=10s 180s ./.cache/build/bench/bench/bench_render --suite engine.render.bench.data-capture-gpu-profile --samples 1
```

Baseline output:

```text
data-capture-gpu-profile preset=bench scene=single_emissive_plane backend=vulkan resolution=64x64 channels=rgb_linear_hdr warmup=5 captures=270 queue_depth_max=1 record_bytes_per_capture=32768 cpu_allocation_blocks_per_capture_p50=295 cpu_allocation_blocks_per_capture_p99=295 cpu_allocation_blocks_per_capture_max=299 gpu_buffer_allocations=0 gpu_texture_allocations=0 gpu_transfer_allocations=0 host_reserved_bytes_per_capture=65536 device_staging_reserved_bytes_per_capture=65536 cpu_roundtrip_ns_p50=1281462 cpu_roundtrip_ns_p99=1532634 cpu_roundtrip_ns_max=2378978 readback_latency_ns_p50=1062529 readback_latency_ns_p99=1099870 readback_latency_ns_max=2116773 readback_polls_p50=2 readback_polls_max=3 gpu_ns_p50=5632 drops=0
```

Current aggregate worktree output:

```text
data-capture-gpu-profile preset=bench scene=single_emissive_plane backend=vulkan resolution=64x64 channels=rgb_linear_hdr warmup=5 captures=270 queue_depth_max=1 record_bytes_per_capture=32768 cpu_allocation_blocks_per_capture_p50=295 cpu_allocation_blocks_per_capture_p99=299 cpu_allocation_blocks_per_capture_max=299 gpu_buffer_allocations=0 gpu_texture_allocations=0 gpu_transfer_allocations=0 host_reserved_bytes_per_capture=65536 device_staging_reserved_bytes_per_capture=65536 cpu_roundtrip_ns_p50=1291611 cpu_roundtrip_ns_p99=2338951 cpu_roundtrip_ns_max=2532476 readback_latency_ns_p50=1062860 readback_latency_ns_p99=2115601 readback_latency_ns_max=2123646 readback_polls_p50=2 readback_polls_max=3 gpu_ns_p50=5632 drops=0
```

Both runs completed all captures with the same record size, staging payload,
poll count median, GPU time median, and zero drops. Timing was collected once
per revision while the host was busy: GPU utilization was observed around
32-39%, including an unrelated Barotrauma process, and external builds were
active. The small median differences do not establish a speedup or regression.

Three further paired runs used that exact render profile fixture source in a
temporary HEAD archive and the aggregate worktree. The archive used the same
pinned vendor submodules and patched vendor source; its vendor preparation
script only located the shared prepared tree because an archive has no `.git`.
Each run used the same bench preset, Vulkan backend, emissive-plane scene,
64x64 resolution, `rgb_linear_hdr` channel, five warmups, and 270 measured
captures. All six runs completed with zero drops, 32,768 record bytes,
65,536-byte host and device staging reservations, zero tracked GPU buffer,
texture, or transfer allocations, readback poll p50/max of 2/3, and GPU
duration p50 of 5,632 ns.

| Pair | Revision | CPU roundtrip p50/p99/max ns | Readback p50/p99/max ns | CPU allocation blocks p50/p99/max |
| --- | --- | ---: | ---: | ---: |
| 1 | HEAD | 1409496 / 2439879 / 2549556 | 1067233 / 2119668 / 2186635 | 295 / 299 / 299 |
| 1 | Current | 1423002 / 2746916 / 4780794 | 1069778 / 2520521 / 4537247 | 295 / 295 / 299 |
| 2 | HEAD | 1312754 / 1512850 / 2352215 | 1065149 / 1083474 / 2127473 | 295 / 295 / 299 |
| 2 | Current | 1274873 / 2367083 / 2468464 | 1061793 / 2116653 / 2128616 | 295 / 299 / 299 |
| 3 | HEAD | 1280323 / 2344030 / 2363466 | 1061963 / 2118848 / 2124438 | 295 / 295 / 299 |
| 3 | Current | 1276556 / 2330044 / 2429320 | 1062044 / 2114600 / 2118827 | 295 / 295 / 299 |

GPU utilization was observed at 31-38% during these runs, and another process
was active. The different p99 and maximum timings are inconclusive under that
contention. No speedup or regression is claimed.

## Stage 4 gate status

The renderer and control fixtures now have matched HEAD versus aggregate-worktree
runs, including successful capture submission and per-operation worst case and
allocation counts. The control soak verifies terminal cleanup only in the
current implementation because HEAD lacks that behavior. Host contention and
unrelated aggregate worktree changes prevent a causal timing conclusion.
These profiles establish workload completion and bounded resource counts under
the stated fixtures; they do not establish a Stage 4 speedup or regression.

### Paired run with the desktop in use, 24 September 2026

The user chose to keep Brave, Discord, Spotify, Steam, and the desktop open for
the acceptance benchmark. Three new HEAD/current pairs ran sequentially in the
`bench` preset with matching control and GPU fixture source hashes. The GPU
pairs ran after the benchmark builds and product tests ended; concurrent work
was not ruled out for every control sample. Observed GPU utilisation before
the GPU pairs was 19 to 29 percent. These are normal-desktop-load results, not
quiet-host timings. The current binary includes aggregate worktree changes.

| Control operation | HEAD p50 range ns | Current p50 range ns | Allocation blocks HEAD/current |
| --- | ---: | ---: | ---: |
| `tools_list` | 153929-155231 | 155131-161162 | 2480 / 2480 |
| `tool_dispatch` | 5881-5891 | 5921-6131 | 104 / 104 |
| `capture_submit` | 16631-22432 | 16671-17242 | 256 / 256 |
| `hook_activate` | 6662-6693 | 6743-7013 | 148 / 151 |
| `hook_drain` | 2665-2675 | 2705-2815 | 10 / 10 |

The GPU fixture completed 270 captures in each of six runs, with zero drops,
32,768 record bytes per capture, 65,536 host and device staging bytes, zero
tracked GPU allocations, and 5,632 ns median GPU time. HEAD CPU roundtrip p50
spanned 1,272,077 to 1,280,392 ns; current spanned 1,270,915 to 1,280,021 ns.
HEAD p99 spanned 1,467,062 to 1,522,547 ns; current spanned 1,411,268 to
2,343,818 ns. The overlapping medians and variable tails do not establish a
causal speedup or regression under the chosen desktop load.

## Product listener shutdown smoke

The development-preset product binaries were launched with finite headless
workloads on 2026-09-24. Each listener answered `initialize` and `tools/list`
over a bound loopback TCP port. After process exit, a new connection to that
port was refused.

| Product | Workload and shutdown | Bound MCP port | Tools | Exit | Socket closed |
| --- | --- | ---: | ---: | ---: | --- |
| Server | `--entities 1 --seconds 8` | 49601 | 32 | 0 | yes |
| CDN | one-file published store, `--port 0 --seconds 8` | 33255 | 8 | 0 | yes |
| Launcher | `--headless`, `launcher_quit` tool | 59469 | 18 | 0 | yes |
| Client | `--headless --entities 1 --frames 600` | 58865 | 30 | 0 | yes |
| Studio | `--headless --frames 600` | 54819 | 40 | 0 | yes |

The CDN store was disposable and removed after the run. These are functional
checks; external Barotrauma and other builds were active. The paired
normal-desktop-load measurements above supersede the pending quiet-host request.
