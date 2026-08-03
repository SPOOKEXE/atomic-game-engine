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
directory exactly is that a handle means the same thing on both sides.

**A replica may mint entities of its own, from the predicted range only.**
`ecs::SparseSet` splits the index space at 2³¹: an authority allocates below it
and `Store::CreatePredicted` allocates above it, so a predicted entity has an
identity the server can never issue. `Store::SetAdoptOnly` still refuses an
*authoritative* mint here, because that index is the server's to hand out.

Two rules for this module, and neither is checked by the build:

- **Nothing predicted goes on the wire.** A predicted index means nothing in the
  other process and the authority never allocates one, so sending it is sending
  a handle whose only possible reading is wrong. An input goes up; state comes
  back.
- **`Store::Promote` is where a prediction becomes a server entity, and this
  module owns *when*.** `ecs` deliberately built the operation and no policy —
  see `ecs/AGENTS.md`. The decision needs a consumer that predicts a spawn, and
  there is none until there is a projectile, so nothing here should invent one
  in the meantime.

`Store::Promote` does not rewrite an `ecs::Entity` held inside an arbitrary
component; such a handle reads as dead afterwards rather than as some other
entity. If a message or a component in this module ever stores one, rewriting it
is this module's job.

## Admission is ordered cheapest first, and nothing is allocated until the end

`Listener` runs `Admission.hpp`'s exchange before a peer exists to it. The order
of the checks *is* the protection, so it is written down rather than left to be
read off the function:

1. A datagram from an unknown address on any channel but
   `net::ChannelKind::Handshake` is dropped.
2. A `Hello` is answered with a cookie `net::Cookie` derives. **Zero bytes**, and
   zero however many are outstanding.
3. An `Answer` is checked against the cookie, then `MaximumClients`, then the
   game's policy, then the X25519 agreement — and only then is a slot taken.

Moving any of those later is the bug this ordering exists to prevent. In
particular the agreement must stay behind the cookie: it is the only step that
costs real arithmetic, and in front of the cookie it is arithmetic any stranger
can make this process do.

**The bound stays.** `MaximumClients` and `Statistics::Turned` are defence the
handshake sits in front of, not defence it replaces.

**An entity with no replicated component is not visible to anybody.** Interest
filters entities and `Replicate` filters components, and the entity that passed
the first and had nothing left after the second used to cross as a bare row in
the join snapshot — no data, and a count of a world the client was told it may
not see. `Authority::Survey` is where that is decided, once per `Publish`
because the answer does not depend on the client.

**Who is allowed to connect is not this module's to invent.**
`Listener::SetAdmission` is the seam and the default admits anybody who
completes the exchange, which the header says in those words. A handshake proves
the peer can receive where it says it can and can do arithmetic; it does not
prove the peer is welcome, and `net::Handshake`'s own header is explicit that an
unauthenticated agreement is safe against a listener and not against a relay.

## Everything after the handshake is sealed, and the budget shrank for it

`Session` holds the two ciphers the exchange produced for the life of the
connection and seals every payload with the packet header as associated data.
The rules are `net/AGENTS.md`'s; what belongs here is what it costs this module.

**The tag comes out of the payload, so every budget is sized against
`net::Packet::MAXIMUM_MESSAGE_BYTES`.** `AuthoritySettings::ChunkBytes` is
capped at construction, loudly, because a chunk that cannot fit is refused by
`Link::Reserve` and a refusal is also what ordinary backpressure looks like —
which is the same shape as the v0.3 delta, the snapshot-chunk cursor and the
oversized forget, three times over.

**A session with no keys carries nothing in either direction.** `Listener` and
`Connector` each adopt them in the same breath as `CompleteHandshake`, and
nothing else may. There is no plaintext path to fall back to and no field on the
wire a peer could use to ask for one.

**The exchange's own confirmation is the first frame of the stream, not a
separate one.** The server's `Sealer` seals the `Welcome` tag at counter zero
and carries on from one, so the admission and the traffic share a single nonce
sequence rather than two that could overlap.

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

## The two halves pull opposite ways, and `SnapshotBuffer` is the other one

Prediction runs the local player *ahead* so input feels immediate.
`SnapshotBuffer` draws everything else *behind* — at a fixed delay from the
newest received tick, interpolating between the two that bracket it — so that a
world arriving at 30 Hz does not judder at 30 Hz on a screen running at 240.

**They must never be applied to the same entity.** Delaying the predicted row by
the jitter budget puts back exactly the lag prediction exists to remove, and the
player feels it as their own character lagging their own keys. The exclusion is
structural rather than a caller's discipline: `SnapshotBuffer::Record` refuses
the nominated entity *and* anything in `Store::CreatePredicted`'s index range,
before it records anything at all — so a tick offered nothing but predicted rows
is a tick the buffer never saw.

