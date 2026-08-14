# replication - module invariants

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
and a message resolves names to ids once rather than per entity - the same rule
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
  module owns *when*.** `ecs` deliberately built the operation and no policy -
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
   game's policy, then the X25519 agreement - and only then is a slot taken.

Moving any of those later is the bug this ordering exists to prevent. In
particular the agreement must stay behind the cookie: it is the only step that
costs real arithmetic, and in front of the cookie it is arithmetic any stranger
can make this process do.

**The bound stays.** `MaximumClients` and `Statistics::Turned` are defence the
handshake sits in front of, not defence it replaces.

**An entity with no replicated component is not visible to anybody.** Interest
filters entities and `Replicate` filters components, and the entity that passed
the first and had nothing left after the second used to cross as a bare row in
the join snapshot - no data, and a count of a world the client was told it may
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
`Link::Reserve` and a refusal is also what ordinary backpressure looks like -
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

## A value crosses in its compact form, and the store never sees one

`ecs::TypeDescriptor::Wire` is a second, lossy serialisation a component may
carry - `scene::Transform` is twenty-eight bytes in a store and ten on a
datagram, `scene::Motion` twenty-four and twelve, which is a measured 25 entity
values a datagram becoming 50. Three rules, and the build checks none of them.

**It happens on the wire and never in the store.** The server is authoritative
and a quantised value must not feed back into its own simulation, or
`just determinism` and `just replay-check` stop being byte-identical. The
natural implementation is a codec per component and the natural mistake is to
round-trip the authority's own values through it, after which the server is
simulating the client's approximation of its world.
`engine.replication.quantisation` ticks a served world beside an unserved one
and requires the two to save to the same bytes, which is exactly the comparison
those two recipes make.

**The snapshot path and the delta path have to agree, and they are two places.**
A delta is built from the dirty bits and a join snapshot is built from a scratch
store, so `BeginSnapshot` puts every value with a wire form *through* it before
copying - the snapshot then carries what the far side would have decoded. If one
path quantised and the other did not, a client's world would depend on when it
joined, which never shows as a failure and always shows as drift between two
clients. The scratch store is the only place that round trip is safe.

**A decode has to be exact on both ends, and that is why (a) came before (b).**
Every grid step is a whole number over a power of two and a decode is one
correctly-rounded division, so the value a client holds is a value the server
can predict bit for bit. D00015(b)'s group signatures hash exactly that; a
decode that only nearly agreed would force a tolerance, and a hash with a
tolerance is not a hash.

`WireBytes` and `WriteValue` in `Authority.cpp` and the one branch in
`Replica::Apply` are the only three places that choose between a type's two
serialisations. A fourth is a place that can disagree with the other three.

## Deltas come from the dirty bits, not from a diff

`ecs::ChangeChannel` already records what moved, for `.Changed` and for render
invalidation; a delta is the third reader of the same bits. Do not add a second
record of what changed, and do not diff two snapshots - a diff costs the size of
the world every tick whether or not anything happened.

`EachChangedBatch` yields *runs* rather than rows precisely so that a delta is a
memcpy per run. A per-entity copy here would undo that.

## The audit is what makes the delta path's optimism safe

`Audit.hpp` hashes groups of replicated state, sends the digests, and takes back
the groups a client says it disagrees with. It is **not a second way to send
state** - it sends none. It is the only thing in this module that can notice a
client quietly holding the wrong value, which is the class of bug v0.15 chased
one cause at a time: the lost creation, the stranded value, the stale forget, the
tick that never completed. `D00015(b)`.

**It only ever catches *stale* divergence, and every other decision falls out of
that.** Anything genuinely moving is already being corrected by ordinary deltas,
so a mismatch is by definition not urgent - which is why the cadence is slow, the
slice is small, both messages are unreliable, and the audit is the last thing
built in a tick so the byte budget turns it away before it turns away anything
that matters.

**Only what a client has already acknowledged is hashed, and that is what makes
the comparison exact rather than approximate.** An entity with an entry in
`Unconfirmed` is one the delta path is still correcting, so the server is simply
ahead of it; hashing it would report a mismatch every time the budget deferred
something, on exactly the servers the budget exists for. Three sets are left out
for three different reasons and none of them is a hedge:

- **Anything unconfirmed**, above.
- **Anything the client owns.** Under v0.13 ownership the client's copy is the
  newer one between submissions, so the server is the side that is behind.
