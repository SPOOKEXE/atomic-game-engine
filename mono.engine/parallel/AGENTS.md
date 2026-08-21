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

## What a dispatch buys, when the ranges hold work

`engine.parallel.bench.contention` puts the same million rows of real work
through every arrangement, so the rows divide against each other. On a
twenty-four thread machine, at `-O3`:

| | |
|---|---|
| serial, no dispatch at all | 2.98 ms |
| 1 worker | 2.24 ms |
| 2 workers | 1.71 ms |
| 4 workers | 1.37 ms |
| 23 workers | 260 us |
| 23 workers, grain 256 | 343 us |
| 23 workers, grain 65536 | 325 us |
| 23 workers, one row in a thousand a hundred times dearer | 1.21 ms |
| the same million rows as 64 dispatches of 16k | 3.05 ms |

**The last row is the one to remember, and it did not dispatch.** Sixteen
thousand rows is under `DEFAULT_GRAIN * MINIMUM_GRAINS`, so all sixty-four calls
ran whole on the caller and the row lands on the serial figure. A system that
batches its work per chunk rather than per tick hands the pool spans it silently
refuses to split; the profile shows a busy calling thread and idle workers, and
nothing anywhere says the word "inline".

The grain rows are what keeps 4096 where it is for a body of this cost - 256 is a
quarter worse for handing over ranges too small to pay, 65536 a fifth worse for
leaving workers with nothing to claim - and the imbalance row is the reminder
that `For` splits by index count and cannot know what an index costs.

## The calling thread works

`Start(0)` leaves one core for the caller, because the caller drains ranges too.
A pool sized to the full core count oversubscribes on every `For`.
