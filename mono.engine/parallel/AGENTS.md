# parallel — module invariants

L2. Below `ecs`, because a world's thread affinity is a concurrency fact and the
storage at L3 has to be able to record it.

## Four sub-areas, and only one of them exists yet

| Sub | Holds | Status |
|---|---|---|
| `jobs/` | engine-internal dispatch | `Jobs.hpp` |
| `threads/` | the userland `thread` datatype | arrives with L13 |
| `process/` | separate OS processes, supervision, crash isolation | arrives with world-per-process |
| `ipc/` | what crosses a process boundary | same |

Do not put userland thread semantics into `Jobs`. They are different contracts:
`Jobs` is fork-join with no handles, and the userland one has to survive a
script yielding.

## `Jobs::For` blocks, and that is the design

A job system that hands back a handle and lets work outlive the frame needs a
lifetime story for everything the job captured. Nothing needs that yet, and
fork-join is the shape that cannot leak a dangling capture.

If you find yourself wanting `Jobs::Async`, the question to answer first is what
happens when the world it was launched from is destroyed mid-flight.

## One batch in flight

`For` inside `For` would deadlock a fixed pool. The design refuses the shape
rather than defending against it. If nested parallelism becomes necessary, that
is a work-stealing deque and a rewrite, not a flag.

## Grain is the whole game, and the default is for cheap bodies

`DEFAULT_GRAIN` is 4096 because that is what measured best, not because it
looks round. The finding worth keeping: over an ECS integration step of three
float multiply-adds per row, a grain of 256 made the parallel version **twice as
slow as the serial one** at 20,000 rows. Waking a worker costs more than the 256
rows it was handed.

Two consequences:

- **A default grain is a guess about how expensive one row is.** 4096 suits a
  body that does almost nothing. A body that raycasts wants a grain in the tens.
  Pass it explicitly whenever the work per row is real.
- **Parallel is not free, and below a crossover it is negative.** For the
  cheapest possible body that crossover was near 60-80k rows, and the ceiling
  past it was about 3.5x rather than the core count — memory bandwidth, not
  threads. Neither number is knowable in advance. Measure the system you are
  writing, in `release`, and put the number in a comment.

## The calling thread works

`Start(0)` leaves one core for the caller, because the caller drains ranges too.
A pool sized to the full core count oversubscribes on every `For`.
