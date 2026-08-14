#pragma once

// One connection's life, with no socket anywhere in it.
//
// Join, leave, timeout and reconnect — v0.3's first roadmap item — expressed as
// a state machine over *events*, not over a transport. A datagram socket, a
// loopback and a test all drive the same object, which is what makes
// `repo_layout.md` §16.6 honest: single-player uses a loopback with real
// encoding, so there is no configuration in which this path is skipped and no
// second lifecycle that only the real network exercises.
//
// **Time is passed in, never read.** Every call that could care about "now"
// takes it as an argument. A wall clock read inside would put a
// non-deterministic input in the middle of the one subsystem whose failures are
// hardest to reproduce, and `ecs/AGENTS.md` already bans exactly that inside a
// system. It also makes a timeout something a test states rather than waits for.
//
// **Budgets are per tick and enforced here.** DATATYPES_LIBRARIES.md §15.1 asks
// for a byte budget per player per tick with the overflow visible in
// `ConnectionStats` rather than as a mystery stall. Enforcing it at the transport
// rather than in userland is the whole point: a limiter above this runs *after*
// the payload has been received and parsed, which is the half that costs.
//
// **There are two limits on a send and they answer different questions.**
// `BytesPerTick` is a cap a game states and it never moves; `CongestionControl`
// is what the path looks able to take and it moves every tick. A send has to
// pass both, and a refusal says which — `ConnectionStats::SendsOverBudget`
// against the configuration, `SendsOverAllowance` against the path. Collapsing
// them would make "raise the cap" look like a fix for congestion.
//
// @tier L11 · shared

