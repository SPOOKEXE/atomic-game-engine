# mono.server — module invariants

The server program: a `server`-tier library and a thin main over it.

## This binary contains no renderer

Not "does not start one". Does not contain one. `mono.server` links no
`client`-tier target, so `render`, `text`, `ui`, `input`, `audio` and `vfx` are
absent from the link line.

Three things follow, and all three are checkable rather than aspirational:

- The `server` preset configures with no graphics stack at all.
- The staged `server/` directory has no `shaders/` folder. A `shaders/` folder
  appearing there is a link-line mistake anybody can see, with no tooling and
  no graph query.
- `mono.tools/architecture/expected_graph.json` records what this program
  links, and the architecture test compares the real graph against it.

If you need something that only exists on the client side, the answer is not to
link it. It is either that the thing belongs in a `shared` module, or that the
server does not actually need it.

## Do not include a client header

`mono.client/include/client/` is invisible here by construction. When something
genuinely has to be shared between the two programs — components, most likely —
it becomes a `shared` engine module, not an include across two programs.

**That has happened, and the sharing is `mono.engine/scene` at L7.**
`server/Simulation.hpp` used to declare a `Position`, a `Velocity` and a
`WorldBounds` of its own, and the client declared a matching pair to receive
them over the wire. All of them are gone: this program links `Engine::scene`,
registers `scene`'s components under `scene`'s names, and a client registering
the same names applies a snapshot from here with no translation layer at all.

So **a component declared in this directory that means something a `scene`
component already means is the change to refuse.** `Chatter` and `Heard` are the
two that remain, and neither is a duplicate of anything — they exist to put
traffic on a bus that would otherwise carry none, and they go when a game file
brings traffic of its own.

## The tick is fixed, and the delta is not measured

`Server::Run` feeds a constant delta derived from the tick rate, never the
elapsed time. A tick is a function of its state and its inbox. Feed it real
elapsed time and a recorded run stops replaying, every physics result becomes
machine-dependent, and the divergence shows up somewhere far from the cause.

If a tick overruns, the loop counts it and carries on. It does not simulate
extra ticks to catch up — a server that tries to make up a lost second by
running thirty ticks back to back falls further behind, and that spiral is much
harder to diagnose than a dropped tick.

## The world counts its own ticks

`Server` keeps no tick counter. `RunSummary::Ticks`, the `--max-ticks` check and
the log line all read `Store.Time().Tick`. A second tally on the host is a fact
that can disagree with itself the first time one of the two is advanced inside a
branch, and the disagreement surfaces as a summary nobody trusts.

Same reason `scene::WorldBounds` is a resource rather than a component. It was
the same four bytes on every entity — a property of the world stored 4096 times,
which the bounce loop then loaded per row. It lives in `scene` now, because the
client's `SceneBounds` was the same idea under a second name.

## Pacing is against an absolute schedule

`nextTickAt += budget`, not `sleep(budget - spent)`. The second form
accumulates the sleep's own overshoot, so the server drifts slower than its
stated rate and nothing in the numbers says why.

## The signal handler does one atomic store

`Stop()` sets an atomic that the loop reads between ticks. Do not grow the
handler. Logging, allocating or touching the world from a signal handler is
undefined behaviour that works right up until it does not.

## Not here yet

`orchestration` at L12 — sessions, matchmaking, sharding, drain — is a `server`
module and does not exist. Neither do `ledger`, `net`, `persistence` or
`gamefile`. When they arrive they are engine modules that this program links,
not code that grows inside `mono.server/`.

This directory holds the program's own attachments: the main, the tick loop,
world placement and the drain path. Anything reusable belongs under
`mono.engine/`.

## The rewind history records what can move, not what is moving

`ServeClients` walks `Transform` and `RigidBody` to fill
`replication::Rewind`, and the predicate is load-bearing. It walked `Motion`
until v0.15, on the reading that a `Motion` is what makes a placement worth
remembering — and `physics` *takes a row's `Motion` away* when it puts the body
to sleep, so that the solver's query never visits a resting row.

The history therefore held whatever happened to be awake. A player standing
still is asleep within a second, which meant they could not be shot, and it
presented as an ordinary miss: a hit test against an empty candidate list
strikes nothing and reports nothing.

`RigidBody` is the question actually being asked — an anchored part never gets
one, so the static geometry the old predicate was aiming at is still excluded —
and `Static` is skipped one layer in, because it is a body that does not move.

## A client's input tick is a claim, and a stale one means the world went quiet

A client stamps its input with the newest tick it has *applied*, and a tick
reaches it only when something changed. In a still scene its idea of the
server's clock stops advancing while the server's does not, so `Rewind::
TickSeenBy` can name a tick that has fallen out of the ring — and `Rewind::Each`
answers nothing, which is a miss with no error anywhere.

`ApplyInputs` resolves an out-of-window tick **at the present** rather than at
the oldest frame held. That follows from why it goes stale: a world that has not
been changing looks the same now as it did then. It also cannot be gamed —
rewinding is the favourable answer for a laggy shooter, so claiming a tick this
server no longer remembers buys the least favourable resolution there is, not
the most. A tick inside the window is honoured exactly as before, and
`RewindSettings::HistoryTicks` remains the fairness bound.