- **Anything carrying a `SuppressWhenTagged` tag.** The far side *derives* that
  row - the two ends are meant to disagree - and a hash has no tolerance.

**Membership is on the wire, and that is the decision the shape turns on.** The
audit lists the entities it hashed rather than letting the receiver work them out
from a group number, which is what lets the sender exclude the three sets above
without the receiver knowing anything about them. It is also why nothing about
interest management had to change: the open question in `D00015` was per-client
against per-cell, and a cell hash is only shareable if a client sees a whole cell
or none of it. **The resolution is per-client over a rotating slice**, because the
rotation is what bounds the per-client cost anyway and turning interest
management from per-entity into per-cell is a larger decision than that entry
authorises.

**The answer is upstream traffic from a peer, so the limit is the server's.** A
client claiming everything mismatches is request amplification, and none of what
bounds it is taken from the message: the tick has to be the audit this server
issued, the labels have to be groups this server hashed, they have to be strictly
ascending so one cannot be named twice, and an audit may be answered once. The
most an answer can therefore buy is the repair of exactly the slice the server
had already chosen to look at, once every `AuditSettings::EveryTicks`. An audit
the link refused is struck off by `Unsent` for the same reason - a question that
was never asked may not be answered.

**Both ends hash the value a replica holds, not the value each of them holds.**
The authority puts its own value through the same encode-and-decode
`BeginSnapshot` puts a join through, so the two ends compute one expression over
one buffer. Assuming the quantiser is idempotent instead would be wrong on the
real codec: `scene::Transform`'s smallest-three rotation re-encodes differently
for **1666 of 2 million** uniformly random orientations, because the recovered
component can come back below one of the three that were sent and the next encode
drops a different one.

**The repair is the recovery walk, not a resend.** A disputed group puts its
entities back into `Unconfirmed`, which is the same seeding an entity coming into
view already gets. A second path that resent values would be the second way to do
one job. What that does not reach is a client holding an entity the server has no
record of sending it - the server cannot name what it does not know about - and
the honest bound on that case is the one this module already has:
`ResnapshotAfterTicks`, reached because a delta naming a row the client does not
hold never lets `Applied` move. `Statistics::Disputed` is the number that says
whether any of this is happening.

**It is off by default and `mono.server` turns it on.** A quiet world sends
nothing is a property this file states two sections down, and anti-entropy is
exactly the thing that has to speak on a world at rest. Those cannot both hold,
so which one a host wants is the host's to say.

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

## Prediction is the local player and nothing else. Dead reckoning is not prediction

**Amended at v0.15, on purpose and here, because the rule as written forbade
`D00015(c)` and was never about it.** What it was about is *agents*: predicting
a second entity means predicting what another player will do, which is wrong
more often than it is right and is visible as rubber-banding when it is wrong.
That argument is untouched and so is the rule it produces - **no entity but the
one `SnapshotBuffer::Predict` names is ever run ahead of the authority on the
strength of inputs, nothing replays an input for a row it does not own, and
`Store::CreatePredicted`'s index range is still the only identity a replica may
mint.**

What the rule did not distinguish is the other thing a client can do with a row
somebody else owns. Predicting an input-driven agent is guessing at a human.
**Dead-reckoning a body is evaluating a function the authority already sent** -
`scene::Motion` *is* the derivative of the pose, so integrating it reads the
message rather than inventing one. Three rules follow, and the build checks
none of them:

- **A body nobody owns may be dead-reckoned, for presentation only.** The guess
  is an offset on the pose `Sample` hands back and it reaches no component, for
  the reason the next section already gives about interpolation and one more:
  `Audit.hpp` hashes what a replica holds, so a row overwritten by a guess would
  be reported as disagreeing with the authority on every sweep. Presentation was
  already the rule; anti-entropy is what makes it enforced.
- **Anything carrying a `scene::NetworkOwner` may not be.** Under v0.13
  ownership an owned body is simulated by its owner *authoritatively* and there
  is nothing arriving for a guess to be reconciled against, so dead reckoning it
  as well simulates it twice with one of the two wrong - and the wrong one is
  whichever the local machine happens not to own. **Extrapolate what nobody
  owns** is the whole of the test.
