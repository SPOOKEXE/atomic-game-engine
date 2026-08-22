# parallel - module invariants

L2. Below `ecs`, because a world's thread affinity is a concurrency fact and the
storage at L3 has to be able to record it. It links `core` and nothing else, and
twenty-six modules link it.

## What is here, and there are no subfolders

| Area | Holds | Where it lives | Since |
|---|---|---|---|
| jobs | engine-internal dispatch over one process-wide pool | `Jobs.hpp`, `src/Jobs.cpp` | v0.2 |
| processes | spawning, supervising, crash isolation, output capture | `Process.hpp`, `Capture.hpp` | v0.2, v0.18 |
| ipc | what crosses a process boundary | `Channel.hpp`, `ProcessChannel.hpp`, `src/SocketChannel.*` | v0.2 |
| settings | the two flags this module owns | `Settings.hpp` | v0.15 |
| threads | the userland `thread` datatype | does not exist yet | |

**This file claimed until v0.19 that `process/` and `ipc/` were directories that
did not exist yet.** Both have existed since v0.2, as files rather than
directories, and so has `jobs/`. The only directory under `src/` is `platform/`,
which carries one implementation per operating system of the four socket calls,
the process spawn, the output capture and the affinity queries. Adding a
subfolder is a real change; do not write one into this table before it is there.

Do not put userland thread semantics into `Jobs`. They are different contracts:
`Jobs` is fork-join with no handles, and the userland one has to survive a
script yielding.

## `Jobs::For` blocks, and that is the design

A job system that hands back a handle and lets work outlive the frame needs a
lifetime story for everything the job captured. Nothing needs that yet, and
fork-join is the shape that cannot leak a dangling capture. Root `AGENTS.md`
rule 5 rests on it: a tick stays one thing that starts and finishes, so a
recorded run still replays.

If you find yourself wanting `Jobs::Async`, the question to answer first is what
happens when the world it was launched from is destroyed mid-flight.

## One batch in flight, and a second dispatch is not refused

`For` inside `For` would deadlock a fixed pool. The pool admits one batch at a
time and **a call that finds it occupied runs its whole span inline on the
calling thread** rather than waiting or failing: `Pool::Claimed` is an
`exchange`, and losing it takes the inline path. That covers a nested `For` from
inside a body and two threads dispatching at once, and neither deadlocks.

Inline and pooled execution are observationally identical, which is what makes a
world tick safe to run as a range inside a larger batch: the world's own
parallel loops degrade to serial instead of corrupting the batch that dispatched
them. If nested parallelism ever has to be real, that is a work-stealing deque
and a rewrite, not a flag.

## `ForWorkers` is bounded by physical cores, not by the pool

`ForWorkers` places each task on a named pinned worker. Only the leading prefix
of the pool that `Jobs::Start` could bind to *distinct physical cores* is
usable, so `PinnedWorkerCount()` is a core count and not a thread count - on a
twenty-four logical, twelve physical machine the pool is 23 workers and the
pinned prefix is 12. A caller whose index must not share a core with its
neighbour has to fall back when that count is zero.

Two asymmetries against `For`, both deliberate and both easy to trip over:

- **The calling thread never takes a task.** `For` drains alongside the pool;
  `ForWorkers` does not, because a task's placement is the point.
- **`For` survives losing the pool and `ForWorkers` cannot.** Every range of a
  `For` can be claimed by the caller, so the join completes whatever the workers
  do. A `ForWorkers` task is bound to a worker index, so if that worker is not
  there the join never finishes. This is why `Jobs::Stop` must not race a
  dispatch, and the rule is a convention rather than a check.

`ForWorkers` also wakes the whole pool, not the named prefix, so the unnamed
workers pay a wake and two lock acquisitions to find no task of their own.

## Grain is the whole game, and the default is for cheap bodies

`DEFAULT_GRAIN` is 4096 because that is what measured best, not because it looks
round. The finding worth keeping: over an ECS integration step of three float
multiply-adds per row, a grain of 256 made the parallel version **twice as slow
as the serial one** at 20,000 rows. Waking a worker costs more than the 256 rows
it was handed.

Two consequences:

- **A default grain is a guess about how expensive one row is.** 4096 suits a
  body that does almost nothing. A body that raycasts wants a grain in the tens.
  Pass it explicitly whenever the work per row is real.
- **Parallel is not free, and below a crossover it is negative.** Measure the
  system you are writing, in `bench`, and put the number in a comment.

## The crossover is a duration, and every constant here states it in rows

