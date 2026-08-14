# world — module invariants

L4. Universe and worlds, above storage and below everything that draws.

## The Universe hands out identifiers, never stores

`ecs/AGENTS.md` says this layer "hands out identifiers, never stores, and
`Store` is the type it is refusing to hand out". That is the rule this module
exists to enforce.

There is no `Universe::Get(WorldId) -> Store &`. Reaching a world's storage is
`Enter`, which runs a body against the store **on the driver thread, while
nothing is ticking**, and takes the reference away again when it returns. A
long-lived `Store &` is what makes thread-per-world and process-per-world
different things; a scoped one does not.

The day a world lives in another process, `Enter` on it is a request rather
than a call. Nothing above this line has to change for that, because nothing
above this line ever held the store.

## A world names another world only through the bus

Everything crossing goes through a bus — MessagingService, MemoryStore,
DataStore, Teleport, Channel — so routing is hub-and-spoke rather than a mesh,
ordering is decided in one place, and there is no type in this module that pairs
a world with an entity.

`v02v03.md` §2.7 has the reasoning. The short version: the cross-world entity
reference is the type that would have broken "nothing crossing a world boundary
is a pointer", and it does not exist. **A destination is a `core::Name`, and that
is the whole of what a world may know about another one** — Teleport and Channel
carry one, and neither can be turned into a handle by anybody holding it.

## A channel is `(world, channel)`, and both halves are enforced

`Postbox::SendTo` addresses a **named channel on a named world**, and the
receiving world opens the channel first. Three rules a reviewer should hold to:

- **The channel is what keeps two subsystems apart.** v0.15's cut was one
  unnamed pipe per pair of worlds, so a match controller and a chat relay talking
  between the same two worlds each heard the other's traffic and had to tell it
  apart by inspecting the payload. A channel nobody opened is `NoSuchChannel`
  rather than a delivery, which is the same distinction the bus can enforce.
- **Every failure is a `BusStatus` and none is a silent drop.** `SendTo` always
  asks for a reply, so there is always a carrier: `NoSuchWorld`, `NoSuchChannel`,
  `WorldNotReady`, `Overflow`, or no ticket at all when the world is over budget.
  `Bus.hpp` carries the table. Delivery is at-most-once and nothing retries —
  only the sender knows whether re-sending is correct.
- **The queue is bounded and the bound is observable.**
  `UniverseSettings::ChannelQueueLimit` caps how many channel deliveries one
  world may have queued for it in one barrier, and the senders past it are told
  `Overflow`. An unbounded queue between two worlds is a memory leak with extra
  steps; a silently bounded one is a game that works until the day it is busy.

## Barrier order is the sender's *name*, never its `Name::Id()`

The barrier applies traffic in `(From.Text(), Sequence)` order, and both halves
are recorded in the envelope. Neither depends on scheduler timing, on which
worker claimed a world, or on the order the registry happened to be walked in.

**`Name::Id()` is not a substitute and the router used it until v0.17.** An id is
handed out in interning order, so it depends on which world was named first *in
this process*: a universe restored from a snapshot interns in file order where
the run that wrote it interned in creation order, and the same two envelopes then
apply in the opposite order. `BusRouter::SortedKeys` had already been given this
exact argument for the snapshot codec — the barrier's sort simply had not been.

The cost is a string compare where there was an integer one, over a range that is
already nearly sorted because each world's outbox is ordered by construction.

## Worlds are the batch

Ticking the local worlds is one `Jobs::For` over them, not a thread per world.
So:

- **A world tick must not block.** It occupies a pool worker; blocking one
  stalls every other world in the host. Bus calls return a `Ticket` and the
  reply lands next tick. There is no synchronous path from inside a tick.
- **A world is bound to a different thread most ticks**, because whichever
  worker claims it runs it. That is why `Store::Owner` is atomic.
- **`EachParallel` inside a world tick runs inline**, because the pool is
  already claimed. That changes timing and not results — see
  `parallel/AGENTS.md`.

## Two kinds of failure, and they are not the same

| | Caught | Blast radius |
|---|---|---|
| **Soft** — a system throws, a script errors, a budget overruns | at the world-tick boundary | one world, marked `Faulted` |
| **Hard** — `abort()`, segfault, OOM | not caught | the whole host |

Do not blur them. Catching `SIGSEGV` and continuing means continuing with a heap
that may be corrupt, which makes the *neighbours* suspect too. A world that
cannot tolerate a neighbour's hard fault declares `Isolation::Dedicated` and
gets a host of its own.

## The barrier is where everything happens

Only the world ticks run in parallel. Creating a world, destroying one,
suspending one, applying bus traffic, firing deferred signals — all of it
happens on the driver thread with nothing else running. That is one place to
reason about, and it is why the control queue exists rather than
`CreateWorld` mutating the world list from wherever it was called.

## Determinism is same-binary

Two runs of the same executable on the same machine reach the same state.
Cross-machine agreement is **not** promised and must not be claimed: floating
point differs between compilers and chips.

What that buys: exact crash recovery, a working parallel-equals-serial test, and
reproducible bugs. What it costs: no simulation system may read wall-clock time,
a thread id, a pointer address, or iterate an unordered container.