- **The guess is bounded twice, and by two different things.** In *time* by
  `InterpolationSettings::ExtrapolateSeconds`, because integrating a quantised
  velocity accumulates error linearly and past a quarter of a second the result
  is worse-conditioned than the quantised pose it started from. In *distance* by
  the body's own half-extent, because that is what stands in for the collision
  nothing here runs. Both are named constants with the derivation at them.

**This is not licence to simulate on a replica.** No broad phase, no narrow
phase, no solver and no gravity: `mono.client/AGENTS.md`'s "nothing there
advances the world" is intact, because nothing here advances one. The whole of
what runs is `physics::Advanced` over one row - the same function the
authority's own `IntegrateMotion` is written out of, so a client cannot be
integrating different arithmetic from the server.

**Where `D00010` and this meet.** D00010 decided a dry buffer *stops* rather
than extrapolating, on the grounds that guessing forward is "a freeze plus a
lie" - the snap arrives when the next tick disagrees with the guess. **That
decision is unchanged**, and `SnapshotBuffer` is where it still is: the render
clock stops at the newest sample, `Sample` holds the last pose the authority
described, and `Statistics::Stalls` counts it. What v0.15 adds sits outside the
buffer and only on the entities carrying the thing D00010 did not have - a
velocity the server sent, which is a right answer to extrapolate *toward*. A
player has none, and neither has a body with no `scene::Motion`: there is no
function to evaluate, so the freeze stands, unchanged and for D00010's reason.

**And the lie is paid back rather than snapped away.** Whatever the guess added
is `velocity * seconds`, so the correction is `seconds` easing to zero at
`InterpolationSettings::UnwindFraction` - one number for the whole world instead
of a per-entity blend, continuous by construction, and unwound slowly enough
that a corrected body never appears to move backwards.

## The two halves pull opposite ways, and `SnapshotBuffer` is the other one

Prediction runs the local player *ahead* so input feels immediate.
`SnapshotBuffer` draws everything else *behind* - at a fixed delay from the
newest received tick, interpolating between the two that bracket it - so that a
world arriving at 30 Hz does not judder at 30 Hz on a screen running at 240.

**They must never be applied to the same entity.** Delaying the predicted row by
the jitter budget puts back exactly the lag prediction exists to remove, and the
player feels it as their own character lagging their own keys. The exclusion is
structural rather than a caller's discipline: `SnapshotBuffer::Record` refuses
the nominated entity *and* anything in `Store::CreatePredicted`'s index range,
before it records anything at all - so a tick offered nothing but predicted rows
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
happened to be watching it - the rule `world`'s `ViewChannel` already follows.

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
each under `ChunkBytes` and **each independently applicable** - never a
reassembly, because this is the unreliable channel and holding a part back until
its siblings arrive is a stall on a path whose premise is that the next tick is
already on its way. Every part is applied the moment it lands. That is why
`Replica` treats a delta at the tick it has already applied as another part
rather than as stale.

**The parts are numbered for the acknowledgement, not for the application, and
that distinction is the whole of D00013.** `Delta::Part` and `Delta::Final` say
where a message sits in its tick and which one ended it; `Applied` may name a
tick only once the client holds every part of it. Without that the client
acknowledged on the strength of the parts that arrived and the server retired
the values in the one that did not - self-healing for a tick that fits one
datagram and not for one that does not.

Three things about the marker, and each of them is a way to get this wrong:

- **It is authored by the sender when the tick is packed, and it means "that is
  all of tick N".** Not "nothing else changed". The priority rotation
  deliberately holds values back under a budget, and what it held back was never
  part of this tick - it keeps its unconfirmed entry and comes back on a later
  one. A marker derived from what changed would leave every trimmed tick
  unacknowledged, on exactly the servers the cap exists for.
- **A part number is a position, not an arrival order.** Parts arrive out of
  order, twice, or not at all, so the receiver keeps a set of positions and a
  contiguity check up to the final one. A count of arrivals reads a duplicate as
  progress.
- **An incomplete tick is passed over, never waited for.** The unreliable
  channel does not resend, so a part that is gone is gone; the next tick
  re-offers everything the missing one carried and acknowledging *that* tick
  confirms it. One lost part costs one tick of acknowledgement. The bound on the
  case where no tick ever completes is `ResnapshotAfterTicks`, which is this
  module's existing answer to a client that cannot be caught up by deltas - and
  `Replica::Statistics::Incomplete` is what says it is happening.