Measured at `-O3` on 24 threads, and the two answers are thirty-two times apart:

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

**Both crossovers are owed a re-take and neither has had one.** They were
measured against a handover that then cost 31 us and now costs 8.7, so the
arithmetic says both should fall. That is a prediction, not a reading:
`engine.ecs.bench.iteration` and `engine.physics.bench.integrate` are where it
gets settled.

Ceilings, while the numbers are here: past the crossover the cheap body tops out
near 1.3x and the `CFrame` body near 5.6x. The cheap one is bandwidth bound
rather than thread bound - at 500k rows both paths stream twelve megabytes.

## What a handover costs, and where it goes

`engine.parallel.bench.dispatch` measures an empty `For` and nothing else. At
`-O3` on twenty-four logical processors, with a 23-worker pool except where a
row says otherwise:

| ranges | workers woken | |
|---|---|---|
| the decision not to dispatch | 0 | 50 ns |
| 8 | 1 | 793 ns |
| 8 | 7 | 8.71 us |
| 128 | 4 | 6.11 us |
| 128 | 23 | 27.37 us |
| 1024 | 23 | 66.51 us |

**It is linear in the workers a dispatch wakes and only weakly in the ranges.**
The first three rows cannot tell those apart, because `For` wakes `ranges - 1`
workers up to the pool size and every one of them moves both at once. The last
three separate them: at a fixed 128 ranges, dropping the pool from 23 workers to
4 takes 27.37 us to 6.11 us, and at a fixed 23 workers, taking the ranges from
128 to 1024 costs 43.7 ns each. So a dispatch is about **1.1 us a woken worker
and 44 ns a range**, and the per-worker term is twenty-five times the other.

## The join serialises on `Pool::Guard`, in the worker loop

**Until v0.19 this file said the serialisation was every worker decrementing
`Batch::Outstanding` under `Pool::Guard`. That has not been true since the join
rewrite and the number said so.** `Retire` decrements `Outstanding` with a
lock-free `fetch_sub` (`src/Jobs.cpp:113`) and takes `Pool::Guard` only on the
decrement that reaches zero, to notify without a lost wakeup.

What every woken worker does take, in turn, is `Pool::Guard` **twice per batch**:

- `src/Jobs.cpp:208`, the `unique_lock` at the top of `WorkerLoop`, to read
  `Generation` and `Current` and count itself into `Pool::Inside`.
- `src/Jobs.cpp:229`, the `lock_guard` at the foot of `WorkerLoop`, to count
  itself back out.

Cite the statement rather than the number when you quote this; the numbers moved
once already and that is the finding this section exists because of.

**The evidence that this is a serialisation and not a wake latency is that it
adds up.** Seven woken workers cost seven times what one costs and twenty-three
cost twenty-three times it, on distinct cores, where anything paid concurrently
would not. The one-worker row is *below* the per-worker fit for the same reason:
with a single worker the two acquisitions are uncontended.

Two places pay for it, and only one of them is the caller's own dispatch:

- **The final `Retire`'s notify** (`src/Jobs.cpp:113-118`) needs the same mutex,
  so it can queue behind workers already counting themselves out, which delays
  `For`'s join at `src/Jobs.cpp:418`.
- **The next dispatch.** `For`'s join waits only on `Outstanding`, so it returns
  while workers are still counting out. `Drained.wait` at `src/Jobs.cpp:381`
  collects that debt at the head of the *following* `For`. `ForWorkers` pays it
  inside its own join instead, at `src/Jobs.cpp:506`, because it has to clear
  `AssignedWorkers` before it returns.

That join is what to attack if the crossover ever has to come down, and doing so
is a rewrite of the join rather than a change to a constant.

## What a dispatch buys, when the ranges hold work

`engine.parallel.bench.contention` puts the same million rows of real work
through every arrangement, so the rows divide against each other. Twenty-four
logical and twelve physical processors, at `-O3`:

| | |
|---|---|
| serial, no dispatch at all | 3.49 ms |
| 1 worker | 2.25 ms |
| 2 workers | 1.49 ms |
| 4 workers | 1.21 ms |
| 23 workers | 242 us |
| 23 workers, grain 256 | 279 us |
| 23 workers, grain 65536 | 368 us |
| 23 workers, one row in a thousand a hundred times dearer | 1.13 ms |
| nested, so the inner dispatch runs inline | 315 us |
| the same million rows as 64 dispatches of 16k | 2.95 ms |
| `ForWorkers`, the same rows as 64 tasks over the pinned prefix | 536 us |

