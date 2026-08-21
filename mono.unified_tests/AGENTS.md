# mono.unified_tests - module invariants

A diagnostic product: every arrangement of this engine's halves in one process,
and every module's own report side by side.

## What it is for, and what it is not for

**Two questions, and they are different.**

The first is **which stage lost the world**. `server --listen` beside `client
--connect` puts a handshake, a UDP socket, packet framing, an encrypted stream,
a reliability window and a bandwidth budget between the thing that serialises
and the thing that draws. A world that does not appear is equally consistent
with a component nobody called `Replicate` on, a datagram that never arrived,
and a draw list that was filled and never read - and two processes with no
shared address space is the worst place there is to find out which. So
`Arrangement{}` - `direct` - joins the two halves at the only seam that matters:
`replication::Authority::Outgoing` hands its byte vectors **directly** to
`replication::Replica::Receive`. Serialise into deserialise, in order, complete.
`unified::Harness` is that arrangement and keeps a name of its own because it is
where a bisection starts.

The second is **whether the modules agree with each other**. Every module here
has a suite and every one of those suites is better at its own subject than
anything in this module could be. What none of them can check is the seam to the
next module along, because the tier system stops them linking it - correctly.
`mono.server` may not link `mono.client`. `Engine::replication` may not link
either. `mono.cdn` knows nothing about a game link. So the claims that span two
modules have exactly one place they can live, and this is it.

**A failure that reproduces under `direct` is above `net`. A failure that does
not is below it**, and `mono.engine/replication/tests/Wire.hpp` is where that one
gets cornered - it runs the same exchange over a real loopback with real
framing, real encryption and `net::LossyTransport` losing a seeded share of it.

## The matrix is the product, not a selection

`unified::Arrangement` has three axes and `AllArrangements` is their whole cross
product:

- `Transport` - `direct`, `loopback`, `lossy`. Nothing between the halves, a
  real `net` link, or a real link that loses datagrams.
- `Content` - `relayed` or not. A `cdn` publication on disk, a
  `server::ContentRelay` answering out of it, a `client::ContentLink`
  reassembling.
- `Discovery` - `advertised` or not. A `network::Beacon` announcing and a
  `network::Directory` collecting.

**Do not prune the product.** A combination somebody thought was uninteresting
is a combination nobody tested, and the interactions are where the value is:
content over a lossy link is a reliable ordered channel with a hole in it,
which is neither `server.contentrelay`'s case nor `client.contentlink`'s and is
not the sum of them.

**Adding an axis means adding it to `AllArrangements`, to `Arrangement::Name`
and to the table it parses from - and to nothing else.** `just unified-soak`
reads the list out of the program with `--list-arrangements` rather than
holding a copy, and `tests/Crossing.cpp` iterates the product rather than naming
its members.

## Every report is imported, never re-declared

`unified/Reports.hpp` holds `server::ContentRelayStatistics`,
`cdn::PublishReport`, `client::ContentLink::Counters`, `net::ConnectionStats`
and the rest **as the types those modules define**.

**Do not copy fields into a local struct.** A field renamed on the other side
has to break this build; a hand-copied mirror keeps compiling and keeps
reporting a number nothing produces any more. That is the same lesson the
replicated component list below teaches from the other direction.

`Reports` uses `std::optional` for a module an arrangement omits. **A zero is
not the same as an absence** - a relay that served nothing and a run with no
relay in it are different facts, and one zero for both is how a matrix reports
twelve passes having run one arrangement eleven times.

## A contradiction names two modules, always

`CrossCheck` returns disagreements. **Every one of them names two modules,** and
that is the entry requirement: a claim about one module belongs in that module's
own suite, where it will be checked by people who know the subject.

- "the relay was asked for more routes than the client asked for" - `client` and
  `server`, and neither links the other.
- "the beacon announced ten times and the directory heard nine" - `network`'s
  encoder against `network`'s decoder, which its two suites run separately.
- "a message is larger than a datagram carries" - `replication` against `net`,
  checked in *every* arrangement including the one with no datagrams, because
  the one with no datagrams is where it is produced.

If a check here ever names one module, move it.

## It draws through mono.client's seam, not a copy of it

