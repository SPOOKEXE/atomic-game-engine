# Script concurrency — the userland surface over worlds and buses

`ROADMAP.md`'s second v0.5 line: *"plans for luau and typescript
multi-threading and multi-processing systems (with locks, synchronise, etc,
also plan integration with hytale setup for worlds)."*

**This document builds nothing.** Every piece of machinery it plans over exists
and has since v0.2 — what is missing is the userland surface, and the rules that
keep it deterministic once a script can reach it.

## What already exists

The "hytale setup" is the Universe/worlds arrangement: many worlds, processed in
parallel, with libraries to cross-message between them. All of it is built.

| Piece | Where |
|---|---|
| Many worlds under one universe | `world::Universe`, `world::World`, `WorldId` |
| Parallel processing of them | `ExecutionMode::WorldParallel` / `WorldSerial`, per host |
| The cross-message library | `world::Postbox` — `Publish`/`Subscribe`, `Get`/`Set`/`Update`/`Remove`, `Push`/`Pop`, `Teleport`, `Deliveries` |
| Four buses behind it | `BusKind::Messaging`, `MemoryStore`, `DataStore`, `Teleport` |
| Correlation | `world::Ticket`, per world and monotonic |
| Process isolation | `world::Supervisor`, `world::HostLink`, `parallel::Process` |
| The barrier | `world::Driver` |

**Nothing below proposes a second one of any of these.** A userland threading
primitive that did not resolve to this list would be a second concurrency model
inside one engine, and the two would disagree about ordering the first time
anything went wrong.

---

## 1. The rule everything else follows

> **A script may only resume from something the barrier delivers in a
> deterministic order.**

Rule 5 says work inside a tick may be parallel and work across ticks may not,
because *a result that lands a tick later on a slower machine is a desync*. A
coroutine that yields and resumes next tick is, by definition, work across
ticks — so the naive reading is that scripts may not yield at all.

That reading is wrong, and the reason is the shape v0.2 already chose. Every
bus call returns a `Ticket` and its reply lands in the inbox at a later tick,
applied **sorted, at the barrier**. `docs/retired/v02v03v04.md` §2.7 argued for
that on ergonomic grounds — *"it is the same contract as `:GetAsync()`, so the
semantics a Luau author already expects are the semantics the engine wants
anyway"* — and the consequence is that the *reply* is already deterministic.
A script resumed by one is therefore resumed deterministically.

So the rule is narrower than "no yielding" and stricter than "yield freely". A
resume is legal when the thing resuming it is:

- a `Ticket` reply the barrier applied,
- a `Deliveries()` entry the barrier applied,
- a tick boundary.

And illegal when it is a wall clock, an OS event, a real timer, a completion on
another thread, or anything else that could land at a different point on a
different machine.

**This is checkable rather than aspirational.** `just determinism` runs one
scene twice and diffs the bytes; `just replay-check` replays a recording and
diffs again. A yield source that violates the rule fails both — eventually, and
somewhere far from the cause, which is why the rule is written down here rather
than discovered later.

## 2. `wait`, and the one decision an author meets first

`wait(n)` is the first thing any Luau author writes, and **seconds are exactly
the desync rule 5 names**: a script sleeping on a wall clock resumes after a
different amount of simulation on a busy machine than on an idle one.

Ticks are the unit that survives. What is open is the spelling, and it is a real
trade rather than a formality:

- **Keep `wait(n)` and make `n` ticks.** Familiar, and quietly different from
  every other engine an author has used. The failure is silent: a script that
  "waits one second" waits sixty ticks and nobody notices until the tick rate
  changes.
- **Name it something else** — `WaitTicks(n)`, `task.wait(n)` with a stated
  unit. Honest, unfamiliar, and an author has to read one line of documentation
  once.

**Recommendation: name it differently, and make `wait` an error that says what
to use instead.** A refusal that names its replacement costs an author one
lookup; a familiar name with different semantics costs a debugging session, and
costs it later.

Neither spelling changes the mechanism: a wait is a resume at a tick boundary,
which is the third legal source in §1.

## 3. What a script sends — the codec

Bus payloads are bytes. Rule 3: nothing crossing a world boundary is a pointer,
and v0.2's shape is *"trivially copyable structs written through
`core::ByteWriter`, or nothing at all for a bare signal."* Roblox's
`MessagingService` takes a table.

So the shim needs a **script-value ↔ bytes codec**, and it is the one piece of
this surface with no prior art anywhere in the tree.

Three requirements, in priority order:

1. **Deterministic.** The bytes go into a recording that has to replay
   identically. **Table iteration order is the trap**: a codec that walks a hash
   map in memory order serialises differently on two runs of one script, and
   `just determinism` fails somewhere far from the codec. Keys are sorted, and
   the sort is part of the format rather than an implementation detail.