**A part the transport refused is not a part that went out**, so `Unsent` rolls
`Client::Streamed` back for a tick whose delta was cut short. The client cannot
acknowledge a tick it holds only some of, so counting one against its silence
measures it against something it was never given the chance to answer - and on a
link whose packet budget is below `MessagesPerTick` that is every tick, for ever.
Same argument as the quiet world and the held-back budget, one layer down.

**A `Structure` message is split too**, and the forget was the third path to be
missed. A world leaving view all at once names every entity in one message, and
three hundred handles is already past a datagram - which `Link::Reserve` refuses
outright rather than fragmenting, so the message saying "stop drawing these"
would be the one that never arrives.

## A refusal is backpressure for some messages and a hole for the rest

**Send `Authority::Outgoing` in order and hand back what the transport would not
take, through `Authority::Unsent`.** This is the shape of bug the chunking fix
did not cover, and it cost a suite that failed one run in three.

A delta needs nothing undone: the unconfirmed set rebuilds it next tick. A
snapshot chunk does, and this is the asymmetry - the cursor moves when the chunk
is *built*, so a refused chunk is a gap in a stream the receiver waits on for
ever. The observed failure was a client that applied 184 of 192 chunks, never
reached the last byte, never joined, and then refused every delta that followed
as stale: `applied=184 refused=17865`, which reads like a protocol error and was
a cursor. A `Structure` message is the same shape - a creation, a destruction and
a forget are each said exactly once and each moved the known set when it was
built.

**`Unsent` covers a refusal and not a loss.** A message the transport accepted
and the network then dropped is a different problem and has a different answer;
see the next section.

The buffer is therefore released **one tick after** the cursor reaches the end,
because `Unsent` is called after `Publish` has returned.

## A join is two blobs, and only the second one ends it

`Authority::SetPreface` names the entities a host wants a client to have before
the world. They are captured as their own `ecs::Store::Save`, at the same tick
and from the same walk as the world's, and every byte of that blob is handed to
the transport before the world's first chunk is built. **Priority could never
have done this**: `SetPriority` orders the values a running world produces, and a
join is one save chunked across ticks in the order the store wrote its
archetypes. That is the whole of D00122.

**The preface merges and the world sweeps.** `Replica` applies a preface with
`ecs::ApplyMode::Overlay` and the world blob with `Authoritative`, and the world
blob still carries the preface's entities. Swapping the modes is the failure this
shape invites: a preface is a slice of a world, so applying it authoritatively
sweeps everything it does not mention - nothing on a join, and the entire world a
client already holds on a *re-snapshot*, wiped a moment before being sent again.

**A tick's chunks come from one blob.** Chunks go out in the order `Outgoing`
was built, but a refusal is per message - so a preface chunk the link turned away
beside a world chunk it took would be resent behind bytes it was supposed to
precede. Two blobs never share an outgoing list, which makes the ordering a
property of the code rather than of the packet budget. It costs one tick at the
seam of a join that already spans many.

**`Carried::Stage` is the second half of the refused chunk.** The section below
on refusals is about one cursor; there are two now, and a refusal that rewound
the wrong one is `applied=184 refused=17865` with a second way to reach it.

**A host that declares no preface is unchanged, and that has to stay true.** An
empty predicate is an empty blob, no extra tick, and the same single-cursor join
this module always had.

**What is missing is the ordering over the scripts, not the scripts.** Roblox
also runs `ReplicatedFirst`'s scripts before the rest of the tree arrives.
`client::BuildReplicatedWorld` opens a `script::Runtime` and `replica-scripts`
runs every client-side `LocalScript` as it lands, so a client does run scripts
out of its replica - what nothing states is which of them may run before the
world's first chunk has. The preface is the half that exists; the guarantee that
a named script goes ahead of it is the half that does not.

## Structure goes on the reliable channel, and values do not

`Delta` carries what moved. `Structure` carries which entities the client holds
- created, destroyed, forgotten - and `Session::ChannelFor` puts it on the
reliable channel while the delta stays unreliable. That split is the whole of
D00011's answer and it is not an exception to `net/AGENTS.md`'s "unreliable by
default": a value is superseded by the next tick and a structural change never
is, so they are the two sides that rule already draws.

**A tick acknowledgement cannot repair a lost structural change, and this is the
reasoning worth keeping.** `Applied` names a tick, not a message. The server's
known set moves when a creation is *said*, so a lost one leaves the server
certain the client holds an entity it has never heard of - and the client goes
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