**The delay is the one number that matters**, it is `InterpolationSettings::DelayTicks`,
and the reasoning for both ends of its range is written at the constant rather
than here.

**It buffers per-entity poses, not snapshots of the world.** A tick's worth of
world is what `Replica` already applied; what interpolation needs is where each
entity was at each of the last few ticks, which is one `CFrame` per entity per
buffered tick and nothing else.

**Nothing it produces may reach a component.** The interpolated pose is returned
by value to whoever is filling a draw list. A render-rate quantity written back
into the store would make a simulation depend on the frame rate of whoever
happened to be watching it — the rule `world`'s `ViewChannel` already follows.

**Reconciliation needs no cross-machine determinism.** The client drifting is
expected; correcting the drift is the mechanism, not a fallback. Nothing here
may assume a client and server compute the same floats.

## A message has to fit a datagram, and the whole world does not

The snapshot is chunked and spread across ticks. **So is a delta**, and for the
same reason with a sharper edge: a delta too large for a payload is not slow, it
is refused by `Link::Reserve` and silently never sent, because a refusal is
ordinary backpressure and a message that can never fit looks exactly like one. A
world of thirty-two entities already built one.

`Authority::Pack` splits a tick's delta into however many messages it takes,
each under `ChunkBytes` and **each independently applicable** — not a numbered
sequence to reassemble, because this is the unreliable channel and waiting for a
part that was dropped is a stall on a path whose premise is that the next tick
is already on its way. That is why `Replica` treats a delta at the tick it has
already applied as another part rather than as stale.

**A `Structure` message is split too**, and the forget was the third path to be
missed. A world leaving view all at once names every entity in one message, and
three hundred handles is already past a datagram — which `Link::Reserve` refuses
outright rather than fragmenting, so the message saying "stop drawing these"
would be the one that never arrives.

## A refusal is backpressure for some messages and a hole for the rest

**Send `Authority::Outgoing` in order and hand back what the transport would not
take, through `Authority::Unsent`.** This is the shape of bug the chunking fix
did not cover, and it cost a suite that failed one run in three.

A delta needs nothing undone: the unconfirmed set rebuilds it next tick. A
snapshot chunk does, and this is the asymmetry — the cursor moves when the chunk
is *built*, so a refused chunk is a gap in a stream the receiver waits on for
ever. The observed failure was a client that applied 184 of 192 chunks, never
reached the last byte, never joined, and then refused every delta that followed
as stale: `applied=184 refused=17865`, which reads like a protocol error and was
a cursor. A `Structure` message is the same shape — a creation, a destruction and
a forget are each said exactly once and each moved the known set when it was
built.

**`Unsent` covers a refusal and not a loss.** A message the transport accepted
and the network then dropped is a different problem and has a different answer;
see the next section.

The buffer is therefore released **one tick after** the cursor reaches the end,
because `Unsent` is called after `Publish` has returned.

## Structure goes on the reliable channel, and values do not

`Delta` carries what moved. `Structure` carries which entities the client holds
— created, destroyed, forgotten — and `Session::ChannelFor` puts it on the
reliable channel while the delta stays unreliable. That split is the whole of
D00011's answer and it is not an exception to `net/AGENTS.md`'s "unreliable by
default": a value is superseded by the next tick and a structural change never
is, so they are the two sides that rule already draws.

**A tick acknowledgement cannot repair a lost structural change, and this is the
reasoning worth keeping.** `Applied` names a tick, not a message. The server's
known set moves when a creation is *said*, so a lost one leaves the server
certain the client holds an entity it has never heard of — and the client goes
on acknowledging, is not behind, and is never re-snapshotted for it. Nothing
that counts ticks can see that; the only thing in the tree that counts messages
is `net::ReliableSender`, so that is what redelivers it. Adding a second
acknowledgement channel for structure beside a working one is the second way to
do one job that `docs/CODE_QUALITY.md` asks about.

**A structural message is deliberately not tick-gated on arrival, and is
deliberately not an applied tick.** It is resent a hundred milliseconds later,
which is six ticks of a world that has moved on, so judging it by its tick the
way a delta is rightly judged is how a destroy never happens. And it carries no
values, so treating it as a tick applied would confirm every value of that tick
without one of them having arrived.

**`Applied` means the last tick applied *in full*, and `Replica` now enforces
it.** A delta naming a row this client does not hold yet — which is exactly what
a creation still in flight looks like — is applied as far as it can be and does
not move `Applied`. Without that, a creation arrived reliably some ticks late
and the entity held none of its components, because the tick its values were in
had already been acknowledged.

## An entity coming into view has not moved

