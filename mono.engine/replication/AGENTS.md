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

## Not here yet

- **Lag compensation** — rewinding the server to what a client saw when it
  fired. Its own plan, and it needs the history buffer this does not keep.
- **Delta compression against an acknowledged baseline** — today a delta carries
  whatever moved since the last tick, not since the last tick the client
  confirmed. The second is smaller and needs per-client history.
- **Priority under a bandwidth cap** — interest says what a client may see;
  nothing yet decides what to drop when what it may see does not fit.
