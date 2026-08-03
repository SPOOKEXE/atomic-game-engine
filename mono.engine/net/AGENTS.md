# net — module invariants

L11 transport, `shared` tier. Connection lifecycle, framing and channels — the
layer `upstream/`, `downstream/`, `predict/` and `http/` all sit on.

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

**Each channel has its own sequence counter.** One shared counter would let a
reliable resend make an unreliable packet look stale.

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

## No vendor type in a public header

asio is `VENDOR`, never `VENDOR_PUBLIC`. No public header here names a socket, an
`io_context` or an `error_code`. That is what keeps a transport swappable and
what stops asio reaching every module that links this.

## Not here yet

- **The transports.** A loopback and an asio UDP socket, both driving `Link`.
- **Reliability.** The acknowledgement window is carried and recorded; nothing
  resends against it yet.
- **Encryption of the stream.** The agreement is wired in and used —
  `replication`'s admission exchange runs `Handshake` and proves the derived
  keys agree — but **the traffic after it is still in the clear.** The two
  `Cipher` halves confirm the exchange and are then destroyed, deliberately: a
  `Sealer` kept somewhere that nothing seals with reads as though the wire were
  protected. Sealing every payload means a counter on the wire, the header as
  associated data, and the tag coming out of `MAXIMUM_PAYLOAD_BYTES`. That is a
  wire format change and it is its own piece of work.
- `upstream/`, `downstream/`, `predict/` — replication, v0.3's remaining items.
- `http/`, `websocket/` — userland networking and the origin's asset serving,
  which is what `mono.cdn`'s streaming waits on.
- **NAT traversal and relay**, a known gap: the transport encrypts, orders and
  rate-limits and has no answer for two peers that cannot see each other.
- **Interest management and lag compensation**, both later and both with their
  own plans.
