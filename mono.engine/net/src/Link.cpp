#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Link.hpp>

#include <algorithm>

namespace engine::net {

	namespace {
		size_t ChannelSlot(ChannelKind channel) {
			// The enum's own order, and the array below is sized from the last
			// value rather than from a literal - a channel added without a slot
			// would index past the end, which is the one bug in this file that
			// would not show up as a wrong number.
			return static_cast<size_t>(channel);
		}
	}

	bool LinkSettings::IsValid() const {
		return HandshakeTimeoutSeconds > 0.0 && IdleTimeoutSeconds > 0.0 && KeepAliveSeconds > 0.0 &&
			   KeepAliveSeconds < IdleTimeoutSeconds && BytesPerTick > 0 && PacketsPerTick > 0 &&
			   Congestion.IsValid();
	}

	Link::Link(ConnectionId id, double nowSeconds, const LinkSettings &settings)
		: Handle(id), Paced(settings.IsValid() ? settings : LinkSettings{}), Control(Paced.Congestion),
		  OpenedAt(nowSeconds), LastReceiveAt(nowSeconds), LastSendAt(nowSeconds),
		  LastKnownSeconds(nowSeconds), LastAdvanceAt(nowSeconds) {
		ResetBudget();
	}

	bool Link::CompleteHandshake(double nowSeconds) {
		if (Phase != ConnectionState::Connecting) {
			// Not idempotent, deliberately. A handshake that completes twice is
			// either a replayed packet or two code paths both thinking they own
			// the transition, and quietly accepting the second hides both.
			return false;
		}

		Phase = ConnectionState::Connected;
		LastReceiveAt = nowSeconds;
		LastSendAt = nowSeconds;
		core::Metrics::Count("net.link.connected", 1.0);
		ENGINE_INFO("link connected");
		return true;
	}

	bool Link::Disconnect(DisconnectReason reason) {
		if (reason == DisconnectReason::None) {
			// An ending always has one. Allowing `None` would let a closed link
			// read as one that never ended.
			return false;
		}
		if (Phase == ConnectionState::Disconnecting || Phase == ConnectionState::Disconnected) {
			return false;
		}

		Phase = ConnectionState::Disconnecting;
		Ending = reason;

		// The one lifecycle edge that had neither a counter nor a line, and the
		// one carrying the reason a player's session ended.
		ENGINE_INFO("link disconnecting: {}", Describe(reason));
		return true;
	}

	void Link::Close(DisconnectReason reason) {
		if (Phase == ConnectionState::Disconnected) {
			return;
		}

		// A reason already decided by Disconnect wins. The caller completing the
		// close is reporting that it finished, not re-deciding why it started.
		if (Ending == DisconnectReason::None) {
			Ending = reason == DisconnectReason::None ? DisconnectReason::Requested : reason;
		}

		Phase = ConnectionState::Disconnected;
		core::Metrics::Count("net.link.closed", 1.0);
		ENGINE_INFO("link closed: {}", Describe(Ending));
	}

