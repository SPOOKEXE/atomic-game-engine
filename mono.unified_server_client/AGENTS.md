# mono.unified_server_client — module invariants

A diagnostic product: the server's world and the client's replicated world in
one process, with `net` cut out of the middle.

## What it is for, and what it is not for

**It exists to bisect a blank scene.** `server --listen` beside `client
--connect` puts a handshake, a UDP socket, packet framing, an encrypted stream,
a reliability window and a bandwidth budget between the thing that serialises
and the thing that draws. A world that does not appear is equally consistent
with a component nobody called `Replicate` on, a datagram that never arrived,
and a draw list that was filled and never read — and two processes with no
shared address space is the worst place there is to find out which.

So this joins the two halves at the only seam that matters:
`replication::Authority::Outgoing` hands its byte vectors **directly** to
`replication::Replica::Receive`. Serialise into deserialise, in order, complete.

**A failure that reproduces here is above `net`. A failure that does not is
below it**, and `mono.engine/replication/tests/Wire.hpp` is where that one gets
cornered — it runs the same exchange over a real loopback with real framing,
real encryption and `net::LossyTransport` losing a seeded share of it.

**Neither replaces the other, and this one is the weaker.** It cannot see a
message that did not fit in a datagram, and this module has had four separate
bugs from exactly that. `Report::LargestMessage` is the one thing it can say
about the wire without having one, and it is checked against
`net::MAXIMUM_MESSAGE_BYTES` in `tests/Harness.cpp`.

## It draws through mono.client's seam, not a copy of it

`client::BuildReplicatedWorld` and `client::RecordReplicatedTick` are the
functions `--connect` runs. A harness that filled its own draw list would prove
the harness.

The cost is real and is accepted rather than hidden: linking `Mono::client`
brings `Engine::render` onto the link line and stages its shaders beside a
binary that opens no window. **Do not fix that by reimplementing the draw
pass** — the day the two diverge is the day this program stops answering the
question it was built for.

## The tier escape

`mono_add_library(... TIER client ... ALLOW_TIER_ESCAPE Mono::server)`.

This is the edge `D00008` reserved and **this is its first real user.** A
program that hosts a server and draws its world in one process is precisely the
arrangement the rule exists to make deliberate.

**It is not single-player.** That edge belongs in `mono.client/CMakeLists.txt`,
is written out in a comment there, and is still not declared — reaching for this
one to avoid declaring that one would be the precedent D00008 is about.

## The replicated component list is duplicated on purpose

`src/Harness.cpp` names `scene.Transform`, `scene.Motion`, `scene.Bounds` and
`scene.Visual`, which is the same list `mono.server/src/Server.cpp` names.

**Do not factor them into one list.** If the two disagree this program says so
on its first tick: a component the server replicates and this does not comes out
as rows arriving with no size to draw them at, which is the exact symptom that
sends somebody looking at the network. Sharing the list hides the class of bug
this exists to find.

## Time is passed in, never read

A tick is a call and a frame is a call. `Settings` alone determines a run, so
two runs of the same command agree — which is what `tests/Harness.cpp`'s probe
case asserts, and why `Settings::Workers` defaults to one rather than to the
machine.

Loss is nominated by ordinal and never by percentage, for the reason
`net::LossSettings` gives: "the fortieth message never arrived" is a test and
"ten percent loss" is a flake with a story attached.

## Reading the report

The columns are the four stages in order and the first one that stops making
sense is the answer:

- `msgs`/`bytes` zero — the authority produced nothing. A component is not
  replicated, or nothing is dirty.
- `applied` stuck at zero — the replica refused what it got. Its own counters
  say which kind, and none of them can be loss, because there is none.
- `cli-ent` below `srv-ent` — rows did not arrive.
- `drawn` below `cli-ent` — rows arrived without a `Bounds` or a `Visual`.
  Those cross once, in the joining snapshot, so losing them is permanent.
- `drawn` equal to `cli-ent` and the scene still empty — it is being drawn and
  not being *looked at*. That is a camera problem, not a replication one.
- `frozen` equal to `frames-per-tick - 1` — the world is stepping once per tick
  rather than being interpolated. `D00010`.
