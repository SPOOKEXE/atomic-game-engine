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

## A world never names another world

Worlds do not address each other. Everything crossing goes through a bus —
MessagingService, MemoryStore, DataStore, Teleport — so routing is hub-and-spoke
rather than a mesh, ordering is decided in one place, and there is no type in
this module that pairs a world with an entity.

`v02v03.md` §2.7 has the reasoning. The short version: the cross-world entity
reference is the type that would have broken "nothing crossing a world boundary
is a pointer", and it does not exist.

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
