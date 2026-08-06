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
// @tier L11 · shared

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

		// Payload bytes this connection may send per tick.
		uint32_t BytesPerTick = 64 * 1024;

		// Packets this connection may send per tick.
		//
		// Separate from the byte budget because they bound different things: a
		// thousand one-byte packets cost almost no bandwidth and a great deal of
		// per-packet overhead at both ends.
		uint32_t PacketsPerTick = 64;

		// Whether these can be used. Requires positive timeouts, a keep-alive
		// shorter than the idle timeout, and non-zero budgets.
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

		// What it has cost and lost.
		// Records the round trip whatever measured it.
		//
		// **A `Link` cannot measure this itself**, and that is why it is set
		// rather than computed: the acknowledgement that closes a round trip is
		// matched against a packet a `ReliableSender` is holding, and a link
		// holds none — it stamps sequences and counts what arrives. Whoever owns
		// both halves tells it.
		//
		// @param seconds The smoothed estimate. Ignored when negative.
		// @since v0.9
		void RecordRoundTrip(double seconds) {
			if (seconds >= 0.0) {
				Totals.RoundTripMilliseconds = static_cast<float>(seconds * 1000.0);
			}
		}

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
		//         will grow by — or a budget is spent. A refusal is counted in
		//         `ConnectionStats::SendsOverBudget`.
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

		ConnectionId Handle;
		LinkSettings Paced;
		ConnectionState Phase = ConnectionState::Connecting;
		DisconnectReason Ending = DisconnectReason::None;
		ConnectionStats Totals;

		double OpenedAt = 0.0;
		double LastReceiveAt = 0.0;
		double LastSendAt = 0.0;

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

		uint32_t BytesLeft = 0;
		uint32_t PacketsLeft = 0;
	};
}
