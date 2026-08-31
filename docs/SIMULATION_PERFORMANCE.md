# Simulation performance

## Scope and reproduction

This is the measured map of the simulation hot paths and the batching rules
that follow from it. Run the lightweight ladder with:

```sh
just simulation-sweep 5
```

The heavier replication publish ladder is separate because its 1-to-200-client
fixture takes materially longer to construct:

```sh
just simulation-publish-sweep 3
```

The measurements below were taken on 2026-08-28 on an AMD Ryzen 9 9900X with
12 physical cores and 24 logical CPUs, GCC 13.3.0, and Linux 7.0.0. Each first
party target was rebuilt at the named optimization level. Values are medians
from the three-sample full sweep, so the exact numbers are evidence for this
machine rather than portable thresholds.

| Operation | O0 | O1 | O2 | O3 |
|---|---:|---:|---:|---:|
| ECS `Each`, 100k rows | 22.17 us | 21.88 us | 21.88 us | 21.40 us |
| ECS overwrite, 100k rows | 14.29 ms | 1.44 ms | 1.15 ms | 1.08 ms |
| ECS remove and add, 100k rows | 98.21 ms | 9.61 ms | 8.45 ms | 8.17 ms |
| Physics broadphase, 16k colliders | 10.57 ms | 3.18 ms | 2.77 ms | 2.72 ms |
| Physics tick, 16k stacked bodies | 131.67 ms | 17.02 ms | 16.52 ms | 16.54 ms |
| Physics solve, 16k settled bodies | 87.71 ms | 12.27 ms | 9.50 ms | 8.82 ms |
| Replication survey, 10k entities and 4 observed slots, per entity | 90 ns | 6 ns | 6 ns | 6 ns |
| Empty dispatch, 128 ranges over 4 workers | 14.42 us | 13.29 us | 9.61 us | 8.14 us |

The benchmark runner reports a broad uncertainty interval for worker dispatch
because operating-system scheduling dominates such short samples. The useful
finding is the crossover, not a claim that one optimization level makes thread
wakeup monotonically faster.

## Findings and decisions

The ordinary component walk is already the desired shape: archetypes expose
contiguous spans, `EachBatch` operates once per matching table, and the inner
loop remains eligible for compiler vectorization. `EachParallel` is intentionally not
the default for small ranges. A 100k trivial row walk is about 21 us while an
empty four-worker dispatch alone is about 8 us at O3, before cache movement or
joining is counted.

Structural ECS mutation is not a steady-state iteration path. Moving 100k rows
between archetypes costs 8.17 ms even at O3. Systems should establish their
component shape during construction, then update values through contiguous
walks. A system that toggles a marker over a large population every tick should
usually model the state as a value or build a stable filtered table instead.

Physics is the remaining simulation bottleneck. At 16k stacked bodies, solving
accounts for 8.82 ms of a 16.54 ms O3 tick, and broadphase work remains the next
large block. O1 removes most of the debug-build arithmetic cost, while O2 and O3
only narrow it further. Future simulation optimization should therefore target
contact islands, sleeping, and broadphase candidate volume before trying to
shave nanoseconds from ECS dispatch.

Replication performs one table survey per scene rather than one component
lookup per entity and declared slot. Signature slots are batched in the job
pool, client lanes apply their own crossover, and `Authority::PublishMany`
coalesces signing across scenes. The observed-slot row falls from 90 ns per
entity at O0 to 6 ns at O1 through O3. The separate publish ladder remains the
source of truth for per-client interest, delta building, ordering, and packing.

Worlds are the coarsest local batch. A lone world stays on the driver so its
systems can use the worker pool. Multiple substantial worlds keep stable pinned
lanes. `UniverseSettings::WorldParallelFloorMilliseconds` uses the sum of the
previous measured world costs, multiplied by ticks owed, to avoid handing tiny
batches to workers. The default 0.05 ms floor comes from the measured 8 to 58 us
dispatch range and can be set to zero to force lane dispatch. In the O3
ten-sample check, 10 worlds of 2k rows fell from 32.01 us before the gate to
5.33 us after it; 50 empty worlds fell from 59.95 us to 21.66 us.

Process isolation is a deployment boundary, not an inner-loop optimization.
`Isolation::Dedicated` gives one world its own supervised process, while shared
worlds are grouped according to the host plan. Worlds cross that boundary only
through copied, named messages. Inside a process, world lanes, ECS table batches,
replication batches, and physics phase batches remain the compute units.

No hand-written SIMD was added to storage or replication. Their contiguous,
typed loops preserve regular strides without introducing a second scalar and
SIMD implementation, and the O1-to-O3 ladder shows where optimization helps. Hand-vectorizing the
contact solver would be a separate parity-sensitive change and needs a profile
of the exact contact shape first.
