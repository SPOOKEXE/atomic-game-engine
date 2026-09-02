# Rings world stress test

Tested on 2 September 2026 at commit `684a30ee0373dd513d43ea9f760f6b8dbbe19f52`
with client `0.21.1`.

## Result

Ten Rings worlds sustained the requested 60 Hz with no dropped ticks. One
hundred worlds did not: simulation fell to 35.3 Hz and dropped 357 ticks during
the 20 second measured run. At 1,000 worlds it reached 3.4 Hz, with a 261.6 ms
median frame and 220.8 ms of mean frame-owner wait time.

The main limit is CPU work inside the worlds, especially the Luau heartbeat.
At 1,000 worlds the pinned workers reported 2,538.6 ms of aggregate CPU work per
tick. The machine has 12 physical cores, so a 60 Hz tick has about 200 core-ms
available before frame-owner and presentation work. The 100-world run already
needed 265.9 core-ms per tick and therefore could not hold 60 Hz.

Memory has a separate load-time problem. The zero-frame 100-world probe held
4.40 GiB immediately after world construction, of which 4.38 GiB was untagged.
After two ticks it fell to 137 MiB. The 1,000-world run peaked at 43.95 GiB,
then ended at 1.13 GiB live. This is a load or first-tick step, not evidence of
a linear leak, but the missing heap tag prevents a more exact source claim.

No scene rendering occurred in these runs. Every count reported zero draw
calls and zero triangles, while the logical GPU heap stayed at 1.1 MiB live and
2.1 MiB peak. The client still ran its offscreen presentation preparation, so
the results include simulation, script, world publication, composition, and
damage-signature costs without measuring raster work.

## Setup

| Item | Value |
|---|---|
| CPU | AMD Ryzen 9 9900X, 12 cores and 24 threads |
| GPU | NVIDIA GeForce RTX 4090, Vulkan backend |
| Memory | 123 GiB |
| OS | Linux 7.0.0-30-generic, x86-64 |
| Compiler | GCC 13.3.0 |
| Build | `bench`, `RelWithDebInfo`, first-party `-O3`, heap hooks on |
| Job system | 23 workers, 12 physical-core lanes |
| Scene | `Rings.luau`, 512 anchored moving parts in 8 rings per world |
| Client | headless, uncapped, 640x360, 60 Hz simulation |
| Main run | 20 seconds after load, normal pinned-world execution |
| Frame history | Last 5 seconds, at most 20,000 frames |
| Heap history | One-second samples; report fit covers the retained run |

The command shape was:

```sh
client --headless --uncapped --frames 1000000000 \
    --profile-seconds 20 --width 640 --height 360 \
    --worlds N --entities 512 --script assets/examples/Rings.luau \
    --profile-snapshot N.profile.txt \
    --heap-report N.heap.txt --heap-warmup 5
```

The normal runs preserve real world-level overlap. A second 100-world run used
`--force-serial-compute` for 12 seconds because the complete nested flame tree
is only retained on the frame-owning thread. Its timings are used only to
locate work, not as the scaling result.

The main worktree gained unrelated live edits while the test was starting and
temporarily did not compile. The measurements therefore came from a clean,
detached build of the named commit. This keeps the tested source stable and
excludes those later edits.

## Scaling results

`Worker CPU/tick` is the sum of worker busy time and may exceed wall time when
cores overlap. `Dropped` is the engine's raw counter, not a comparable rate:
catch-up is capped, so a slower run can discard several owed ticks at once.