`client::BuildReplicatedWorld` and `client::RecordReplicatedTick` are the
functions `--connect` runs. A harness that filled its own draw list would prove
the harness.

The cost is real and is accepted rather than hidden: linking `Mono::client`
brings `Engine::render` onto the link line and stages its shaders beside a
binary that opens no window. **Do not fix that by reimplementing the draw
pass** - the day the two diverge is the day this program stops answering the
question it was built for.

The same rule applies to the content path: `ContentLink` and `ContentRelay` are
the client's and the server's own, and content crosses a wire wrapped in a
`replication::User` message because that is what `Connector::SendUser` does.

## The tier escape

`mono_add_library(... TIER client ... ALLOW_TIER_ESCAPE Mono::server)`.

This is the edge `D00008` reserved and **this is its first real user.** A
program that hosts a server and draws its world in one process is precisely the
arrangement the rule exists to make deliberate.

**It is not single-player.** That edge belongs in `mono.client/CMakeLists.txt`,
is written out in a comment there, and is still not declared - reaching for this
one to avoid declaring that one would be the precedent D00008 is about.

## The replicated component list is duplicated on purpose

The default list lives in `replication::DefaultReplicatedComponents` and the
server names the same components independently.

**Do not factor the two into one list.** If they disagree this program says so
on its first tick: a component the server replicates and this does not comes out
as rows arriving with no size to draw them at, which is the exact symptom that
sends somebody looking at the network. Sharing the list hides the class of bug
this exists to find.

## Time is passed in, never read - with one stated exception

A tick is a call and a frame is a call. `Settings` and `Arrangement` alone
determine a run, so two runs of the same command agree - which is what
`tests/Harness.cpp`'s probe case asserts, and why `Settings::Workers` defaults to
one rather than to the machine. Every clock the arrangement needs -
`Session`, `ContentRelay`, `Beacon` - is fed `Crossing::Now`, which advances by
one tick period per `Step`.

**The exception is `--seconds`, and it exists because a heap slope is bytes per
*second*.** A leak cannot be fitted to a tick count, so the soak needs a wall
clock and says so. It makes the *number of ticks* in a run a property of the
machine; it changes nothing about what a tick does.

Loss is nominated by ordinal and never by percentage, for the reason
`net::LossSettings` gives: "the fortieth message never arrived" is a test and
"ten percent loss" is a flake with a story attached. **What an ordinal numbers
depends on the transport** - messages under `direct`, datagram arrivals under
`lossy` - because those are the only things each of them can lose.

## The heap tags are the leak's return address

`Crossing::Step` opens a scope per stage: `unified.server.simulate`,
`unified.server.publish`, `unified.carry.direct` or `unified.carry.wire`,
`unified.content`, `unified.discovery`, `unified.client.record`,
`unified.client.draw`.

**A stage that stops pushing its tag makes every future soak blame `untagged`,**
which is the one answer that says nothing. `tests/Crossing.cpp` asserts the tags
exist after a run, so removing one is a failing test rather than a quiet loss of
resolution.

`just unified-soak` is the long-running check and `just heap-soak` is the
client's. They find different things: a leak in a session's retransmission
buffer or in the relay's reassembly is invisible to a client connected to
nothing.

## Reading the report

The columns are the four stages in order and the first one that stops making
sense is the answer:

- `msgs`/`bytes` zero - the authority produced nothing. A component is not
  replicated, or nothing is dirty.
- `applied` stuck at zero - the replica refused what it got. Its own counters
  say which kind, and under `direct` none of them can be loss, because there is
  none.
- `cli-ent` below `srv-ent` - rows did not arrive.
- `drawn` below `cli-ent` - rows arrived without a `Bounds` or a `Visual`.
  Those cross once, in the joining snapshot, so losing them is permanent.
- `drawn` equal to `cli-ent` and the scene still empty - it is being drawn and
  not being *looked at*. That is a camera problem, not a replication one.
- `frozen` equal to `frames-per-tick - 1` - the world is stepping once per tick
  rather than being interpolated. `D00010`.
- `routes` stuck at zero under a `relayed` arrangement - no route ever
  completed. The relay's `refused` and `rate-dropped` counters say which.

Underneath it, every module's report, and then the contradictions. **A run with
no contradictions is the claim; the reports are the evidence for it.**
