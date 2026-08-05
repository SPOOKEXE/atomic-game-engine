# net — module invariants

L11 transport, `shared` tier. Connection lifecycle, framing and channels — the
layer `upstream/`, `downstream/`, `predict/` and `http/` all sit on.

`http/` exists as of v0.9 and has its own rules, below.

## This module does not know what a component is

Nor an entity, a world or a store. Replication is a **reader** of this, one layer
up. A transport that knows about entities cannot also carry a script's remote
call or a group of content bytes, and all three go down the same wire.

The link to `ecs` would not fail the tier check — both are `shared` — so this is
a convention the build cannot catch, which by rule 6 means it is written down.
It is written down here.

## Time is passed in, never read

Every call that could care about "now" takes it as an argument. There is no
`Clock` member and there must not be.

Two reasons and both matter. A wall clock read inside puts a non-deterministic
input in the middle of the subsystem whose failures are hardest to reproduce —
`ecs/AGENTS.md` bans exactly that inside a system. And it makes a timeout
something a suite *states* rather than waits for: the whole lifecycle suite runs
in microseconds because it never sleeps.

## The lifecycle goes one way

`Connecting` → `Connected` → `Disconnecting` → `Disconnected`. Nothing goes
backwards and a `Disconnected` link stays that way.

**A reconnect is a new `ConnectionId` with a new generation, never a revived
link.** A handle that can come back to life is a handle every caller must
re-check after every await, and that is the check nobody writes.

`Disconnecting` is not a formality. Skipping straight to `Disconnected` means a
peer that left politely is indistinguishable from one that crashed, and every
clean exit costs the other end a full idle timeout.

**The idle timeout applies while disconnecting too.** A peer that stops answering
mid-goodbye must not hold a slot open forever, and the graceful path is exactly
where that is easiest to forget.

**`CompleteHandshake` is not idempotent**, deliberately. A second completion is
either a replayed packet or two code paths both thinking they own the transition,
and quietly accepting it hides both.

## Unreliable is the default, and reliable is the exception

DATATYPES_LIBRARIES.md §15.1: *unreliable by default for state and reliable for
events.* A late position update is worse than a dropped one, because the next is
already on its way and is more correct than the one being waited for.

Making everything reliable is the mistake that turns one lost packet into a
visible stall for every player. Do not add a "reliable by default" convenience.

**The stale rule applies to unreliable traffic only.** A reliable packet arriving
late is a resend that still has to be delivered in order; discarding it would
silently drop an event the sender believes was acknowledged.

**Each channel has its own sequence counter, in both directions.** One shared
counter would let a reliable resend make an unreliable packet look stale — and
so would one shared *high-water mark* on the receiving side, which is the half
that was actually wrong until v0.4. `Link` keeps a window per channel and every
one wraps on its own: `Packet::IsNewer` is a half-range comparison and answers
nonsense when the two numbers come from different counters.

**A channel's first packet is accepted, whatever its sequence.** Zero is a
legitimate sequence — it is the first one `NextHeader` stamps — so no value can
stand for "nothing yet" and the window carries a flag instead. Treating zero as
"already seen sequence 0" reads a channel's opening packet as a repeat of one
that never existed, and counts every sequence below the one it opened at as
lost.

**A `Handshake` packet has no window and moves none.** It is answered before
there is a link to number it, so it belongs to no stream. It still proves the
peer is alive.

Because the acknowledgement fields report one sequence and there is now one
space per channel, `Link::NextHeader` acknowledges the channel it stamps.
Retiring a reliable payload needs an acknowledgement on *every* packet, and
that is `ReliableReceiver::Acknowledging`'s job — the two are the same root
cause, one window over two counters, seen from either end.

## Sequence comparison is wrap-aware, and this is not optional

A 16-bit counter wraps every 65536 packets — about eighteen minutes at sixty a
second, well inside one match. A plain `>` discards every packet for the
eighteen minutes after the first wrap, so a build that is fine in testing breaks
in a long game. `Packet::IsNewer` does the half-range comparison and every
sequence decision goes through it.