**`Applied` means the last tick applied *in full*, and `Replica` enforces it in
two ways.** A delta naming a row this client does not hold yet - which is
exactly what a creation still in flight looks like - is applied as far as it can
be and does not move `Applied`. Without that, a creation arrived reliably some
ticks late and the entity held none of its components, because the tick its
values were in had already been acknowledged. And a tick short of one of its
parts does not move it either; see the section above.

## An entity coming into view has not moved

A delta is built from the dirty bits, and an entity entering a client's interest
did not change - it was always there and that client could not see it. So the
run walk finds nothing for it and it used to arrive as a bare row with none of
its components, for as long as it stood still.

`Authority` seeds an unconfirmed entry for every entity it has just told a
client about, which puts it through the recovery walk that already reads current
values and already keeps offering until the client confirms. A second path that
did the same job is the second one that would rot.

## A quiet world is not a client falling behind

`ResnapshotAfterTicks` is measured against the last tick a delta actually went
out on, not against the tick number. A client acknowledges the last tick it
*applied*, and a world where nothing moves sends nothing to apply - so a client
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
anything could enforce it - and that division is a per-client cap with an extra
step. Interest is per client too, so a shared ordering would spend one client's
bandwidth on another's entities. A machine's uplink is still a real limit; it is
`MaximumClients` times the per-client budget, and that is a deployment decision.

**The rotation outranks the score, and it is not a weighted sum.** A sum lets a
permanently high score hold a low one off the wire for the life of the
connection. A value that has waited `StarvationTicks` jumps every score there
is, so the longest anything waits is that deadline plus the ticks it takes to
drain what was already waiting - a bound rather than a hope.

**Nothing is ranked on a tick that fits.** The delta is packed in the order the
dirty bits handed it over, and only re-packed by score when that did not fit.
A scheme that ranked every entity every tick would be a tax on every server that
was never over budget.

**Three counters, and they mean three different things.**
`Authority::Statistics::Deferred` is the authority holding traffic over, which
is the ordinary answer to a world larger than a link.
`net::ConnectionStats::SendsOverBudget` is the *link* refusing against a number
somebody configured, which after the cap exists means a misconfiguration or a
retransmission storm. `net::ConnectionStats::SendsOverAllowance` is the *path*
refusing, which is congestion and is nobody's mistake. D00007 says the diagnosis
people will get wrong is "that component is broken"; `Deferred` is the number
that says otherwise, and `Listener` warns at construction when
`MessagesPerTick + ChunksPerTick` exceeds `LinkSettings::PacketsPerTick`.

**The link's allowance is the authority and `BytesPerTick` is a ceiling on what
this module will *produce*.** `Authority::Pack` spends the lower of the two, and
`Authority::SetAllowance` is how the number gets here - `Listener::Advance`
reads `ConnectionStats::SendAllowanceBytes` immediately after `ResetBudget`,
which is where the link decides it.

Packing past the allowance never sent more: `Link::Reserve` refuses the excess
and `Unsent` rebuilds the same rows next tick. What it did was spend the encode
and hand the shortfall to the **transport** rather than to the priority
scheduler, which is the thing that exists to decide what a client sees when not
all of it fits. A host that never calls `SetAllowance` keeps `SIZE_MAX` and
behaves exactly as this module did before the controller existed, which is also
how the case tells the two apart.

**Told rather than read, because this module holds no link.** Same division as
`SetPriority` and `Rewind::Record`: the arithmetic is here and the lookup is the
host's. An `Authority` that reached for a `net::Link` would be an `Authority`
that knows what a socket is.

**Nothing in the ordering may read a clock, a pointer or an unordered
container.** The comparator is a total order - score, then wait, then entity
handle, then component - because `std::sort` is not stable and two runs of one
server must produce the same bytes. The recovery walk sorts the unconfirmed
entities for the same reason, and that one is not hypothetical: the map was
walked in place, and the only way to see it is to build the same set of entries
through two different sequences of insertions.

## What an instance *is* crosses whole, and it is three components

`ecs.Hierarchy`, `ecs.InstanceName` and `ecs.InstanceClass` are all replicated
by default. Until v0.15 only the first was: the rule read "`scene.` or
`ecs.Hierarchy`", so the other two crossed in a join snapshot - `Store::Save`
carries every component - and never in a delta. An entity the world already held
was named on a client and an entity created while that client was connected
arrived with no name and no class, which `server.replication`'s
private-containers case saw as four children of a `Player` called nothing.