**The last two rows are the ones to remember.** Sixteen thousand rows is under
`DEFAULT_GRAIN * MINIMUM_GRAINS`, so all sixty-four calls ran whole on the
caller and the row lands on the serial figure - it did not dispatch at all. A
system that batches its work per chunk rather than per tick hands the pool spans
it silently refuses to split; the profile shows a busy calling thread and idle
workers, and nothing anywhere says the word "inline". And `ForWorkers` is twice
the cost of `For` on the same work because the pinned prefix is twelve threads
where the pool is twenty-three: placement is bought with parallelism.

The grain rows are what keeps 4096 where it is for a body of this cost, and the
imbalance row is the reminder that `For` splits by index count and cannot know
what an index costs.

## The calling thread works

`Start(0)` leaves one core for the caller, because the caller drains ranges too.
A pool sized to the full core count oversubscribes on every `For`.

`WorkersPerHost` is the same subtraction for the process case: several hosts on
one machine each calling `Start(0)` is a hundred and ninety threads over
twenty-four cores. It rounds down deliberately.

## The pool's lifetime, and why it is never destroyed

**One pool per process, and the program owns it.** `Jobs::Start` is a program's
call: `mono.client`, `mono.server`, `mono.studio` and each test or benchmark
suite that needs workers. A library must not start it, because the worker count
is a function of how many hosts share the machine and only the program knows
that. `Start` is idempotent, so a second call with a different count is ignored
rather than resizing the pool.

**Whoever started it should stop it, and nothing breaks when they do not.**
`Stop` joins every worker and leaves the pool empty; it is safe to repeat and
safe without a `Start`. A program that forgets it leaks twenty-three parked
threads until the process exits, which is what a process exit is for.

**It is never destroyed, and that is what makes forgetting harmless.** `Get()`
holds a leaked `Pool *`, the same shape `ecs::Components` and `ecs::ChunkPool`
use and for a problem of the same kind. Destroying the pool destroys four
condition variables with every worker still parked on `Available`, and
`pthread_cond_destroy` waits for the last waiter: before v0.19 a process that
called `Jobs::Start` and did not call `Jobs::Stop` returned zero from `main` and
then hung in `exit` for ever, after every test had already reported passing.
That reads as a hung suite rather than as a stuck teardown, which is what made
it expensive to find, and `mono.unified_tests/benchmarks/Crossing.cpp:85-94`
carries a `Jobs::Start(1)` whose only job was to shove the pool earlier in the
static destruction order to avoid it.

`tests/Jobs.cpp`'s "a program that never stops the pool still exits" spawns a
child that starts a pool and returns, and requires it to exit within fifteen
seconds - a regression fails the case rather than wedging the runner. Do not
give `Pool` a destructor to make a leak checker quiet.

## A `[.]` tag does not keep a case out of a suite run

`mono.tools/testrunner` runs a suite as `test_parallel -# "[#Jobs]"`, and **an
explicit Catch2 filter matches hidden cases**, so every `[.child]` case in a
file runs in-process beside the ordinary ones. Catch2 does not run them in
declaration order either, so a case that leaves process-wide state behind lands
on whichever case was scheduled next.

That is not hypothetical: the child case added for the section above started a
pool of two and did not stop it, and about one run in twenty the next case to be
scheduled asked for four workers, got the two still running because `Start` is
idempotent, and failed an assertion nowhere near the cause. It reproduces only
under the runner's own filter, which is why running the binary directly or with
`[jobs]` never shows it.

**So a case in this directory that changes process-wide state must restore it
however it exits, and a child case must first establish that it really is a
child.** `HasInheritedChannel` is this module's answer to the second, `Pool`,
`EmptyPool` and `ForcedSerial` in `tests/Jobs.cpp` are the answer to the first,
and none of it is checked by anything.

## `Start` and `Stop` are not thread-safe against anything, and nothing checks it

`Jobs::For`, `Jobs::ForWorkers`, `WorkerCount` and `PinnedWorkerCount` are safe
from any thread. `Start` and `Stop` are not, against each other or against a
dispatch, and this is **a convention rather than something the build enforces**:

- Two `Stop`s at once both join the same `std::thread`s, which is undefined.
  Nothing this module can hold fixes that, which is why the whole call is a
  convention and not a lock.
- `Stop` during a `ForWorkers` hangs the join, for the reason in the
  `ForWorkers` section.

`Stop`'s tail *is* under `Pool::Guard` as of v0.19 - it was the one place five
fields read under the guard everywhere else were written without it - but that
buys tidiness rather than safety, and the two rules above still hold.