## Budgets are enforced here, not above

A limiter in userland runs **after** the payload has been received and parsed,
which is the half that costs. Bytes and packets are budgeted separately, because
a thousand one-byte packets cost almost no bandwidth and a great deal of
per-packet overhead at both ends.

`Reserve` asks and books in one call. A separate "may I" and "I did" is two calls
a caller can get out of step, and the one that gets forgotten is the second.

**Overflow is visible in `ConnectionStats::SendsOverBudget`** — §15.1 asks for
exactly that. A budget that silently drops traffic is indistinguishable from a
network that silently drops traffic, and the two want completely different fixes.

**A refusal is an answer, not a discard, and the caller has to read it.**
`Reserve` returning false means the payload did not go; whether that costs a
tick or costs a client is the caller's to know. `replication::Authority::Unsent`
is the one that knows — see `replication/AGENTS.md`. This module deliberately
keeps no outbox to retry from, because an outbox here would hold payloads whose
meaning it is not allowed to understand.

Budgets reset at the barrier with everything else per-tick. Resetting anywhere
else lets a connection spend two ticks' worth inside one.

## A challenge costs the responder nothing, and that is not an optimisation

`Cookie` derives its answer from a secret this end already holds plus the bytes
the peer already sent, and verifies it by deriving it again. **Never by looking
it up.** A table of pending challenges — even a bounded one, even an LRU — has
moved the exhaustion target rather than removed it: one datagram from a stranger
would buy an entry, and the whole reason the challenge exists is that a stranger
gets nothing.

So the rule for anything added here: an unanswered challenge costs **zero
bytes**, and it costs zero however many are outstanding. If a feature wants
per-peer state before the peer has answered, that feature belongs after the
answer.

**The reply is the same size as the question.** A responder that answered a
35-byte hello with something larger is a reflector somebody else's traffic can
be bounced off, and the amplification factor is the whole of what makes that
worth doing.

What a returned cookie proves is exactly one thing: somebody at that address
received a datagram this end sent there, recently. Not identity, not
authorisation. Deciding who may connect belongs above this module —
`replication::Listener::SetAdmission`.

## The stream is sealed, and the nonce discipline is structural

Every payload above the handshake is ChaCha20-Poly1305 with **the packet header
as associated data**, so a rewritten channel, sequence or acknowledgement fails
the tag rather than being acted on. The keys come out of `Handshake` and are
held for the life of the connection by `replication::Session` — one `Sealer` and
one `Opener` per direction, and holding them *is* the guarantee rather than a
convenience.

**A repeated nonce is the catastrophic failure and nothing here is allowed to
make one possible.** Two frames under one key and one nonce leak the XOR of
their plaintexts and hand over the material to forge tags. The three properties
that make it impossible are in `Cipher.hpp` and none of them may be weakened to
make plumbing convenient: the counter is private and only moves forward, a
`Sealer` is move-only and a moved-from one is poisoned, and **there is no
constructor from raw key material at all.** If holding one across a connection
wants it copyable, the plumbing is wrong.

**The counter is on the wire whole, in `PacketHeader::Counter`, and is not
derived from the sequence.** The sequence is the obvious candidate and it is the
wrong one: it is 16 bits and `IsNewer` exists precisely because it wraps every
eighteen minutes at sixty packets a second. A nonce derived from a wrapping
counter is a nonce that repeats. Eight bytes a packet is what that costs.

**A resend is sealed again under a fresh counter, never replayed verbatim.**
Both are safe against a repeat — a verbatim replay is the same frame rather than
a second one — and they fail differently. The header is the associated data and
it carries a live acknowledgement, so a verbatim replay would have to freeze the
acknowledgement on the one packet a stalled stream most needs current, or stop
covering the mutable header fields with the tag. `ReliableSender` therefore
holds **plaintext**, and `Session::Flush` seals it again.