#include <engine/net/Congestion.hpp>
#include <engine/net/ConnectionId.hpp>
#include <engine/net/ConnectionStats.hpp>
#include <engine/net/Enums.hpp>
#include <engine/net/Packet.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::net {

	// How a connection is paced and when it is given up on.
	//
	// The defaults are conventional rather than measured, and saying so is
	// better than implying otherwise — the same standing that `ChunkLimits` has.
	struct LinkSettings {
		// How long a handshake may take before the attempt is abandoned.
		double HandshakeTimeoutSeconds = 5.0;

		// How long a connection may hear nothing before it is presumed gone.
		//
		// Longer than the keep-alive by a wide margin on purpose: at a whisker
		// above it, one lost keep-alive drops a healthy connection.
		double IdleTimeoutSeconds = 10.0;

		// How often to send something when there is nothing to say.
		//
		// A connection with no traffic is indistinguishable from a dead one, and
		// a packet carrying only an acknowledgement is the cheapest way to tell
		// them apart.
		double KeepAliveSeconds = 1.0;

		// The most payload bytes this connection may send per tick, ever.
		//
		// **A ceiling the controller operates under, and no longer the only
		// limit.** It was kept rather than removed when congestion control
		// landed, because the two are answers to different questions: a game may
		// legitimately refuse to spend more than N on one player even on a path
		// that would carry ten times that — a hundred players on one host is a
		// hundred of these, and the operator's bill is not a function of what
		// the path can take. What it stopped being is a *rate*: it does not open
		// up on a fat path and it never did back off on a congested one, and
		// `CongestionControl` is the half that does both.
		uint32_t BytesPerTick = 64 * 1024;

		// Packets this connection may send per tick.
		//
		// Separate from the byte budget because they bound different things: a
		// thousand one-byte packets cost almost no bandwidth and a great deal of
		// per-packet overhead at both ends.
		//
		// **Left as a fixed cap deliberately.** The controller paces bytes,
		// because bytes are what a bottleneck queues; per-packet cost is a
		// property of the two endpoints rather than of the path between them, so
		// there is nothing on the wire for a controller to measure it against.
		uint32_t PacketsPerTick = 64;

		// How the send rate follows the path.
		//
		// @since v0.15
		CongestionSettings Congestion;

		// Whether these can be used. Requires positive timeouts, a keep-alive
		// shorter than the idle timeout, non-zero budgets, and congestion
		// settings that are themselves valid.
		bool IsValid() const;
	};

	// The lifecycle of one connection.
	//
	// Owned by whatever holds the transport. It never sends anything itself — it
	// says what *should* be sent and records what was — because a state machine
	// that can also do I/O is one that cannot be tested without doing I/O.
	//
	// @since v0.3
	class Link {
	  public:
		// Opens a link in `Connecting`.
		//
		// @param id The handle this connection answers to.
		// @param nowSeconds The current time.
		// @param settings How it is paced.
		Link(ConnectionId id, double nowSeconds, const LinkSettings &settings = {});

		// The handle this connection answers to.
		ConnectionId Id() const {
			return Handle;
		}

		// Where it is in its life.
		ConnectionState State() const {
			return Phase;
		}

		// Why it ended, or `None` while it has not.
		DisconnectReason Reason() const {
			return Ending;
		}

		// Records the round trip whatever measured it.
		//
		// **A `Link` cannot measure this itself**, and that is why it is set
		// rather than computed: the acknowledgement that closes a round trip is
		// matched against a packet a `ReliableSender` is holding, and a link
		// holds none — it stamps sequences and counts what arrives. Whoever owns
		// both halves tells it.
		//
		// **This is also the congestion controller's delay signal**, which is
		// why it is no longer just a field being set. Nothing extra crosses the
		// wire to feed it: it is the same estimate `ConnectionStats` has
		// reported since v0.9, arriving at the same call.
		//
		// **No time argument, which is the one departure from this module's rule
		// and is deliberate.** A round trip is recorded immediately after the
		// packet that closed it was handed to `OnPacket`, so the link was told
		// what time it is a moment ago and asking the caller again would be
		// asking for a number it has already given. The controller is stamped
		// with the last time this link was told, which is that one.
		//
		// @param seconds The smoothed estimate. Ignored when negative.
		// @param varianceSeconds The estimate's variance —
		//        `ReliableSender::RoundTripVarianceSeconds`. Negative is read as
		//        unknown, and the controller's noise threshold then falls back
		//        to `CongestionSettings::MinimumQueueSeconds` alone.
		// @since v0.9
		void RecordRoundTrip(double seconds, double varianceSeconds = -1.0);

		// How the send rate is following the path.
		//
		// @return The controller, valid for the life of the link.
		// @since v0.15
		const CongestionControl &Congestion() const {
			return Control;
		}

		// What this link has carried and what it has refused.
		//
		// **Read this before concluding a component is not replicating.**
		// `SendsOverBudget` off zero means the link turned messages away, which
		// looks from the outside exactly like a sender that never sent them.
		//
		// @return The running totals, valid until the next pump.
		const ConnectionStats &Stats() const {
			return Totals;
		}

		// The settings in use, after the validity fallback.
		const LinkSettings &Settings() const {
			return Paced;
		}

		// Moves a `Connecting` link to `Connected`.
		//
		// @param nowSeconds The current time.
		// @return False when the link was not `Connecting` — a handshake that
		//         completes twice is a protocol error, not an idempotent call.
		bool CompleteHandshake(double nowSeconds);

		// Begins a graceful close.
		//
		// Moves to `Disconnecting` rather than straight to `Disconnected`, so a
		// goodbye can reach the far side. Without that step a peer that left
		// politely is indistinguishable from one that crashed, and every clean
		// exit costs the other end a full idle timeout.
		//
		// @param reason Why. `None` is refused — an ending always has one.
		// @return False when already ending or ended.
		bool Disconnect(DisconnectReason reason);

		// Completes a close, from either side.
		//
		// @param reason Why, when the link was not already `Disconnecting`.
		void Close(DisconnectReason reason);

		// Advances time: timeouts, and the keep-alive clock.
		//
		// Call once per tick, before deciding what to send.
		//
		// @param nowSeconds The current time.
		void Advance(double nowSeconds);

		// Records a packet that arrived and says whether to act on it.
		//
		// Applies the sequence rule: an unreliable packet older than one already
		// seen **on its own channel** is counted as stale and refused, because
		// applying it would move the world backwards.
		//
		// **The high-water mark it is judged against is that channel's.** The
		// counters are per channel, so one mark for the link lets a reliable
		// resend drag it past the unreliable stream and the next unreliable
		// packet is then thrown away for being behind a number it was never
		// counting against.
		//
		// A `Handshake` packet has no mark and moves none. It is answered
		// before there is a link to number it, so it belongs to no stream.
		//
		// @param header The packet's header.
		// @param payloadBytes How much payload it carried.
		// @param nowSeconds The current time.
		// @return Whether the payload should be acted on.
		bool OnPacket(const PacketHeader &header, size_t payloadBytes, double nowSeconds);

		// Whether `payloadBytes` may be sent this tick, and books it if so.
		//
		// **Asks and books together, on purpose.** A separate "may I" and "I
		// did" is two calls a caller can get out of step, and the one that gets
		// forgotten is the second.
		//
		// @param payloadBytes The payload about to be sent, before it is sealed.
		// @return False when the link is not `Connected`, the payload is over
		//         `Packet::MAXIMUM_MESSAGE_BYTES` — the limit less the tag it
		//         will grow by — or a budget is spent, whether the configured
		//         one or the one the congestion controller allows.
		//         `ConnectionStats::SendsOverBudget` counts the first and
		//         `SendsOverAllowance` the second. **A caller that must not lose
		//         the send reads both.**
		bool Reserve(size_t payloadBytes);

		// Stamps the header for the next outgoing packet on a channel.
		//
		// Advances that channel's sequence and folds in the acknowledgement the
		// far side is owed, so an acknowledgement never needs a packet of its
		// own.
		//
		// **The acknowledgement is of the same channel this packet goes on**,
		// because there is one sequence space per channel and one field to say
		// which sequence arrived. A caller that needs the reliable stream
		// acknowledged by every packet whatever its channel — which is what
		// retires a reliable payload on a mostly one-way conversation —
		// overwrites these two fields with `ReliableReceiver::Acknowledging`.
		//
		// @param channel Which channel the packet belongs to.
		// @return The header to write.
		PacketHeader NextHeader(ChannelKind channel);

		// Returns the per-tick budgets to full.
		//
		// Called at the barrier, with everything else that is per tick. A budget
		// reset at some other point would let a connection spend two ticks'
		// worth inside one.
		void ResetBudget();

		// Whether nothing has been sent for `KeepAliveSeconds`.
		//
		// @param nowSeconds The current time.
		// @return Whether to send a packet carrying only an acknowledgement.
		bool NeedsKeepAlive(double nowSeconds) const;

		// Records that a packet was sent, for the keep-alive clock and the
		// totals.
		//
		// @param payloadBytes The payload that went.
		// @param nowSeconds The current time.
		void OnSent(size_t payloadBytes, double nowSeconds);

	  private:
		// What the far side has sent on one channel, and what went missing.
		//
		// One of these per channel because the sequences are per channel. A
		// single mark for the whole link is judged with `Packet::IsNewer`
		// against whichever channel most recently moved it, and the reliable
		// channel moves it a long way at a join — so the first unreliable
		// packets after one were refused as stale, which is exactly the failure
		// the per-channel counters exist to prevent.
		struct ChannelWindow {
			// The highest sequence seen on this channel.
			//
			// Wrap-compared against this channel's traffic and no other's:
			// `IsNewer` is a half-range comparison and answers nonsense when
			// the two numbers come from different counters.
			uint16_t Highest = 0;

			// The 32 sequences before `Highest`, one bit each.
			uint32_t Bits = 0;

			// Whether anything has arrived on this channel at all.
			//
			// **A flag rather than a sentinel value, and that is the off-by-one
			// this avoids.** All 65536 sequences are legitimate — zero most of
			// all, since it is the first one `NextHeader` stamps — so there is
			// no number that can mean "nothing yet". Starting `Highest` at zero
			// and trusting it would read a channel's first packet as a repeat
			// of one that never existed, and would count `Sequence` packets
			// lost for a stream that opens anywhere else.
			bool Seen = false;
		};

		void Observe(ChannelWindow &window, uint16_t sequence);
		void ObserveAcknowledgement(const PacketHeader &header);

		ConnectionId Handle;
		LinkSettings Paced;
		ConnectionState Phase = ConnectionState::Connecting;
		DisconnectReason Ending = DisconnectReason::None;
		ConnectionStats Totals;

		// The send rate as a function of the path, under `BytesPerTick`.
		CongestionControl Control;

		double OpenedAt = 0.0;
		double LastReceiveAt = 0.0;
		double LastSendAt = 0.0;

		// The last time anybody told this link what time it is, from either
		// `Advance` or `OnPacket`. What the controller's observations are
		// stamped with.
		double LastKnownSeconds = 0.0;

		// The last time `Advance` was called, and nothing else.
		//
		// **Separate from the above, and the separation is load-bearing.** A
		// tick's length is the gap between two advances; measuring it against
		// the last time *anything* named a time gives zero, because the packets
		// that arrived earlier in the tick named the same instant. That reads as
		// a stalled tick and clamps the allowance to almost nothing.
		double LastAdvanceAt = 0.0;
		bool Ticked = false;

		// One counter per channel, because they are ordered independently — a
		// reliable resend must not make an unreliable packet look stale.
		//
		// Sized from the enum rather than from a literal. The handshake slot is
		// never advanced, because a handshake datagram is sent before there is a
		// link to number it; it is here so that indexing by channel cannot run
		// off the end the day another one is added.
		uint16_t OutgoingSequence[static_cast<size_t>(ChannelKind::Handshake) + 1]{};

		// What has arrived, one window per channel — the receiving mirror of
		// the counters above, and sized from the enum for the same reason.
		//
		// The handshake slot is never touched, because a handshake datagram
		// carries no sequence anybody assigned. It is here so that indexing by
		// channel cannot run off the end.
		ChannelWindow Incoming[static_cast<size_t>(ChannelKind::Handshake) + 1];

		// Where the far side's acknowledgement of *this* end's reliable stream
		// has been judged up to.
		//
		// **The outbound twin of `ChannelWindow`, and the reason it is here
		// rather than in `ReliableSender` is that a `Link` may not have one.**
		// Reliability is a layer above and is optional; congestion control is
		// not, and both read the same two header fields the far side already
		// sends. `ReliableSender` reads them to retire payloads; this reads them
		// to decide whether the path dropped something. One acknowledgement,
		// two questions, no second ack path.
		uint16_t JudgedSequence = 0;
		bool ReliableStreamOpen = false;

		// The reliable sequence whose acknowledgement closes the controller's
		// current observation period.
		//
		// Re-armed to whatever is being sent now each time the frontier reaches
		// it, so "the period is over" means "everything outstanding when it
		// opened has been answered" — which is one round trip, measured rather
		// than assumed, on a loopback and on a satellite alike.
		uint16_t PeriodSequence = 0;

		uint32_t BytesLeft = 0;
		uint32_t PacketsLeft = 0;

		// What the controller allows this tick, spent alongside `BytesLeft`.
		// Kept apart so that a refusal can say which of the two turned it away.
		uint32_t AllowanceLeft = 0;
	};
}