The join loop itself must stay outside `Pool::Guard`: a worker needs that mutex
to observe `Stopping`, so joining while holding it deadlocks.

## Processes are for crash isolation, not for speed

Two processes simulating two worlds are not faster than two threads doing the
same; they are more survivable. A soft fault is caught at the world-tick
boundary and never needs a process. A hard fault takes the address space, and
the only honest isolation from one is a separate address space.

`Process` is **unsynchronised and single-owner by design** - it is move-only so
that two owners cannot both reap one child. Do not share one across threads and
do not add a mutex to make that possible; the fix for a supervisor that wants to
poll from elsewhere is to move the handle, not to lock it.

Nothing in this engine installs a `SIGCHLD` handler, and `Process` relies on
that. One that reaped children would make `waitpid` here return -1 and turn
every clean exit into `ExitReason::Gone` with nothing saying why. `RequestStop`
sends `SIGTERM` and relies on the other side handling it; `mono.server` does.

`Capture` is the opposite shape to `Process` on purpose: it runs a program *for*
its output and blocks until it ends. It forks rather than spawning because the
child has to close the read end it inherited, so everything between the fork and
the exec is a syscall and nothing allocates or takes a lock. It reads to EOF
*before* it waits; waiting first deadlocks the moment the child writes more than
a pipe buffer.

## A channel never blocks, and that is what makes it substitutable

Everything crossing a world boundary is bytes, so the only difference between
two worlds in one process and two worlds in two processes is what carries them.
A caller holding a `Channel` cannot tell which it has.

- **Never blocks.** A world tick occupies a job worker, so a send that waited
  for room would stall every other world in the host. A full channel refuses and
  says so.
- **Bounded, in bytes.** Frames vary by three orders of magnitude, so a frame
  count says nothing about memory.
- **Closed and drained, in that order.** A peer that exits cleanly must not
  strip this end of what it already said.

The socket implementation holds its mutex across `send` and `recv`. That is
allowed here and nowhere else in the module: both handles are non-blocking, so
neither call can wait on the peer, and pumping outside the lock would need a
second lock over the buffers. **Both directions are pumped on every entry point,
including the read-only ones** - a frame too big for one write otherwise sits in
`Outbound` until something pushes it, and the caller that would push it is the
one waiting for the answer.

## The platform layer is four socket calls, a spawn, a capture and affinity

`src/platform/Socket.hpp` names no operating system and neither does any public
header. The framing, the buffering and the backpressure are in `SocketChannel`,
shared, because those are the parts that would silently diverge if there were
one copy per platform. What actually differs is the handle type, the spelling of
"would block", and which function closes it.

`mono.build/MonoLibrary.cmake` compiles `src/platform/<os>/` only on that `<os>`
and treats `posix` as the family for everything except Windows, so an
implementation this module needs on a platform it already supports is a file in
the right directory and no build edit at all. A genuinely new operating system
also needs a `MONO_PLATFORM` branch in `MonoLibrary.cmake`, which is the only
place in the build that names one.

Two things not to do: **do not let an `#ifdef` for an operating system into
`src/*.cpp` or into `include/`**, and **do not define the same symbol in both
`platform/posix/` and `platform/<os>/`** - both would compile, and which one
wins is a link order.

## Settings are declared here, not in `core::Config`

`core` is below this module, so a settings layer that reached upwards to apply
everybody's values would be an inverted dependency. `Config` owns the sources
and the precedence; this module owns `engine.jobs.workers` and
`engine.serial-compute`.

`ConfiguredWorkers` is read at the call site rather than applied here, because
the automatic answer is `WorkersPerHost(hosts)` and only the program knows how
many hosts share the machine.

## Rules the build does not check

Root rule 6, listed rather than left in somebody's memory:

- `Start` and `Stop` are not thread-safe. Nothing enforces it.
- A `For` body may only write what its own range names. Nothing enforces it, and
  it is what makes inline and pooled execution identical.
- `ForWorkers` mappings must name workers below `PinnedWorkerCount()`. This one
  *is* checked, at runtime: a bad mapping falls back to running inline.
- A grain passed by a caller should carry the measurement it came from in a
  comment beside it. Nothing enforces it; `physics::INTEGRATE_GRAIN` is what it
  looks like when it is done.

## Gaps

`Capture.hpp` and `Settings.hpp` have no test suite. Root `AGENTS.md` is
explicit that this is a gap rather than a convention, and neither header has a
reason to be one of the three deliberate exceptions.