	void Link::Advance(double nowSeconds) {
		if (Phase == ConnectionState::Disconnected) {
			return;
		}

		// **The one place the control law runs, and that is rule 5.**
		// Observations arrive whenever a packet does, which may be several times
		// in a tick; they accumulate into windowed minima, which do not care
		// what order they arrive in. The decision that turns them into a window
		// happens here, once, at the point in the tick every caller already
		// advances this link's timeouts - and before anything asks `Reserve`
		// what it may send, which is the order the header states.
		//
		// A tick's length is measured rather than configured because the tick
		// rate is not this module's to know: a rate needs seconds and the only
		// seconds available are the ones the caller keeps handing in. **Measured
		// between two advances and nothing else** - the packets that arrived
		// earlier in this tick named the same instant, so measuring against the
		// last time anybody named a time reads every tick as a stall. The very
		// first call has nothing to measure against at all, so the initial
		// window the controller opened with stands for exactly one tick.
		//
		// The period the controller measures against closes when the far side
		// has acknowledged everything that was outstanding when it opened. That
		// is one round trip by observation rather than by assumption, and it is
		// what keeps the ramp honest on a path whose trip is shorter than a
		// tick.
		bool answered = false;
		if (ReliableStreamOpen && !Packet::IsNewer(PeriodSequence, JudgedSequence)) {
			answered = true;
			PeriodSequence = OutgoingSequence[ChannelSlot(ChannelKind::Reliable)];
		}

		if (Ticked) {
			Control.Advance(nowSeconds - LastAdvanceAt, Paced.BytesPerTick, answered);
		}
		Ticked = true;
		LastAdvanceAt = nowSeconds;
		LastKnownSeconds = nowSeconds;

		Totals.QueueMilliseconds = static_cast<float>(Control.QueueSeconds() * 1000.0);

		Totals.SinceLastReceiveSeconds = static_cast<float>(nowSeconds - LastReceiveAt);

		if (Phase == ConnectionState::Connecting) {
			if (nowSeconds - OpenedAt >= Paced.HandshakeTimeoutSeconds) {
				Close(DisconnectReason::HandshakeFailed);
				core::Metrics::Count("net.link.handshake.failed", 1.0);
			}
			return;
		}

		// The idle timeout applies while disconnecting too. A peer that stopped
		// answering mid-goodbye must not hold a slot open forever, and the
		// graceful path is exactly where it is easiest to forget that.
		if (nowSeconds - LastReceiveAt >= Paced.IdleTimeoutSeconds) {
			Close(DisconnectReason::TimedOut);
			core::Metrics::Count("net.link.timedout", 1.0);
		}
	}

	void Link::Observe(ChannelWindow &window, uint16_t sequence) {
		if (!window.Seen) {
			// A channel's first packet opens its window wherever the far side
			// happens to have got to. It is judged against nothing, because
			// there is nothing yet to be older than, and it counts no loss -
			// a stream that opens at 5000 has not lost 5000 packets.
			window.Seen = true;
			window.Highest = sequence;
			window.Bits = 0;
			return;
		}

		if (Packet::IsNewer(sequence, window.Highest)) {
			const uint16_t shift = static_cast<uint16_t>(sequence - window.Highest);

			// A jump wider than the window means everything the window held is
			// now older than the 32 sequences before this one, so it is cleared
			// rather than shifted into nonsense.
			if (shift >= 32) {
				window.Bits = 0;
			} else {
				window.Bits = (window.Bits << shift) | (1u << (shift - 1));
			}

			// Anything between the old high-water mark and the new one that is
			// not in the window never arrived. Counted as lost here, which is
			// the only place both numbers are known.
			if (shift > 1) {
				Totals.PacketsLost += shift - 1;
			}
			window.Highest = sequence;
			return;
		}

		// Older than the high-water mark. Record it in the window when it is
		// still inside it, so a late-but-not-lost packet is not counted twice.
		const uint16_t behind = static_cast<uint16_t>(window.Highest - sequence);
		if (behind >= 1 && behind <= 32) {
			const uint32_t bit = 1u << (behind - 1);
			if ((window.Bits & bit) == 0) {
				window.Bits |= bit;
				if (Totals.PacketsLost > 0) {
					// It was counted lost when the gap opened. It arrived after
					// all, so the estimate is corrected rather than left to
					// overstate loss for the life of the connection.
					--Totals.PacketsLost;
				}
			}
		}
	}

