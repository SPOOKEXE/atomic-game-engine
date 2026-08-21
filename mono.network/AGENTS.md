# network - module invariants

Finding a peer, and being findable. `shared` tier, above `Engine::net` and
below every program. The client, the server, the studio and the content origin
all import it.

## This module ends at an endpoint

What comes out of here is an `engine::net::Endpoint`. That is the argument
`replication::Connector` takes, the address a `delivery::Source` names and the
host an `http::Client` dials.

**It must not grow a connection.** `replication::Listener` and
`replication::Connector` already own a connected session over a transport, and
two ways to do one job is the most expensive debt in a monorepo - AGENTS.md
rule, and this is the module most likely to violate it, because "connect to the
session I just found" reads like it belongs here. It does not.

The property that buys is the one the whole module exists for: a caller walking
`Directory::Listings` never learns whether a row was heard on the subnet,
listed by a rendezvous point or typed into a config file, except by asking its
`Reach` in order to show a person.

## Time is passed in, never read

`engine::net`'s rule, inherited whole. Every announcement interval, every
expiry and every give-up deadline is an argument.

Two reasons, both the same as `net`'s. A wall clock read inside puts a
non-deterministic input in a subsystem whose failures are hardest to reproduce.
And it makes a timeout something a suite *states* rather than waits for - which
is why the discovery and rendezvous suites run in microseconds and never sleep.

## Everything from a wire is hostile, and an advert is a hint

An advert arrives on an open UDP port from an address anybody can write. Strings
are capped, enums are range-checked against their closed lists, trailing bytes
are a refusal, and a frame that disagrees with itself is dropped whole.

**A listing is never a trust decision.** `Listing::Joinable` is a filter for a
user interface - it hides rows a person cannot act on - and is not the check
that a peer is who it claims. That check happens after a connection exists,
against a pinned identity, one layer up:
`replication::ConnectorSettings::ServerIdentity`.

## Every table is bounded, and the bound drops the new one

`Directory::MaximumListings`, `PointSettings::MaximumSessions`,
`MAXIMUM_INTRODUCTIONS`. An open port receives whatever is sent to it, so a
table that grew with what arrived is a table a stranger fills for the cost of
one datagram each.

**Past the cap a new entry is dropped and an existing one still refreshes.**
That is the right way round and it is easy to get wrong: a table that evicted to
make room lets a flood push out exactly the sessions somebody is looking at,
which is the outcome the cap exists to prevent rather than a gentler version of
it.

## The key ring is a cost multiplier a stranger controls

`Decode` is handed every key this process holds, because a frame does not say
which one it was tagged under and the section below explains why it must not.
So a tagged advert costs one MAC attempt per key before it can be refused.
`network.bench.discovery` prices that: **384 ns against the one key that
verifies, 20.1 us against a ring of sixty-four that do not.** The multiplier is
chosen by whoever is sending, not by whoever is listening, and the sending costs
nothing.

An untagged advert costs the same whatever the ring holds - 73 ns either way -
so every public announcement, which is nearly all subnet traffic, is unaffected.
What is affected is a process holding a lot of keys, and the answer available
today is not to hold a lot: **give a browser the keys for the sessions a person
cares about, not every key it has ever been told.** A per-frame key hint would
remove the trial and reintroduce the oracle, so this is a bound on the caller
rather than something fixable here.

## Ordinals reach a wire, so the lists are append-only

`Reach`, `Access`, `Purpose`, and the rendezvous `MessageKind`. A value may be
added at the end; none may be reordered or removed. `tests/Enums.cpp` writes the
numbers out by hand rather than deriving them, because a test that computed them
from the enum would agree with any reordering.

`Reach`'s order is also behaviour: `Directory` keeps the *smaller* value when
one session arrives twice, because the order is how much has to keep working.

## Private authenticates; it does not hide

`Access::Private` means a `SessionKey` is required. A private session
broadcasting on a subnet is **visible to everybody on that subnet** and joinable
by nobody without the key.

Say that plainly rather than letting somebody assume the name of their session is
a secret. A broadcast datagram is readable by anything on the link and always
will be.

Three consequences, and each is enforced somewhere:

- **A `Private` advert with no key is never announced.** `Beacon::Announcing`
  answers false and the refusal is counted. An untagged private advert is a
  public one wearing a label.
- **A browser lists a private session it cannot verify.** It has to: the person
  about to be given the key never learns the session exists otherwise. The row
  is not joinable and `DirectoryCounters::Locked` says why.
- **`DecodedAdvert::Authenticated` covers three situations and distinguishes
  none of them** - no tag, a tag under a key we do not hold, and a tag that
  failed. A field that told them apart would answer "is this the right key" one
  guess at a time.

## The rendezvous point holds no keys, and must not

Giving it one would make every operator of a point a holder of every private
session's secret, which is the trust this module exists to avoid needing.

So a `Private` registration is **absent from every browse reply** rather than
gated behind a check the point cannot make. Reaching one requires its
`SessionId`, which is 128 bits the host handed over along with the key. The key
is what the *poke* carries, and a private host drops a poke whose tag does not
verify.

That is possession plus return routability. It is not a session establishment,
and nothing here should start behaving as though it were.

## There is no relay, and adding one is a product decision

When both routers refuse, `ReachState::Failed` is the answer. A relay is
bandwidth somebody pays for, a bottleneck with a latency floor, and the piece
that turns a small coordination service into an operational commitment.

A hidden fallback would make "peer-to-peer" mean two different things depending
on the day.

## A punched hole belongs to a port

This is the one that decides API shape rather than policy. A router's mapping is
per port, so a hole punched on a discovery socket gets a *discovery socket*
through and the game's socket is as unreachable as it was.

So `RendezvousClient` borrows the transport it will punch on, and there are two
ways to drive it:

- `Pump` - it owns the drain. What `Presence` does, on the announcing socket.
- `Deliver` - the caller owns the drain and offers each datagram. What a program
  wanting the punch to serve *its own traffic* does, handing over the transport
  that traffic uses.

Routing a shared socket is safe rather than lucky: `engine::net::Packet::MAGIC`
is `ATN1`, an advert is `ATNA`, a rendezvous message is `ATNR`.

## `Presence` is the only thing here that opens a socket

Everything else borrows a transport, which is what makes the whole module
testable over a loopback with real encoding - `TransportSettings::Broadcast`
reaches every other end of a loopback network precisely so that discovery is a
path a suite exercises rather than one tested on somebody's subnet.

Two sockets, and the split is not tidiness. A host announces from an
**ephemeral** port and a listener binds the **well-known** one, so a machine can
host as many sessions as it likes. The rendezvous client shares the announcing
socket, for the reason above.

**A partial success is a success.** A socket that could not be opened turns off
the half that needed it and records a `PresenceFault`; the rest runs. A program
that refused to start over this would be refusing over a feature nobody asked to
be essential.

## Announcements, not probes

Hosts announce on an interval; listeners never ask. The other arrangement lists a
session sooner and costs the property that matters more - every host would have
to bind the well-known port, so a machine could host exactly one session.

It also means a host answers nothing it was not going to say anyway. There is no
datagram a stranger can send a beacon that produces a reply, which is one fewer
amplifier.