| Worlds | Moving parts | Load to ready | Frames | Tick Hz | Dropped | Frame p50 | Frame p99 | Mean idle | Worker CPU/tick |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 5,120 | 0.34 s | 45,149 | 60.0 | 0 | 0.275 ms | 3.198 ms | 0.194 ms | 23.502 ms |
| 100 | 51,200 | 1.48 s | 708 | 35.3 | 357 | 27.516 ms | 33.604 ms | 23.700 ms | 265.871 ms |
| 250 | 128,000 | 3.61 s | 232 | 11.5 | 861 | 77.937 ms | 142.651 ms | 64.935 ms | 704.113 ms |
| 500 | 256,000 | 10.97 s | 115 | 5.7 | 1,039 | 160.162 ms | 221.504 ms | 132.467 ms | 1,430.151 ms |
| 1,000 | 512,000 | 21.42 s | 70 | 3.4 | 966 | 261.601 ms | 299.941 ms | 220.782 ms | 2,538.604 ms |

The 1,000-world frame history contains only 19 frames. Its median is useful,
but its p99 and maximum are the same sample and should not be treated as a
stable tail estimate.

## Heap results

| Worlds | Peak RSS | Tracked live at exit | Tracked peak | Total allocated | GPU live |
|---:|---:|---:|---:|---:|---:|
| 10 | 0.64 GiB | 33.38 MiB | 492.95 MiB | 1.58 GiB | 1.1 MiB |
| 100 | 4.67 GiB | 137.50 MiB | 4.42 GiB | 11.27 GiB | 1.1 MiB |
| 250 | 11.39 GiB | 305.24 MiB | 11.00 GiB | 24.03 GiB | 1.1 MiB |
| 500 | 22.61 GiB | 589.11 MiB | 21.98 GiB | 45.99 GiB | 1.1 MiB |
| 1,000 | 45.00 GiB | 1.13 GiB | 43.95 GiB | 90.37 GiB | 1.1 MiB |

The load-only probe used `--frames 0`. At 100 worlds it finished in 2.69
seconds with 4.40 GiB still live and 4.42 GiB peak. A 0.1-second probe that ran
two ticks had the same peak but only 137 MiB live. This proves that most of the
roughly 45 MiB-per-world peak belongs to construction or first-tick cleanup.
The tracker attributed 4.38 GiB of the load-only live heap as `untagged`, so
adding a bounded heap scope around scripted world construction is required
before changing an allocator on this evidence.

The 20-second heap fits were low, generally around 0.14 to 0.21 for the largest
positive slopes. Their shape was an early step followed by a level, not a
credible line. These runs do not show a leak. They are also too short to replace
the normal minute-scale heap soak when leak detection is the question.

## Top 10 bottleneck locations

The table removes duplicate parents and the per-world dynamic labels such as
`client.world.266`. Flame rows are per occurrence: a system value is the worst
single occurrence in that frame, not the sum across all worlds.