	void Link::ObserveAcknowledgement(const PacketHeader &header) {
		// **The acknowledgement fields describe this end's reliable stream, and
		// that is what makes a loss signal available with nothing added to the
		// wire.** `ReliableReceiver::Acknowledging` rewrites them on every
		// outgoing packet whatever its channel, so what arrives here is the far
		// side saying which of the sequences `NextHeader` stamped it has.
		//
		// Judged three behind the newest acknowledgement, which is TCP's
		// duplicate-acknowledgement threshold and is here for its reason: a gap
		// that close to the front is far more often a reorder that is about to
		// resolve than a packet that is gone, and a controller that cut its rate
		// on every reorder would spend a routed path permanently backed off.
		constexpr uint16_t REORDER_THRESHOLD = 3;

		// **A hostile peer can acknowledge a sequence far ahead of anything
		// sent, and `IsNewer(next, ...)` is the whole answer to it.** The
		// frontier only ever moves over sequences this end actually stamped, so
		// the loop is bounded by what was sent rather than by what was claimed -
		// and a link that has stamped nothing has `next` equal to the frontier,
		// which is why there is no separate guard for that case.
		const uint16_t next = OutgoingSequence[ChannelSlot(ChannelKind::Reliable)];
		const uint16_t limit = static_cast<uint16_t>(header.Acknowledge - REORDER_THRESHOLD);

		uint32_t lost = 0;
		while (!Packet::IsNewer(JudgedSequence, limit) && Packet::IsNewer(next, JudgedSequence)) {
			const uint16_t behind = static_cast<uint16_t>(header.Acknowledge - JudgedSequence);

			// Outside the 32 the window covers is undecidable from this packet,
			// and it is undecidable from every later one too - the window only
			// moves further away. Counted lost, which is also what the far side
			// is telling the sender by never setting the bit.
			const bool acknowledged = behind <= 32 && (header.AcknowledgeBits & (1u << (behind - 1))) != 0;
			if (!acknowledged) {
				++lost;
			}
			++JudgedSequence;
		}

		if (lost > 0) {
			Totals.SendsLost += lost;
			Control.OnLoss(lost, LastKnownSeconds);
		}
	}

	bool Link::OnPacket(const PacketHeader &header, size_t payloadBytes, double nowSeconds) {
		ENGINE_PROFILE_CAT("Link::OnPacket", core::ProfileCategory::Network);

		if (Phase == ConnectionState::Disconnected) {
			return false;
		}

		// Anything arriving proves the far side is alive, including a packet
		// this call is about to refuse. The timeout is about the peer, not about
		// whether its last packet was useful.
		LastReceiveAt = nowSeconds;
		LastKnownSeconds = nowSeconds;
		Totals.SinceLastReceiveSeconds = 0.0f;
		++Totals.PacketsReceived;
		Totals.BytesReceived += payloadBytes;

		if (header.Channel == ChannelKind::Handshake) {
			// No window, and deliberately none. A handshake datagram is answered
			// before there is a link to number it, so its sequence belongs to no
			// stream - judging it against a mark, or letting it move one, would
			// be inventing a numbering the sender never used. It still proves
			// the peer is alive, which the lines above have already recorded.
			return true;
		}

		// Before the stale rule, because a stale packet's acknowledgement is not
		// stale: the payload is about a moment that has passed and the
		// acknowledgement is about what the far side has, which is the newest
		// thing it knows however old the rest of the packet is.
		ObserveAcknowledgement(header);

		ChannelWindow &window = Incoming[ChannelSlot(header.Channel)];

		// The stale rule: unreliable traffic only, against **this channel's own
		// high-water mark**. A reliable packet arriving late is a resend that
		// still has to be delivered in order - discarding it here would silently
		// drop an event the sender believes was acknowledged. And judging an
		// unreliable packet against the reliable channel's mark is the same
		// mistake from the other side: the two counters advance at completely
		// different rates, so a join that spends several reliable packets would
		// leave every unreliable one behind a number it never counted against.
		//
		// A repeat of the newest sequence is not *older* than what has been
		// seen, so it is not stale and is not counted as one.
		if (header.Channel == ChannelKind::Unreliable && window.Seen &&
			!Packet::IsNewer(header.Sequence, window.Highest) && header.Sequence != window.Highest) {
			++Totals.PacketsStale;
			Observe(window, header.Sequence);
			return false;
		}

		Observe(window, header.Sequence);
		return true;
	}