2. **Identical across both VMs.** A world scripted in Luau and one scripted in
   JavaScript must produce the same bytes for equivalent values, or the two
   disagree on the wire while each looks internally consistent. **That is one
   shared test, not two per-VM ones.**
3. **Bounded.** A payload has a maximum size and a maximum depth, both refused
   rather than truncated. A cyclic table is an error, not a hang.

What crosses: booleans, numbers, strings, the three value types, and
arrays/tables of those. What does not: functions, instances (an `Entity` is
*"meaningless outside this world"* — a reference must cross as whatever the game
uses to name things, not as a handle), and anything holding a pointer.

## 4. Locks, and why there is no mutex

The roadmap line asks for locks by name. The answer is that a lock in the shape
an author expects cannot exist here, and the thing they actually want already
does.

- **Between worlds** — rule 3 says nothing crossing a world boundary is a
  pointer, so there is no shared memory to guard. What a cross-world lock would
  have been built out of is a compare-and-swap, and that is
  **`MemoryStore::Update`**: it takes the version the caller read, and fails
  `BusStatus::Conflict` when the version has moved on, so the caller re-reads and
  retries. Optimistic rather than blocking, which is also the only shape that
  works when the other party might be in another process.
- **Within a world** — a world's scripts are already serialised by its own tick.
  A mutex there protects nothing and can only deadlock.

**Say both plainly in the userland documentation**, because the failure mode of
not saying them is somebody building a cross-world lock out of shared state and
ending the process-per-world option permanently — which rule 3 warns is the
failure that is invisible afterwards.

`synchronise` is the barrier, and `world::Driver` already runs it. Userland does
not get a second one.

## 5. Budgets and refusals are part of the contract

Three ways a call fails that a script author must be able to see and handle:

- **`OverBudget`.** Each bus gives a world a request allowance per tick.
  v0.2's reasoning is on the record: *"Roblox has these because they turned out
  to be necessary; there is no reason to rediscover that."* An over-budget call
  fails with a named error rather than starving a neighbour or silently
  dropping.
- **A replica refuses.** `Postbox::IsReplica()` exists and a replica's bus
  handles refuse writes. A client-side script calling `Set` or `Teleport` must
  fail visibly — the same story `Store::SetProperty` already tells, and it
  should be one story rather than two. A script author cannot tell "rejected"
  from "applied and overwritten by the next delta" from inside the script.
- **`NoSuchWorld`, `Unsupported`.** Named, not swallowed.

The engine-side halves of all three exist. What is owed is that each surfaces as
something a script can catch, with a message that says which it was.

## 6. Threads, and what `parallel/threads/` is for

`parallel/AGENTS.md` tables four sub-areas and says only `jobs/` exists:

| Sub | Holds | Status |
|---|---|---|
| `jobs/` | engine-internal dispatch | `Jobs.hpp` |
| `threads/` | the userland `thread` datatype | not built |
| `process/` | separate OS processes, supervision | `Process.hpp`, flat |
| `ipc/` | what crosses a process boundary | `ProcessChannel.hpp`, flat |

**The paths are aspirational and the plan should stop citing them as though they
were current.** What exists is `Channel.hpp`, `Jobs.hpp`, `Process.hpp` and
`ProcessChannel.hpp` in one flat directory. Creating `threads/` is part of
building the userland thread, not a prerequisite somebody else did.

The contract is already stated and must not be merged into `Jobs`: *"`Jobs` is
fork-join with no handles, and the userland one has to survive a script
yielding."* A userland thread is a coroutine plus a resume rule — §1's rule —
and not an OS thread. Nothing about it should reach `Jobs`, which blocks by
design and would have to answer "what happens when the world it was launched
from is destroyed mid-flight" to do otherwise.

## 7. `Await`

`world::Bus.hpp` has `Ticket`. **There is no `Await` anywhere in the tree**, and
`ROADMAP.md`'s v0.5 line cites one.

Either define it over `Ticket` — a suspend whose resume source is §1's first
legal case — or stop citing it. Citing an API that does not exist is the
`CDN.md` problem this repository already carries at the top of `ROADMAP.md`.

---

## What this implies for v0.6

The order matters, because two of these are decisions and the rest are work:

1. **Decide `wait`'s spelling** (§2) before any script uses it. Renaming a
   userland call after game files exist is a migration.
2. **Design the codec with the VM pair in mind** (§3), because "identical bytes
   from both VMs" is a property of the format rather than something either
   binding can add afterwards.
3. Build the suspend/resume machinery over `Ticket` (§1, §7).
4. Surface the refusals (§5).
5. Create `parallel/threads/` and the userland thread on top of all of it (§6).

**Nothing here needs a new bus, a new execution mode, or a second barrier.** If
a design starts to want one, that is the signal to re-read §1 rather than to add
it.
