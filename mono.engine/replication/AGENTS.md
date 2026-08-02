# replication — module invariants

L12, `shared` tier. What the server tells a client, and what the client does
with it. `net` carries the bytes; this decides what they mean.

## The server is authoritative and the client is a replica

Not a peer, not a cache, not a mirror that may disagree politely. The server's
store is the world; a client's is a copy that is always some amount behind and
sometimes wrong, and correcting it is the normal case rather than an error path.

**A replica may not write to a bus.** `world::Replica` is the flag and
`world::Postbox` refuses at the call. This module must never route around that:
an operation a client wants performed goes up as an input and comes back as
state, which is the only shape in which the server stays the one that decided.

## Every field of an inbound message is hostile, in both directions

`net/AGENTS.md` says there is no trusted direction and that applies here with
more force, because this layer names entities and components. A client sending
a delta, an entity index outside the directory, a component ordinal nothing
registered, or a chunk offset past the total is an attack or a bug and both are
refused the same way.

**A snapshot is applied into a scratch store first.** `ecs::Store::Apply`
already does this and it is not an optimisation to remove: a corrupt snapshot
that half-merged would leave a client with a world that is neither the old one
nor the new one, and nothing downstream could tell.

## Names cross, ids do not

A `ComponentId` is a dense counter assigned in registration order and means
something different in the other process. Every component on the wire is named,
and a message resolves names to ids once rather than per entity — the same rule
`ecs`'s snapshot follows, for the same reason.

An `ecs::Entity` **is** carried as its index and generation, and that is
deliberate rather than an exception: the whole point of reproducing the
directory exactly is that a handle means the same thing on both sides. See
`ecs/docs/TODO.md` on the one case that does not hold yet — a replica cannot
mint its own entities, because both stores allocate from index zero.

## Deltas come from the dirty bits, not from a diff

`ecs::ChangeChannel` already records what moved, for `.Changed` and for render
invalidation; a delta is the third reader of the same bits. Do not add a second
record of what changed, and do not diff two snapshots — a diff costs the size of
the world every tick whether or not anything happened.

`EachChangedBatch` yields *runs* rather than rows precisely so that a delta is a
memcpy per run. A per-entity copy here would undo that.

## Time is passed in, never read

The same rule as `net`, for the same two reasons: a wall clock read inside is a
non-deterministic input in the subsystem whose failures are hardest to
reproduce, and it makes a timeout something a suite states rather than waits
for.

## A tick number is the unit of agreement

Not a timestamp, not a sequence number. The server stamps what it sends with the
tick it describes; a client acknowledges the last tick it applied; prediction
replays the inputs after that tick. A design that agreed on wall time instead
would need the two clocks to agree, and they do not.

## Prediction is the local player and nothing else

Everything else is interpolated authoritative state. Predicting a second entity
means predicting what another player will do, which is wrong more often than it
is right and is visible as rubber-banding when it is wrong.

**Reconciliation needs no cross-machine determinism.** The client drifting is
expected; correcting the drift is the mechanism, not a fallback. Nothing here
may assume a client and server compute the same floats.

## A message has to fit a datagram, and the whole world does not

The snapshot is chunked and spread across ticks. **So is a delta**, and for the
same reason with a sharper edge: a delta too large for a payload is not slow, it
is refused by `Link::Reserve` and silently never sent, because a refusal is
ordinary backpressure and a message that can never fit looks exactly like one. A
world of thirty-two entities already built one.

`EmitDelta` splits a tick's delta into however many messages it takes, each
under `ChunkBytes` and **each independently applicable** — not a numbered
sequence to reassemble, because this is the unreliable channel and waiting for a
part that was dropped is a stall on a path whose premise is that the next tick
is already on its way. That is why `Replica` treats a delta at the tick it has
already applied as another part rather than as stale.

## Not here yet

- **Lag compensation** — rewinding the server to what a client saw when it
  fired. Its own plan, and it needs the history buffer this does not keep.
- **Priority under a bandwidth cap** — interest says what a client may see;
  nothing yet decides what to drop when what it may see does not fit. **There is
  already a policy here and nobody chose it**: a tick's delta goes out as several
  messages and the link refuses the ones past its budget, so what gets dropped
  is whatever happened to be last in the component list. Deterministic, which is
  something, and arbitrary, which is the problem — one component starving
  forever because of where it sits in a vector is the failure mode. See
  `docs/DEFERRED.md`.
- **Authenticating a client** — `Listener` admits on first contact.
  `net::Handshake` is built and not wired in. `docs/DEFERRED.md`.