	bool Link::Reserve(size_t payloadBytes) {
		if (Phase != ConnectionState::Connected) {
			return false;
		}
		if (payloadBytes > Packet::MAXIMUM_MESSAGE_BYTES) {
			// A message that can never be sent, whatever the path does. Kept
			// apart from the two budget refusals below, which are a busy link.
			ENGINE_WARN_EVERY(
				1.0,
				"a {} byte message is past the {} byte limit and can never be sent",
				payloadBytes,
				Packet::MAXIMUM_MESSAGE_BYTES
			);
			// **The message limit, not the payload limit.** What a caller hands
			// over is sealed before it goes, so the tag is sixteen bytes of the
			// datagram that this number does not get to spend. Measuring against
			// the sealed size instead would pass a message here and fail it at
			// the framing, which is a message that can never be sent and is
			// indistinguishable at the call site from a busy link.
			//
			// Refused rather than fragmented. A fragmented datagram is lost
			// entirely when any one of its fragments is, which multiplies the
			// loss rate the unreliable channel is designed around.
			++Totals.SendsOverBudget;
			return false;
		}
		if (PacketsLeft == 0 || BytesLeft < payloadBytes) {
			// The number an enforced budget needs visible.
			// Without it an enforced budget and a congested link look identical
			// from a game's point of view.
			++Totals.SendsOverBudget;
			core::Metrics::Count("net.link.overbudget", 1.0);
			return false;
		}

		// **The configured budgets are asked first and the path second, so
		// `SendsOverBudget` still means exactly what it has always meant.** It
		// is what `D00007`'s reopen trigger is phrased against and what
		// `render`'s debug panel documents as not being congestion - and a
		// caller reading it wants to know whether its own numbers are the thing
		// turning traffic away, because that is the half it can fix by changing
		// them. Congestion is the other counter, and it is a fact about the path
		// rather than about this configuration.
		if (AllowanceLeft < payloadBytes) {
			++Totals.SendsOverAllowance;
			core::Metrics::Count("net.link.overallowance", 1.0);
			return false;
		}

		BytesLeft -= static_cast<uint32_t>(payloadBytes);
		AllowanceLeft -= static_cast<uint32_t>(payloadBytes);
		--PacketsLeft;
		return true;
	}

	PacketHeader Link::NextHeader(ChannelKind channel) {
		const ChannelWindow &window = Incoming[ChannelSlot(channel)];

		PacketHeader header;
		header.Channel = channel;
		header.Sequence = OutgoingSequence[ChannelSlot(channel)]++;

		if (channel == ChannelKind::Reliable && !ReliableStreamOpen) {
			// Where judging starts. Nothing before the stream's first sequence
			// was ever sent, so nothing before it can be missing - and a
			// frontier that started at zero on a stream opening elsewhere would
			// report every sequence below it as lost, which is the mirror of the
			// bug `ChannelWindow::Seen` exists to prevent.
			ReliableStreamOpen = true;
			JudgedSequence = header.Sequence;
			PeriodSequence = static_cast<uint16_t>(header.Sequence + 1);
		}

		// This channel's window, because there is one sequence space per channel
		// and one field to report a sequence in. A window shared across channels
		// would name a sequence the far side's counters cannot place.
		header.Acknowledge = window.Highest;
		header.AcknowledgeBits = window.Bits;
		return header;
	}

	void Link::ResetBudget() {
		BytesLeft = Paced.BytesPerTick;
		PacketsLeft = Paced.PacketsPerTick;

		// The controller decided this in `Advance`, which every caller runs
		// immediately before this one. Read rather than computed here so that
		// the allowance is a function of one tick's worth of observation and not
		// of where in the tick somebody happened to reset a budget.
		AllowanceLeft = std::min(Control.AllowanceBytes(), Paced.BytesPerTick);
		Totals.SendAllowanceBytes = AllowanceLeft;
	}

	void Link::RecordRoundTrip(double seconds, double varianceSeconds) {
		if (seconds < 0.0) {
			return;
		}

		Totals.RoundTripMilliseconds = static_cast<float>(seconds * 1000.0);
		if (varianceSeconds >= 0.0) {
			Totals.RoundTripVarianceMilliseconds = static_cast<float>(varianceSeconds * 1000.0);
		}

		Control.OnRoundTrip(seconds, varianceSeconds, LastKnownSeconds);
	}

	bool Link::NeedsKeepAlive(double nowSeconds) const {
		if (Phase != ConnectionState::Connected) {
			return false;
		}
		return nowSeconds - LastSendAt >= Paced.KeepAliveSeconds;
	}

	void Link::OnSent(size_t payloadBytes, double nowSeconds) {
		LastSendAt = nowSeconds;
		++Totals.PacketsSent;
		Totals.BytesSent += payloadBytes;
	}
}