A delta is built from the dirty bits, and an entity entering a client's interest
did not change — it was always there and that client could not see it. So the
run walk finds nothing for it and it used to arrive as a bare row with none of
its components, for as long as it stood still.

`Authority` seeds an unconfirmed entry for every entity it has just told a
client about, which puts it through the recovery walk that already reads current
values and already keeps offering until the client confirms. A second path that
did the same job is the second one that would rot.

## A quiet world is not a client falling behind

`ResnapshotAfterTicks` is measured against the last tick a delta actually went
out on, not against the tick number. A client acknowledges the last tick it
*applied*, and a world where nothing moves sends nothing to apply — so a client
in perfect agreement with a still world stopped acknowledging new ticks and was
re-snapshotted for it, every hundred and twenty-one ticks, for as long as the
world stayed quiet.

**Values only, for the same reason.** A tick that sent nothing but a `Structure`
message produced nothing for the client to apply and therefore no new
acknowledgement, so counting it as a tick that streamed is the same bug one step
along. So is a tick whose every message the byte budget refused: nothing went
out, and `Statistics::Deferred` is the number that says what is happening.

## Priority decides the order, and only the order

`Replicate` and the interest predicate decide what a client may be sent.
`SetPriority` and the rotation decide what goes first when not all of it fits,
and can put nothing on the wire that interest excluded.

**The cap is per client.** The budget is `net::Link`'s and there is one link per
connection, so a per-server cap would have to be divided among clients before
anything could enforce it — and that division is a per-client cap with an extra
step. Interest is per client too, so a shared ordering would spend one client's
bandwidth on another's entities. A machine's uplink is still a real limit; it is
`MaximumClients` times the per-client budget, and that is a deployment decision.

**The rotation outranks the score, and it is not a weighted sum.** A sum lets a
permanently high score hold a low one off the wire for the life of the
connection. A value that has waited `StarvationTicks` jumps every score there
is, so the longest anything waits is that deadline plus the ticks it takes to
drain what was already waiting — a bound rather than a hope.

**Nothing is ranked on a tick that fits.** The delta is packed in the order the
dirty bits handed it over, and only re-packed by score when that did not fit.
A scheme that ranked every entity every tick would be a tax on every server that
was never over budget.

**Two counters, and they mean different things.**
`Authority::Statistics::Deferred` is the authority holding traffic over, which
is the ordinary answer to a world larger than a link.
`net::ConnectionStats::SendsOverBudget` is the *link* refusing, which after the
cap exists means a misconfiguration or a retransmission storm. D00007 says the
diagnosis people will get wrong is "that component is broken"; `Deferred` is the
number that says otherwise, and `Listener` warns at construction when
`MessagesPerTick + ChunksPerTick` exceeds `LinkSettings::PacketsPerTick`.

**Nothing in the ordering may read a clock, a pointer or an unordered
container.** The comparator is a total order — score, then wait, then entity
handle, then component — because `std::sort` is not stable and two runs of one
server must produce the same bytes. The recovery walk sorts the unconfirmed
entities for the same reason, and that one is not hypothetical: the map was
walked in place, and the only way to see it is to build the same set of entries
through two different sequences of insertions.

## Not here yet

- **Lag compensation** — rewinding the server to what a client saw when it
  fired. Its own plan, and it needs the history buffer this does not keep.
- **A distance or a line of sight in the priority score.** `SetPriority` is the
  hook and nothing in this engine fills it in, because the two scores worth
  having are read off a transform and this module carries named components
  without knowing which of them is a position. The same argument
  `SetInterest` is built on. Until a game supplies one the rotation is in sole
  charge, which is a plain round robin.
- **A value lost from a tick that took several messages.** A tick's delta is
  split into as many independently applicable messages as it takes, the client
  applies whichever arrive and acknowledges the tick, and the server retires
  every value that tick carried — including the ones in the message that never
  came. So the v0.3 unconfirmed-entry resend is self-healing for a tick that
  fits one datagram and not for one that does not. What is stranded is narrow —
  a value that moves once and then never again, since anything that changes
  arrives on its own merits next tick — and closing it needs the client to know
  a tick had more parts than it received, which is a field on the wire.
  `replication/tests/Loss.cpp` pins it.
- **Authenticating a client, and authenticating the server.** The exchange is
  wired in, the stream that follows it is encrypted, and a stranger no longer
  gets a slot for a datagram — but the agreement still binds to no identity, so
  a peer on the path can hold one exchange with each side and read everything.
  **Encryption without that binding is protection against a listener and not
  against a relay**, and the two are worth keeping apart in one's head. Closing
  it means binding the transcript to a server identity — the Ed25519 key
  `assets` already verifies manifests with — which `net/Handshake.hpp` marks as
  its own item. Until then the honest statement is the one in `Listener.hpp`:
  the default admits anybody who completes the handshake.