**A prefix with an exception written into it cannot fail loudly, so it is a
list.** `Defaults.cpp` names the three, and `engine.replication.defaults` walks
every registered `ecs.` component and requires each to be either in that list or
in the exclusions the suite names. A fourth component added to `ecs` is a red
suite rather than a silence on a delta. Rule 6; the build does not check it and a
test does.

**They are signed, so they cross once per change and not once per creation.**
Saying a name in the `Structure` message that creates the entity would be one
reliable copy and nothing per tick after it, and it would be the v0.7 recolour
bug again: `.Name` is a writable property a script sets whenever it likes, and a
fact that crosses only at birth is wrong for ever afterwards. What guarantees
arrival is the mechanism every other value already has - an entry in
`Unconfirmed`, re-offered every tick until the client acknowledges a tick it was
in.

The steady-state cost is therefore **zero bytes**: a hash of two four-byte
columns per tick, and traffic only on the tick something is created or renamed.
That is also why there is no string table for the class name. It is text on a
wire, and a per-connection dictionary to compress a message sent once per
instance would save nothing on the ticks that matter.

**A class crosses as its registered name, and that is `ecs`'s registration
rather than a wire form here.** A `ClassId` is a registration index and rule 4
forbids one on a wire: `Classes::Register` runs wherever the code needing a tree
runs and `RegisterGuiClasses` is called lazily on first use, so two processes of
one build can number a class differently depending on which of them opened an
interface. Changing `Components::Register<InstanceClass>`'s serialiser - rather
than fitting a `TypeDescriptor::Wire` over it - fixes the save file and the
studio's edit stream in the same breath, and leaves one answer to "how is a class
written down" instead of two.

**A class name the receiver does not know leaves the entity untyped, and the
world untouched.** `ClassOf` answers with an invalid id and a warning names the
class. That is the render half's answer to a pipeline it cannot build, not
`studio::RojoSync`'s substituted `Folder`: a Rojo project is written against a
class tree this engine implements a fraction of, so substituting is right there
and here it would make `:IsA` tell a confident lie about two builds that do not
match. Every other component on the entity still arrives.

## A delta's rows are not one width

`Authority::Pack` records an **offset and a length per row**, not one stride per
component. A stride is right only while every row encodes to the same number of
bytes, which is true of a `Transform` and false of anything that writes a name:
`scene.Visual` writes two and `ecs.InstanceName` writes one, and a name is as
long as its text.

It was reachable before instance names crossed - two parts whose meshes are spelt
differently is enough - and invisible because a demo world's mesh names are
almost all empty and therefore all four bytes long. The receiver was never the
problem: `WriteComponents` reads the values sequentially and always could take
any width. It was the sender slicing rows back out at `row * stride` after the
priority sort had reordered them.

The guard at the top of `BuildComponents` is about the *component* and not about
a row: a type whose stored size already exceeds a message can never produce a row
that fits. What a given row costs is only known once `offer` has written it.

## Not here yet

- **A weapon, a health pool, or anything else a game decides.**
  `examples::Shooting` is the demo's rule and `mono.server` joins it to `Rewind`;
  a second game would write its own, which is the point of `Input::Bytes` being
  "the game's own encoding". A hit test in *this* module would be a game rule in
  a network one.
- **A line of sight in the priority score.** Supplied at v0.9 as
  `DistancePriority::Blocked`, on the same terms as the rest. Distance is
  supplied too -
  `replication::DistancePriority` - and the split it is built on is the thing to
  preserve: **the arithmetic is here and the lookup is not.** A caller hands in
  two accessors, one for where a client is looking and one for where an entity
  is, and this module still names no component and links no simulation module.
  A scorer that reached for `scene::Transform` directly would be the coupling
  `SetInterest` and `SetPriority` were both shaped to avoid.

  **A hidden entity is scaled, never zeroed.** One scored at nothing is one the
  rotation alone ever sends, so a player walking round a corner meets a wall of
  objects snapping into place. It should update *less*, not never.

  **The cheap test gates the expensive one.** A raycast is orders of magnitude
  dearer than the subtraction beside it, so anything already scoring below
  `OcclusionFloor` is never asked about - it cannot be moved far enough by
  occlusion to change its place in the order.
