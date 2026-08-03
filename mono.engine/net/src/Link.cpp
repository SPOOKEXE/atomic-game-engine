#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Link.hpp>

#include <algorithm>

namespace engine::net {

	namespace {
		size_t ChannelSlot(ChannelKind channel) {
			// The enum's own order, and the array below is sized from the last
			// value rather than from a literal — a channel added without a slot
			// would index past the end, which is the one bug in this file that
			// would not show up as a wrong number.
			return static_cast<size_t>(channel);
		}
	}

	bool LinkSettings::IsValid() const {
		return HandshakeTimeoutSeconds > 0.0 && IdleTimeoutSeconds > 0.0 && KeepAliveSeconds > 0.0 &&
			   KeepAliveSeconds < IdleTimeoutSeconds && BytesPerTick > 0 && PacketsPerTick > 0;
	}

	Link::Link(ConnectionId id, double nowSeconds, const LinkSettings &settings)
		: Handle(id), Paced(settings.IsValid() ? settings : LinkSettings{}), OpenedAt(nowSeconds),
		  LastReceiveAt(nowSeconds), LastSendAt(nowSeconds) {
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
	}

	void Link::Advance(double nowSeconds) {
		if (Phase == ConnectionState::Disconnected) {
			return;
		}

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
			// there is nothing yet to be older than, and it counts no loss —
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

	bool Link::OnPacket(const PacketHeader &header, size_t payloadBytes, double nowSeconds) {
		ENGINE_PROFILE("Link::OnPacket");

		if (Phase == ConnectionState::Disconnected) {
			return false;
		}

		// Anything arriving proves the far side is alive, including a packet
		// this call is about to refuse. The timeout is about the peer, not about
		// whether its last packet was useful.
		LastReceiveAt = nowSeconds;
		Totals.SinceLastReceiveSeconds = 0.0f;
		++Totals.PacketsReceived;
		Totals.BytesReceived += payloadBytes;

		if (header.Channel == ChannelKind::Handshake) {
			// No window, and deliberately none. A handshake datagram is answered
			// before there is a link to number it, so its sequence belongs to no
			// stream — judging it against a mark, or letting it move one, would
			// be inventing a numbering the sender never used. It still proves
			// the peer is alive, which the lines above have already recorded.
			return true;
		}

		ChannelWindow &window = Incoming[ChannelSlot(header.Channel)];

		// The stale rule: unreliable traffic only, against **this channel's own
		// high-water mark**. A reliable packet arriving late is a resend that
		// still has to be delivered in order — discarding it here would silently
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
			// The number DATATYPES_LIBRARIES.md §15.1 asks to be visible.
			// Without it an enforced budget and a congested link look identical
			// from a game's point of view.
			++Totals.SendsOverBudget;
			core::Metrics::Count("net.link.overbudget", 1.0);
			return false;
		}

		BytesLeft -= static_cast<uint32_t>(payloadBytes);
		--PacketsLeft;
		return true;
	}

	PacketHeader Link::NextHeader(ChannelKind channel) {
		const ChannelWindow &window = Incoming[ChannelSlot(channel)];

		PacketHeader header;
		header.Channel = channel;
		header.Sequence = OutgoingSequence[ChannelSlot(channel)]++;

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
