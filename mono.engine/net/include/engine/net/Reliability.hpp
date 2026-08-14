#pragma once

// The half of reliability that resends, and the half that reorders.
//
// `Link` stamps an acknowledgement onto every outgoing packet and records every
// one that arrives. Nothing acts on it: a lost reliable packet is noticed and
// then forgotten. This is what acts on it — the sender holds a payload until the
// far side says it arrived, and the receiver holds a payload that arrived early
// until the gap in front of it is filled.
//
// **Unreliable is still the default, and nothing here changes that.** Only a
// payload the caller has already decided is `ChannelKind::Reliable` reaches
// these types, and there is deliberately no convenience that makes a payload
// reliable for you. A late position update is worse than a dropped one, because
// the next one is already on its way and is more correct than the one being
// waited for.
//
// **The acknowledgement is about the reliable channel alone, and it has to ride
// every packet whatever channel that packet is on.** `Link` keeps a window per
// channel — it has to, or a reliable resend makes an unreliable packet look
// stale — and `Link::NextHeader` can therefore only report the window of the
// channel it is stamping. A game is mostly one-way and mostly unreliable, so a
// reliable stream acknowledged only by reliable traffic coming back would hardly
// be acknowledged at all, and the sender would resend payloads that arrived.
// `ReliableReceiver::Acknowledging` therefore rewrites the two acknowledgement
// fields of every outgoing header, and `ReliableSender` reads them back. Nothing
// in `Link` consumes those fields, so nothing is lost by it.
//
// **This does no I/O, for the same reason `Link` does not.** It says what should
// be sent again; something above it sends. That is also why a resend is not
// exempt from the per-tick budget: the caller offers each due payload to
// `Link::Reserve` exactly as it would a fresh one, and a refusal is counted in
// `ConnectionStats::SendsOverBudget` like any other.
//
// **Time is passed in, never read.** A retransmit timeout is something a suite
// states rather than waits for, which is why the whole reliability suite runs in
// microseconds without sleeping.
//
// **Both queues are bounded, and that is the security property.** An
// unacknowledged queue with no bound is a memory exhaustion attack from a peer
// that simply stops acknowledging, and an out-of-order queue with no bound is
// the same attack from a peer that sends a stream with a hole it never fills.
//
// @tier L11 · shared