- **Authenticating a *client*.** The server is authenticated at v0.9 - see
  below - and the other direction is not: `SetAdmission` still decides who gets
  in on whatever a game knows, and the default admits anybody who completes the
  handshake. A client identity would be a second key pair and a second pin, and
  it is a different problem from the relay one.

## The server's identity is what makes the encryption worth having

`Welcome::Identity` is an Ed25519 signature over the same transcript the
confirmation tag is computed on, and `ConnectorSettings::ServerIdentity` is the
pin a client checks it against.

**The tag and the signature are not the same check and neither replaces the
other.** The tag proves the sender reached these keys - which a relay also does,
because it reaches them by holding one exchange with each side and reading
everything in between. The signature proves *which* server reached them, because
a relay cannot make the server sign a transcript naming the relay's own
ephemeral key.

`tests/Admission.cpp` models that relay properly and the first version of the
case did not: it had the relay forward the server's welcome unchanged, which is
refused at the *tag* and proves nothing about the signature. The case that
matters runs two exchanges, so the tag passes and only the signature stands in
the way - and there is a control beside it that removes the pin and watches the
same bytes get in.

**Both ends default to the weaker mode and both say so.** A listener with no
identity signs nothing; a connector with no pin checks nothing and logs a
warning when it dials. Refusing to run without a key would refuse every
deployment in this engine today; refusing *quietly* would be worse than either.

**A pinned client refuses rather than downgrades.** A setting that looks like
security and is not is worse than no setting, because somebody relies on it.

## `Rewind` records and does not restore

Lag compensation here is a *record* of past placements and a query against it.
Nothing is re-simulated and no state is put back, and that is the line to hold:
rollback is a much larger idea with a much larger blast radius, and a hit test
does not need one.

**The lookup is the caller's, as `DistancePriority`'s is.** `Record` is handed a
position rather than reading one, so this module still names no component. A
version that reached for `scene::Transform` would be the coupling `SetInterest`
and `SetPriority` were both shaped to avoid.

**A fractional tick, because a client's view is fractional.** A renderer sits
between two ticks and blends - `SnapshotBuffer::RenderTick` returns a `double`
for exactly that reason - so sampling a whole tick answers with a pose the
client never saw. The error is largest for the fastest things, which is where a
hit test is already hardest.

**Half the round trip, not all of it.** The gap between what a client saw and
what the server holds is the time the *snapshot* took to get out; the input's
journey back is already accounted for by the tick the input names. Doubling it
is the classic way to make compensation too generous, and the suite pins the
number.

**Refusals are refusals.** A tick older than the history, or an entity that was
not recorded at the tick asked for, answers `false` rather than reaching for a
neighbouring frame. Answering with a nearby placement would be inventing
something the client never saw, and it would be invisible - the caller cannot
tell an interpolated answer from a fabricated one.

**A repeated or out-of-order `Begin` is refused.** A frame is identified only by
the tick stamped on it, so one landing in the slot the next tick wanted makes
every later query find the wrong frame with nothing saying so.

## A client proves itself the opposite way round from a server

`ConnectorSettings::ServerIdentity` **pins** a key: a server has one identity
and every client knows which to expect. `Identify` **sends** one: a server
expects many clients and cannot list them in a setting, so the key travels with
the claim and `SetClientPolicy` decides. That asymmetry is how a client
certificate has always worked and is not an inconsistency to tidy up.

**The claim cannot ride the answer.** The transcript names both ephemeral keys
and a client does not have the server's until the welcome, so there is no
earlier message it could sign. It is the first thing said on the encrypted
stream instead, which costs no extra round trip.

**Verification is not a policy question.** A claim that does not verify is
dropped without consulting `SetClientPolicy` - a policy answering "is this key
allowed" is a game's business; one answering "is this key real" would be every
game re-implementing a signature check, and one of them would get it wrong.

## Adding a `MessageKind` breaks one thing silently

The switches are `-Wswitch`-checked and `ReadMessage`'s range comparison is not.
It reads `kind > static_cast<uint8_t>(MessageKind::Disputed)`, and a kind
appended after that one parses as out of range - so every message of it is
dropped as malformed, on both ends, with the counter saying only "malformed".
`Identify` did exactly that. Update the comparison in the same commit as the
enum. There are **two** of them, in `ReadMessage` and in `PeekMessageKind`, and
missing the second drops the message at the router instead of at the parser.