**`Cipher` refuses a forgery and not a replay**, deliberately. A captured frame
sent again is authentic by construction; discarding it is the sequence window's
and `ReliableReceiver`'s job, because they are the layers that can tell an
attack from an ordinary resend. Nothing in `Opener` remembers a counter — a
dropped packet leaves a gap, a duplicate repeats one and a reorder lowers one,
and an opener with a window would refuse genuine traffic on all three.

**There is no downgrade because there is nothing to ask for.** No field on the
wire says whether a packet is sealed, so a peer can only send plaintext and be
refused. A `Session` with no keys sends nothing and accepts nothing.

**The tag comes out of the payload budget and the number to size against is
`Packet::MAXIMUM_MESSAGE_BYTES`, not `MAXIMUM_PAYLOAD_BYTES`.** `Link::Reserve`
measures against the first. A budget left on the second produces a message that
can never be sent and is indistinguishable at the call site from a busy link —
which is the failure this module has already been bitten by three times.

## Every field of an inbound packet is hostile

There is no trusted direction. `repo_layout.md` §1 says anyone can run a server,
so a client's packets and a server's are both attacker-controlled from the other
side.

`Packet::Read` refuses a wrong magic, an unknown version, a channel byte outside
the enum, a length over the maximum and a length running past the buffer — and
marks the reader failed. **A channel byte is range-checked before the cast**:
casting it anyway produces a `ChannelKind` no switch handles, and every
`Describe` and dispatch downstream then reads a value the type says cannot exist.

**A version mismatch is refused, never negotiated downward.** A server speaking
an old version to an old client is a server running two protocols, and the second
one is the one nobody tests.

An oversized payload is refused rather than fragmented. A fragmented datagram is
lost entirely when any one fragment is, which multiplies the loss rate the
unreliable channel is designed around.

## A `Link` does no I/O

It says what should be sent and records what was. A state machine that can also
do I/O is one that cannot be tested without doing I/O, and that is how a
lifecycle ends up only exercised by the real network.

That is also what makes `repo_layout.md` §16.6 honest: single-player rides a
loopback with **real encoding**, so there is no configuration in which this path
is skipped and no second lifecycle that only a socket exercises.

## A link that loses things is part of this module, and it is deterministic

`LossyTransport` wraps another `Transport` and discards some of what arrives at
it. It is here rather than in a suite because `net`, `replication` and
`mono.server` all need it, and this repository has no mechanism for a test-only
library shared between modules — a module's `tests/` may reach its own `src/`
and nothing else, so the alternatives were three copies or a fourth way to share
code.

**A wrapper, never a third implementation.** What it loses is real datagrams
through real framing over the loopback or a real socket. A third implementation
of the interface would be a third set of bugs, and the two cases only a routed
network produces would have to be built again.

**Loss is applied on arrival, not on the way out.** `Send` promises `Ok` means
the datagram left and distinguishes that from `Full`, `TooLarge`, `Unreachable`
and `Closed`; a wrapper deciding to lose one before offering it would have to
invent one of those statuses or hide one the transport underneath would have
given. So `Send` is pure delegation. Wrap the end that receives.

**No clock and no `std::random_device`, which is not negotiable here.** Whether
arrival *n* is lost is a pure function of *n* and a seed the caller states —
`core::Random` is indexed rather than streamed for exactly this reason. A
timeout in this module is something a suite states rather than waits for, and
loss is the same: a failing case is reproducible from its seed alone, and
nothing here can reach a recorded run.

**Nominating one datagram is worth more than a percentage**, and `DropNext` is
worth more than either — a test knows it has just made the server publish a
creation and does not know which arrival that will be.

## No vendor type in a public header

asio is `VENDOR`, never `VENDOR_PUBLIC`. No public header here names a socket, an
`io_context` or an `error_code`. That is what keeps a transport swappable and
what stops asio reaching every module that links this.