#include <engine/net/Enums.hpp>
#include <engine/net/Packet.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::net {

	// How long a reliable payload waits, how many may wait, and when a peer that
	// is not acknowledging is given up on.
	//
	// The defaults are conventional rather than measured, and saying so is
	// better than implying otherwise — the same standing `LinkSettings` has. The
	// one exception is `MaximumUnacknowledged`, which is derived rather than
	// chosen and says so.
	//
	// @since v0.3
	struct ReliabilitySettings {
		// How long to wait for an acknowledgement before sending it again.
		//
		// A hundred milliseconds is six ticks at sixty a second: long enough
		// that an acknowledgement riding the far side's next packet normally
		// beats it, short enough that one lost event is not a visible stall.
		// `ConnectionStats::RoundTripMilliseconds` is where a derived figure
		// would come from once something measures one.
		double RetransmitTimeoutSeconds = 0.1;

		// Sequences the sender may run ahead of its oldest unacknowledged
		// payload, and therefore payloads it may be holding at once.
		//
		// **32 is derived, not chosen, and 32 is the ceiling.** A resend goes
		// out under the sequence it was first sent with, and the window that
		// acknowledges it covers `Acknowledge` and the 32 sequences before it.
		// Letting the sender run further ahead than that puts its oldest
		// unacknowledged payload outside the only window that can ever
		// acknowledge it, and it would then be resent for the life of the
		// connection. `IsValid` refuses anything larger.
		//
		// It is also the memory bound: 32 of the largest payload is 38 KB per
		// connection per direction.
		size_t MaximumUnacknowledged = 32;

		// Attempts on one payload before the far side is presumed gone.
		//
		// 32 at the default timeout is a payload that has gone unacknowledged
		// for three seconds while the connection was otherwise alive enough to
		// stay out of the idle timeout. That is a peer that is not
		// acknowledging, and holding its backlog longer is holding it forever.
		uint32_t MaximumResends = 32;

		// Reliable payloads that may be held waiting for a gap ahead of them.
		//
		// The receive-side twin of `MaximumUnacknowledged`, and the same
		// arithmetic. A peer that respects its own send window can never reach
		// it; a peer that sends 1..n and never sends 0 is asking for unbounded
		// storage, and that is the one this is here for.
		size_t MaximumOutOfOrder = 32;

		// Whether these can be used. Requires a positive timeout, non-zero
		// bounds, and an unacknowledged window no wider than the 32 sequences
		// `PacketHeader::AcknowledgeBits` covers. Anything else falls back to
		// the defaults, as `LinkSettings` does.
		bool IsValid() const;
	};

	// Holds reliable payloads until the far side acknowledges them.
	//
	// One per connection per direction. It does not assign sequences —
	// `Link::NextHeader` owns the per-channel counters, because a reliable
	// resend must not make an unreliable packet look stale — so a caller stamps
	// a header, sends it, and hands the same sequence here.
	//
	// **Track every reliable packet, in the order the sequences were stamped.**
	// The window arithmetic assumes the payloads waiting are a consecutive run
	// of that channel's sequences, which is what makes a resend certain to fall
	// inside the far side's acknowledgement window.
	//
	// @since v0.3
	class ReliableSender {
	  public:
		// One payload still waiting, offered back for another send.
		struct Unacknowledged {
			// The sequence it first went out with, and the one it must go out
			// with again.
			//
			// **A resend reuses its original sequence and must not consume a
			// new one from `Link::NextHeader`.** A hole in the reliable stream
			// is a hole the receiver's ordering waits on until the idle
			// timeout, so a resend under a fresh sequence would stall exactly
			// the stream it was trying to repair.
			uint16_t Sequence = 0;

			// The payload, as a view **into the sender's own storage**.
			//
			// Not copied. Valid until the next call that adds or retires an
			// entry — `Track` or `OnAcknowledge` — which outlasts the send loop
			// `Due` exists to feed.
			std::span<const std::byte> Payload;
		};

		// @param settings How long a payload waits and how many may wait.
		explicit ReliableSender(const ReliabilitySettings &settings = {});

		// The settings in use, after the validity fallback.
		const ReliabilitySettings &Settings() const {
			return Paced;
		}

		// Why this gave up on the far side, or `None` while it has not.
		//
		// `TimedOut` once one payload has been sent `MaximumResends` times
		// without being acknowledged. Every one of those attempts was inside
		// the far side's acknowledgement window, so a peer that answered none
		// of them is gone or is choosing not to answer, and either way its
		// backlog is memory this side is holding on its behalf. Close the link
		// with it.
		//
		// **Not the same thing as a full window.** A full window is
		// backpressure and says nothing about the peer; see `HasRoom`.
		DisconnectReason Overflow() const {
			return Overflowed;
		}

		// Payloads waiting for an acknowledgement.
		size_t Waiting() const {
			return Pending.size();
		}

		// Payloads confirmed sent again by `OnResent`, over this sender's life.
		uint64_t Retransmissions() const {
			return Resends;
		}

		// Whether another reliable payload may be sent yet.
		//
		// **Ask before stamping a header, not after.** This is the one place
		// the module's ask-and-book-together rule does not apply, and
		// deliberately: `Track` cannot book anything until a sequence exists,
		// and a sequence stamped for a packet that is then not sent is a hole
		// the receiver waits on. So the question has to come first, and the
		// answer is a reason to leave the payload in the caller's outbox for a
		// tick rather than a reason to drop it.
		//
		// **The window is over sequences, not over entries.** Payloads retire
		// out of order — a lost one is acknowledged long after everything
		// behind it — so counting what is waiting would let the newest run
		// arbitrarily far ahead of one straggler, and put that straggler
		// outside the only window that can ever acknowledge it.
		bool HasRoom() const;

		// Holds a reliable payload that has just been sent.
		//
		// @param sequence The sequence the packet went out with, from
		//        `Link::NextHeader`.
		// @param payload The payload, copied — it has to outlive the send.
		// @param nowSeconds The current time, which starts its retransmit
		//        clock.
		// @return False when `HasRoom` was false, in which case nothing is
		//         held and the packet should not have been sent.
		bool Track(uint16_t sequence, std::span<const std::byte> payload, double nowSeconds);

		// Retires everything an arriving header acknowledges.
		//
		// Both halves of the window: `Acknowledge` itself, and each of the 32
		// sequences before it whose bit is set in `AcknowledgeBits`. So one
		// packet can retire 33, and a single lost acknowledgement does not
		// force a resend of something that did in fact arrive.
		//
		// @param header The header that arrived, with the acknowledgement the
		//        far side's `ReliableReceiver::Acknowledging` put on it. Its
		//        channel does not matter — the acknowledgement rides every
		//        packet, which is what keeps a mostly one-way conversation from
		//        needing packets of its own.
		// @param nowSeconds The current time, which every retired entry that
		//        has never been resent becomes a round-trip sample against.
		// @return How many payloads were retired.
		size_t OnAcknowledge(const PacketHeader &header, double nowSeconds);

		// The smoothed round trip, in seconds, or zero before the first sample.
		//
		// **Smoothed rather than instantaneous**, because one sample includes
		// whatever the far side happened to be doing and a number that jumps by
		// forty milliseconds between two reads is one no interface can show.
		// The weight is RFC 6298's one eighth, which is what every TCP has used
		// for thirty years and is a better default than a number invented here.
		//
		// **Only packets that were never resent are measured**, which is Karn's
		// rule and is not optional: an acknowledgement of a resent packet does
		// not say *which* transmission it answers, so a sample from one is
		// either the true trip or the trip plus a retransmit timeout, and there
		// is no way to tell. Measuring them makes the estimate worst on exactly
		// the links that need it most.
		//
		// @return The estimate in seconds. Zero means nothing has been measured
		//         yet, which a caller should read as "unknown" rather than as
		//         "instant".
		// @since v0.9
		double SmoothedRoundTripSeconds() const {
			return SmoothedRoundTrip;
		}

		// How much the round trip moves about, in seconds, or zero before the
		// first sample.
		//
		// RFC 6298's `RTTVAR`: the smoothed mean deviation of a sample from the
		// estimate, at a weight of one quarter. It is the half of the estimator
		// that says how much to trust the other half.
		//
		// **A congestion controller needs both and a single last-sample is no
		// substitute for either.** Queueing delay is read as the round trip
		// rising above its own floor, and on a wireless link the trip rises and
		// falls by tens of milliseconds with nothing queued anywhere — so a
		// controller with no variance to compare against reads jitter as
		// congestion and backs off for ever. `CongestionSettings::VarianceFactor`
		// is what consumes this.
		//
		// @return The variance in seconds. Zero means nothing has been measured.
		// @since v0.15
		double RoundTripVarianceSeconds() const {
			return RoundTripVariance;
		}

		// What is due to be sent again, oldest first.
		//
		// **Offer each one to `Link::Reserve` before sending it.** A resend is
		// not exempt from the per-tick budget; when the budget refuses one, do
		// not call `OnResent` for it. Its retransmit clock then does not
		// restart, it is still held, and it comes back from the next `Due` —
		// while the refusal itself is already visible in
		// `ConnectionStats::SendsOverBudget`.
		//
		// @param nowSeconds The current time.
		// @return A view into this sender's own buffer, valid until the next
		//         call to `Due`.
		std::span<const Unacknowledged> Due(double nowSeconds);

		// Records that a due payload went out again.
		//
		// @param sequence The sequence it went out with, unchanged.
		// @param nowSeconds The current time, which restarts its retransmit
		//        clock.
		// @return False when nothing is waiting under that sequence, which
		//         means it was acknowledged between `Due` and the send.
		bool OnResent(uint16_t sequence, double nowSeconds);

	  private:
		// One payload, the clock it is measured against, and how many times it
		// has been asked for.
		struct Held {
			uint16_t Sequence = 0;
			uint32_t Attempts = 0;
			double SentAtSeconds = 0.0;
			std::vector<std::byte> Payload;
		};

		// The smoothed round trip in seconds, or zero before the first sample.
		double SmoothedRoundTrip = 0.0;

		// The smoothed mean deviation of a sample from that estimate.
		double RoundTripVariance = 0.0;

		ReliabilitySettings Paced;
		DisconnectReason Overflowed = DisconnectReason::None;
		uint64_t Resends = 0;

		// The newest sequence handed to Track, which is not the newest entry
		// still waiting once anything has retired out of order.
		uint16_t Newest = 0;

		// In send order, which is also the order a receiver needs the gaps
		// filled in: everything behind a hole waits on the hole, so the oldest
		// resend is the one that unblocks the most. Bounded at 32, so the
		// linear scans over it are over a handful of entries.
		std::vector<Held> Pending;

		// Reused rather than returned by value: Due runs per connection per
		// tick, and a vector per call is a per-frame allocation for nothing.
		std::vector<Unacknowledged> Ready;
	};

	// Releases reliable payloads in the order they were sent, and acknowledges
	// them.
	//
	// A reliable packet that arrives early is held rather than delivered, and a
	// resend of one already delivered is dropped rather than delivered twice.
	// Neither is the stale rule: that applies to unreliable traffic alone, and
	// `Link::OnPacket` deliberately lets a late reliable packet through so it
	// can reach this.
	//
	// @since v0.3
	class ReliableReceiver {
	  public:
		// One payload, in order and ready to be acted on.
		struct Delivery {
			// The sequence it was sent with.
			uint16_t Sequence = 0;

			// The payload, owned. A copy is unavoidable here: a payload that
			// arrives early is acted on some ticks after the buffer it came in
			// was reused.
			std::vector<std::byte> Payload;
		};

		// @param settings How many payloads may be held at once.
		// @param firstSequence The sequence the stream begins at. Zero for a
		//        `Link`, whose channel counters start there.
		explicit ReliableReceiver(const ReliabilitySettings &settings = {}, uint16_t firstSequence = 0);

		// The settings in use, after the validity fallback.
		const ReliabilitySettings &Settings() const {
			return Paced;
		}

		// Why this stopped accepting, or `None` while it has not.
		//
		// `BudgetExceeded` once `MaximumOutOfOrder` payloads are held. A peer
		// respecting its own send window cannot reach it, so a peer that does
		// is spending this side's memory on its own behalf without ever
		// exceeding a per-tick budget. Close the link with it.
		DisconnectReason Overflow() const {
			return Overflowed;
		}

		// Payloads held waiting for a gap ahead of them.
		size_t Holding() const {
			return Held.size();
		}

		// Payloads dropped because they had already been delivered or held.
		//
		// Expected rather than alarming: it is what a resend looks like when
		// the acknowledgement for the original was the packet that got lost.
		uint64_t Duplicates() const {
			return Repeats;
		}

		// The sequence the next delivery will carry.
		uint16_t Expecting() const {
			return NextSequence;
		}

		// Fills in the acknowledgement fields of an outgoing header.
		//
		// **Call this on every outgoing header, whatever its channel.** The
		// acknowledgement is about the reliable stream — see the file comment
		// for why `Link`'s shared window cannot be — and a game is mostly
		// one-way, so a reliable stream acknowledged only by reliable traffic
		// going the other way would hardly be acknowledged at all.
		//
		// @param header The header, as stamped by `Link::NextHeader`.
		// @return The same header, acknowledging what has arrived here.
		PacketHeader Acknowledging(PacketHeader header) const;

		// Takes an arriving reliable payload.
		//
		// The acknowledgement is updated even when the payload is refused: a
		// duplicate is a resend, and the far side resends precisely because it
		// has not heard that the original arrived.
		//
		// @param sequence The packet's sequence.
		// @param payload The payload, copied — it may be held for some ticks.
		// @return False when it was a duplicate, or when the bound has been
		//         reached and `Overflow` names the reason. Either way nothing
		//         was held.
		bool Accept(uint16_t sequence, std::span<const std::byte> payload);

		// Everything now deliverable, oldest first.
		//
		// Empty while the gap ahead is unfilled, however much is queued behind
		// it — which is the whole promise, and the reason the sender resends.
		//
		// @return A view into this receiver's own buffer, valid until the next
		//         call to `Drain`.
		std::span<const Delivery> Drain();

	  private:
		void Record(uint16_t sequence);

		ReliabilitySettings Paced;
		DisconnectReason Overflowed = DisconnectReason::None;
		uint64_t Repeats = 0;

		// Wraps with the sequence it tracks, which is what makes the release
		// loop in Drain correct across 65536 without a special case.
		uint16_t NextSequence = 0;

		// The acknowledgement window over the reliable stream: the highest
		// sequence seen and the 32 before it.
		//
		// Starts one *behind* the first sequence of the stream, so that a
		// receiver which has heard nothing acknowledges nothing. Starting at
		// zero would acknowledge sequence zero — the first reliable payload a
		// `Link` ever sends — before it had arrived, and that payload would
		// then never be resent.
		uint16_t Highest = 0;
		uint32_t Bits = 0;

		// Keyed by sequence rather than kept sorted: the release loop asks for
		// one specific sequence at a time, so ordering the container would buy
		// nothing and would need a wrap-aware comparator to be correct at all.
		std::unordered_map<uint16_t, std::vector<std::byte>> Held;

		// Reused between calls, as the sender's is.
		std::vector<Delivery> Ready;
	};
}