| Rank | Location | Evidence | Reading and next move |
|---:|---|---|---|
| 1 | `mono.engine/world/src/Universe.cpp:746`, pinned world dispatch and join | 2,538.604 ms aggregate worker CPU per tick and 220.782 ms mean owner wait at 1,000 worlds | This is the capacity wall, not one leaf to micro-tune. Lower per-world cost first, then remeasure lane balance and the parallel floor. |
| 2 | `mono.engine/examples/src/Scene.cpp:357`, `mono.engine/scriptluau/src/LuauRuntime.cpp:994`, and `mono.engine/examples/Rings.luau:120`, script heartbeat | In the serial 100-world flame tree, `script-heartbeat` averaged 0.651 ms for the worst occurrence and reached 3.761 ms. `script beat` accounted for 0.639 ms and 3.734 ms of that. | The 512-orbiter Luau loop and `BulkMoveTo` are the largest measured world leaf. Profile the loop, CFrame creation, and bulk binding separately before changing them. |
| 3 | `mono.client/src/Client.cpp:352` and `mono.client/src/Scene.cpp:1131`, scripted world construction | The zero-frame 100-world probe held 4.40 GiB, with 4.38 GiB untagged. Load-to-ready rose to 21.42 s and tracked peak reached 43.95 GiB at 1,000 worlds. | This is the largest memory bottleneck and the largest attribution gap. Add a heap and time scope around each load stage, especially `LoadScene`, VM creation, and the 512 `Instance.new` calls. |
| 4 | `mono.client/src/Client.cpp:2472`, all-world pre-render and publication | Mean was 9.400 ms at 1,000 worlds. The 250-world maximum was 44.097 ms. | The frame owner enters every world, calls `PresentMany`, enters every world again, and publishes every draw list. Cache or batch only after separating those passes in the profile. |
| 5 | `mono.client/src/Compositor.cpp:145`, `Compositor::Compose` | Mean was 16.324 ms at 1,000 worlds, the largest frame-owner leaf there. Its live buffer was exactly 100 MiB at 1,000 worlds. | It scans every slot, copies every payload, and offsets every copied instance. Headless mode still pays this CPU cost. Avoid recomposing unchanged world packets or add a no-presentation simulation mode for this kind of test. |
| 6 | `mono.client/src/Client.cpp:3440` and `mono.engine/render/src/WorldPresentation.cpp:89`, presentation signature | Mean was 8.473 ms at 1,000 worlds, up from 0.086 ms at 10 worlds. | `ScenePresentationSignaturesOf` hashes the combined instance span each frame. Reuse a source revision or a composited signature when the same packet is seen again. |
| 7 | `mono.client/src/Client.cpp:629` and `mono.client/src/Client.cpp:1007`, content demand references | Mean was 4.671 ms at 1,000 worlds, up from 0.009 ms at 10 worlds. | `RequestWantedContent` walks every simulated world. Check why `ContentRequested` remains hot and use a changed-world queue if the revision gate is already proving most worlds unchanged. |
| 8 | `mono.engine/physics/src/SyncBroadphase.cpp:120`, static broadphase synchronization | At 1,000 worlds the path held 141.53 MiB live, including 78.12 MiB self and 62.50 MiB in `physics.index-static`. The serial 100-world maximum was 0.608 ms for one occurrence. | Rings moves anchored parts by transform, so the nominally static set changes. Confirm whether scripted anchored motion should be indexed as dynamic or whether the static rebuild can consume a batched transform revision. |
| 9 | `mono.client/src/Scene.cpp:83`, default `move-camera` | The serial diagnostic averaged 0.082 ms and reached 0.167 ms for the worst world. Under pinned contention the 100-world mean was 1.799 ms and maximum was 3.825 ms. | The demo camera calls `FindSpawn`, which walks the tree, on every tick in every world. Cache the fact that Rings has no spawn or author a camera in the demo. |
| 10 | `mono.client/src/Scene.cpp:1235`, `mono.engine/scene/src/Visibility.cpp:124`, and `mono.client/src/Scene.cpp:1272`, visibility and instance collection | In the serial tree `render preparation` averaged 0.075 ms and reached 0.821 ms; `collect-instances` reached 0.780 ms. `sync rendered.walk` held 8.98 MiB at 1,000 worlds. | The first presentation walks 512 descendants per world and builds a copied draw list. Its steady memo is effective, but construction and first presentation still scale directly with world count. |

## Smaller measured locations

At 1,000 worlds, frame-owner `Universe::Tick` overhead outside worker work was
2.892 ms mean, `schedule worlds` was 0.610 ms, `barrier` was 0.184 ms, and
`input` was 0.112 ms. These are real linear walks, but none is the current wall.
The profiler panel itself was present because collection was enabled; the
serial diagnostic measured it at 0.272 ms mean and it is not game work.

## Raw evidence

The generated captures are under `.cache/world-stress/`:

- `10`, `100`, `250`, `500`, and `1000` each have `.log`, `.time.txt`,
  `.profile.txt`, and `.heap.txt` files.
- `100-serial.profile.txt` is the complete serial flame-tree diagnostic.
- `100-load-only.heap.txt` is the zero-frame load attribution probe.
- `100-short.heap.txt` is the two-tick cleanup probe.

The heap reports recorded zero dropped heap scopes and zero foreign frees.