## Not here yet

- **The transports.** A loopback and an asio UDP socket, both driving `Link`.
- **Reliability.** The acknowledgement window is carried and recorded; nothing
  resends against it yet.
- **Binding the agreement to a server identity.** The stream is encrypted and
  the exchange is unauthenticated, and those are two different things: a peer
  knows it is talking to *something* that completed X25519, not that it is
  talking to this server. So the traffic is safe against a listener and not
  against a relay, which can hold one exchange with each side and read
  everything. `Handshake.hpp` carries the `TODO(D00006)`. A static server key and
  a signature over the transcript is the shape; where the key comes from and who
  trusts it is a deployment question.
- `upstream/`, `downstream/`, `predict/` — replication, v0.3's remaining items.
- `http/`, `websocket/` — userland networking and the origin's asset serving,
  which is what `mono.cdn`'s streaming waits on.
- **NAT traversal and relay**, a known gap: the transport encrypts, orders and
  rate-limits and has no answer for two peers that cannot see each other.
- **Interest management and lag compensation**, both later and both with their
  own plans.

## `http/` is a content protocol, not a web framework

It exists because a delivery group is megabytes and `Packet::MAXIMUM_PAYLOAD_BYTES`
is 1200. `Transport` carries datagrams for a simulation with a per-tick packet
budget; a content origin ships bulk bytes and wants ordering, retransmission and
flow control from the operating system. CDN.md §5.

**The subset is small and each omission closes something. Do not widen it
casually:**

- **`GET` and `HEAD` only.** An origin serves. Upload is `mono.cdn/control/`'s,
  in TypeScript, over its own API.
- **`Content-Length` framing only, and `Transfer-Encoding` is refused outright.**
  A body that can be framed two ways is request smuggling: two parsers in a chain
  disagree about where one message ends and the next begins, and the disagreement
  *is* the attack. Two `Content-Length` fields are refused **even when they
  agree**, because "even when they agree" is the check somebody eventually
  loosens.
- **No folded header lines, no bare `LF`, no absolute-form target.** Each is a
  spelling two implementations can read differently.

**Strict rather than forgiving is a security position, not a style.** Postel's
rule is the wrong one for a parser sitting behind a port.

**The protocol is split from the socket and that split is the design.**
`Message.hpp` is parsing and formatting over spans, so the whole wire format is
exercised by a suite that opens no port and waits for nothing. `Server` and
`Client` are the thin halves that own file descriptors.

**A response to `HEAD` carries a length and no body, and the reader has to be
told.** It is the one place the response format is not self-describing —
`ParseResponse` takes `bodyOmitted` for that reason, and a reader that believed
the length would wait for bytes nobody is sending.

**A response with no `Content-Length` is refused rather than read to
end-of-connection.** "Until the peer hangs up" is a framing an origin can use to
make a truncated group look complete, and the client would then hash short bytes
and report content corruption for what was a dropped socket.

### Polled, and completions land on the caller's thread

`Pump` drives everything — accepting, reading, dispatching, writing — on the
thread that called it. Nothing here calls `run()` or `poll()` on an
`io_context`, exactly as `UdpTransport` does not.

That is allowed to be *asynchronous* where `Transport` is not, and the reason is
CDN.md §3: **an origin has no tick.** Rule 5 governs work inside a tick, and a
request that completes a poll later changes nothing a recorded run would have to
reproduce. A datagram arriving on somebody else's thread mid-tick is a desync; a
byte of a group arriving between two polls is not.

**A handler runs inside `Pump` with every other connection waiting, so it must
not block.**

### The bounds are the security surface

A public port with no connection ceiling, no per-connection buffer bound and no
message size limit is a denial-of-service primitive with a friendly name. All
three are in `ServerSettings` and none is optional. Timeouts are counted in
*polls* rather than wall time, for this module's standing reason: time is passed
in, so a suite states a timeout instead of sleeping for one.
