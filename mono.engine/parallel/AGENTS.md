# parallel - module invariants

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
- **Parallel is not free, and below a crossover it is negative.** Measure the
  system you are writing, in `bench`, and put the number in a comment.

## The crossover is a duration, and every constant here states it in rows

Re-measured at `-O3` on 24 threads, and the two answers are thirty-two times
apart:

| body | crossover | serial work at the crossover |
|---|---|---|
| three float adds per row | ~262,144 rows | 49 us |
| `physics::IntegrateMotion`, a `CFrame` per row | ~8,000 rows | 29 us |

In rows they share nothing. In microseconds they are the same number, and that
number is one handover. **So the threshold every caller actually wants is
"about thirty microseconds of work", and `grain * MINIMUM_GRAINS` can only ever
express it for one row cost.** Neither constant is going to be right for a
caller it was not measured on; both callers that measured pass a grain of their
own, and `Jobs::For`'s `minimum` is there for the ones whose index is not a row
at all.

Ceilings, while the numbers are here: past the crossover the cheap body tops out
near 1.3x and the `CFrame` body near 5.6x. The cheap one is bandwidth bound
rather than thread bound - at 500k rows both paths stream twelve megabytes.

## What a handover costs, and where it goes

`engine.parallel.bench.dispatch` measures an empty `For` and nothing else:

| | |
|---|---|
| the decision not to dispatch | 48 ns |
| dispatched, 8 empty ranges, 23 workers | 31 us |
| dispatched, 8 empty ranges, **one** worker | 2.3 us |
| each further empty range | ~95 ns |

**It is linear in the pool, not in the work.** Every worker wakes, finds
nothing left to claim, and then queues on `Pool::Guard` to decrement
`Batch::Outstanding` - so a batch cannot finish until all twenty-three have
taken one mutex in turn. That serialised join, not the `notify_all`, is why a
short span cannot repay a dispatch, and it is what to attack if the crossover
ever has to come down. Doing so is a rewrite of the join, not a change to a
constant.

## The calling thread works

`Start(0)` leaves one core for the caller, because the caller drains ranges too.
A pool sized to the full core count oversubscribes on every `For`.
