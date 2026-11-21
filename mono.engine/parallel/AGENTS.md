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

`ForWorkers` signals only the distinct workers named by its mapping, and
generic `For` signals a bounded prefix. A worker is counted into `Pool::Inside`
by the dispatcher before it is signaled, so only a worker the batch selected can
read `Current`. Shutdown signals every worker before joining.

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

**The cheap body's crossover has been re-taken against the lock-free
handshake.** `engine.ecs.bench.iteration` now has `EachParallel` ahead of `Each`
from 64k rows at the default grain (7.3 us against 14.4 us) and at 100k (11.8 us
against 22 us). The `CFrame` body, re-taken in `engine.physics.bench.integrate`,
pays from 4,000 rows, and `physics::INTEGRATE_GRAIN` is 384 for it.

Ceilings, while the numbers are here: past the crossover the cheap body tops out
near 1.3x and the `CFrame` body near 5.6x. The cheap one is bandwidth bound
rather than thread bound - at 500k rows both paths stream twelve megabytes.

## What a handover costs, and where it goes

`engine.parallel.bench.dispatch` measures an empty `For` and nothing else. At
`-O3` on twenty-four logical processors, with a 23-worker pool except where a
row says otherwise, medians of three interleaved runs on a machine that other
builds were loading:

| ranges | workers woken | |
|---|---|---|
| the decision not to dispatch | 0 | 50 ns |
| 8 | 7 | 1.1-1.3 us |
| 8, one-worker pool | 1 | 0.5-0.7 us |
| 128, four-worker pool | 4 | 3.9-6.1 us |
| 128 | 23 | 15-34 us |
| 1024 | 23 | 36-215 us |

The handshake takes no lock. Each worker owns a `WorkerSlot` holding its wake
word, the dispatcher stores the batch generation into the slots of exactly the
workers it wants, and the join waits on `Batch::Outstanding` and `Pool::Inside`
as atomics. `Inside` is set by the dispatcher to the number it woke, before the
first wake, so the next dispatch cannot rewrite the slot while a woken worker
still reads it and no worker has to count itself in under a mutex.

Two things decide the cost, and both are in `src/Jobs.cpp`:

- **The pinned prefix spins for 30 us before sleeping.** A frame dispatches
  back to back, and a spinning worker takes the next batch from a store. Only
  the workers bound to distinct physical cores spin; a whole pool spinning
  across hyperthread siblings burned its scheduler share on a loaded machine
  and was then preempted holding a range, which measured several times slower
  on the wide rows.
- **Sleepers are woken one band of eight at a time.** A futex call per worker
  on the dispatching thread was several times the old handover on a full pool.
  A dispatcher rings a band only when a worker it named in that band is
  asleep, and the band's unnamed sleepers go straight back to sleep.

The wide rows are the noisy ones because they wait on the slowest of
twenty-three threads, so one preemption anywhere decides the sample. Compare
them by interleaving the two builds, not against a stored figure.

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
use and for a problem of the same kind. Destroying the pool destroys
condition variables with every worker still parked on them, and
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

`Stop` sets `Stopping`, moves every worker's wake word and rings every band, so
a sleeping or spinning worker returns. It joins outside `Pool::Guard`, which
`Start` and the count queries still take.

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
