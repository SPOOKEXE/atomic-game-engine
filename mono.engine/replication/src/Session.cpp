#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Session.hpp>

#include <algorithm>
#include <utility>

namespace engine::replication {

	net::ChannelKind ChannelFor(MessageKind kind) {
		switch (kind) {
		case MessageKind::Delta:
			// The one thing whose loss is covered. The next delta is already on
			// its way and is more correct than the one being waited for, so
			// resending a stale one costs bandwidth to deliver something the
			// receiver would then discard.
			return net::ChannelKind::Unreliable;

		case MessageKind::SnapshotChunk:
		case MessageKind::Structure:
		case MessageKind::Input:
		case MessageKind::Applied:
		case MessageKind::Identify:
			// A lost chunk is a client that never joins. A lost input is a jump
			// that never happened. A lost acknowledgement stalls the stream.
			// None of them is covered by the next message.
			//
			// **And a lost structural change is the worst of the four**, because
			// nothing above notices it: the server's known set says the client
			// has been told, so the entity is never mentioned again and the
			// client goes on acknowledging happily while missing it. A tick
			// acknowledgement cannot repair that — it says which tick was
			// applied, not which message — so the redelivery has to come from
			// the one thing here that counts messages, which is this channel.
			// **And a lost identity claim is a client that never gets in**, on a
			// server that requires one: nothing else carries it, and the client
			// has no way to know it was dropped rather than rejected.
			//
			// `docs/DEFERRED.md` D00011.
			return net::ChannelKind::Reliable;
		}

		// Unreachable while every kind is handled above, and reliable is the
		// safe answer if one is ever added and missed here.
		return net::ChannelKind::Reliable;
	}

	Session::Session(
		net::Transport &transport,
		const net::Endpoint &peer,
		net::ConnectionId id,
		double nowSeconds,
		const SessionSettings &settings
	)
		: Transport_(&transport), Peer_(peer), Link_(id, nowSeconds, settings.Link),
		  Sender(settings.Reliability), Receiver(settings.Reliability) {}

	void Session::ClearInbound() {
		Inbound_.clear();
	}

	bool Session::AdoptKeys(net::Handshake::Session keys) {
		if (Sealer_.has_value() || Opener_.has_value()) {
			// Refused rather than replaced. A second set of keys on a live
			// connection is a replay or two code paths both believing they own
			// the transition, and swapping a sealer out is the one operation
			// here that could start a counter over. The ciphers passed in die
			// on the way out of this call and wipe themselves.
			return false;
		}

		Sealer_ = std::move(keys.Sending);
		Opener_ = std::move(keys.Receiving);
		return true;
	}

	bool Session::Transmit(net::PacketHeader header, std::span<const std::byte> payload) {
		if (!Sealer_.has_value()) {
			// **Fail closed, and this is the only place that decides it.** A
			// session with no keys is one whose admission did not finish, and
			// putting its payload on the wire in the clear rather than not at
			// all is the downgrade doing itself. Both send paths come through
			// here, so neither carries its own copy of this decision.
			return false;
		}

		// **The counter is written into the header before the frame is sealed,
		// because the header is what the frame is sealed over.** Reading the
		// next counter does not spend it; the seal below is what does.
		header.Counter = Sealer_->NextCounter();

		Framing.Clear();
		if (!net::Packet::WriteHeader(Framing, header, payload.size() + net::Cipher::OVERHEAD_BYTES)) {
			return false;
		}

		// The whole header as associated data, so a rewritten channel, sequence,
		// acknowledgement or counter fails the tag instead of being acted on.
		const std::optional<net::Cipher::Sealed> sealed =
			Sealer_->Seal(payload, Framing.Bytes().last(net::Packet::HEADER_BYTES));
		if (!sealed.has_value() || sealed->Counter != header.Counter) {
			// An exhausted sealer, or a header stamped with a counter the seal
			// did not use. The second is checked rather than assumed because the
			// packet it would produce cannot be opened by anybody and would read
			// on the far side as ordinary packet loss.
			return false;
		}

		Framing.WriteRaw(sealed->Bytes.data(), sealed->Bytes.size());

		if (Transport_->Send(Peer_, Framing.Bytes()) != net::TransportStatus::Ok) {
			Stats_.Undeliverable++;
			return false;
		}

		Stats_.Sent++;
		return true;
	}

	bool Session::Emit(net::ChannelKind channel, std::span<const std::byte> payload, double nowSeconds) {
		if (channel == net::ChannelKind::Handshake) {
			// Never from here. The exchange that produces the keys is framed by
			// `Admission.hpp` straight to the transport, and a payload on that
			// channel leaving a session would be one nothing sealed.
			return false;
		}

		// Asked and booked in one call. A separate "may I" and "I did" is two
		// calls a caller can get out of step, and the one that gets forgotten
		// is the second.
		if (!Link_.Reserve(payload.size())) {
			return false;
		}

		net::PacketHeader header = Link_.NextHeader(channel);

		// The reliable stream's own acknowledgement, written over the link's.
		// `Link`'s windows are per channel, so the header it stamps carries the
		// acknowledgement of whichever channel this packet is going on — and
		// this stream is mostly unreliable deltas, so a reliable payload would
		// almost never be acknowledged. The ack that retires one has to ride
		// every packet, and only the receiver tracking them can put it there.
		header = Receiver.Acknowledging(header);

		// Refused here when this session holds no keys — see `Transmit`, which
		// is where both send paths make that decision.
		if (!Transmit(header, payload)) {
			return false;
		}

		Link_.OnSent(payload.size(), nowSeconds);

		if (channel == net::ChannelKind::Reliable) {
			// Tracked after it has actually gone, and tracked as **plaintext**.
			// Tracking first and failing to send would hold a payload the peer
			// will never acknowledge, and the resend would be the only thing
			// that ever delivered it.
			Sender.Track(header.Sequence, payload, nowSeconds);
		}
		return true;
	}

	bool Session::Send(std::span<const std::byte> message, double nowSeconds) {
		// The kind decides the channel, and the kind is in the message. Reading
		// it here rather than taking it as a parameter is what stops a caller
		// making everything reliable.
		core::ByteReader reader(message);
		Message read;
		if (!ReadMessage(reader, read)) {
			return false;
		}

		const net::ChannelKind channel = ChannelFor(read.Kind);
		if (channel == net::ChannelKind::Reliable && !Sender.HasRoom()) {
			// Backpressure, not an ending. The caller keeps the message and
			// offers it again; the window opens as acknowledgements arrive.
			return false;
		}

		return Emit(channel, message, nowSeconds);
	}

	size_t Session::Flush(double nowSeconds) {
		// The send half, and the one that scales with what is going wrong: a
		// resend is work this frame for a packet an earlier frame already
		// paid for, so a link losing packets shows up here as time and not
		// only as a counter on the F4 panel.
		ENGINE_PROFILE_CAT("replica.flush", core::ProfileCategory::Network);

		size_t sent = 0;

		for (const net::ReliableSender::Unacknowledged &waiting : Sender.Due(nowSeconds)) {
			// A resend reuses its original sequence: a fresh one would leave a
			// hole the receiver's ordering waits on forever.
			if (!Link_.Reserve(waiting.Payload.size())) {
				// Over budget. Its clock is not restarted, so it comes back
				// from the next `Due` — and the refusal is already counted in
				// the link's own `SendsOverBudget`.
				continue;
			}

			net::PacketHeader header;
			header.Channel = net::ChannelKind::Reliable;
			header.Sequence = waiting.Sequence;
			header = Receiver.Acknowledging(header);

			// **A resend is sealed again, under a fresh counter, rather than
			// replayed as the bytes that went the first time.** Both are safe
			// against a repeated nonce — the counter only ever moves forward,
			// and a verbatim replay is the same frame rather than a second one
			// — and they fail differently, which is what decides it. The header
			// is the associated data and it carries a live acknowledgement, so a
			// verbatim replay would have to either freeze the acknowledgement
			// this packet is also carrying, on the one packet a stalled stream
			// most needs to be current, or stop covering the mutable fields with
			// the tag and let anybody on the path rewrite them. Re-sealing keeps
			// both, and costs one ChaCha pass over a payload that is resent at
			// most `ReliabilitySettings::MaximumResends` times.
			//
			// The receiver is what stops a resend being applied twice, exactly
			// as it did before: `ReliableReceiver` deduplicates by sequence, and
			// the sequence is unchanged. `Cipher` deliberately refuses a forgery
			// and not a replay, because this layer is the one that can tell the
			// difference between an attack and a resend.
			if (!Transmit(header, waiting.Payload)) {
				continue;
			}

			Sender.OnResent(waiting.Sequence, nowSeconds);
			Link_.OnSent(waiting.Payload.size(), nowSeconds);
			Stats_.Retransmissions++;
			sent++;
		}

		return sent;
	}

	bool Session::Receive(std::span<const std::byte> datagram, double nowSeconds) {
		core::ByteReader reader(datagram);

		const std::optional<net::Packet::Inbound> packet = net::Packet::Read(reader);
		if (!packet.has_value()) {
			Stats_.Refused++;
			return false;
		}

		if (packet->Header.Channel == net::ChannelKind::Handshake) {
			// **Refused before `OnPacket`, which is the point.** A session
			// exists because an admission already succeeded, so a second key
			// exchange arriving on it is a replay or somebody at this address
			// trying to take the connection from under whoever holds it —
			// and letting it reach the link would let that somebody reset the
			// idle timeout of a connection they do not own. The listener
			// answers the one legitimate case, a `Welcome` that went missing,
			// before a datagram ever gets here.
			Stats_.Refused++;
			return false;
		}

		if (!Opener_.has_value()) {
			// **Fail closed, and this is the whole of the downgrade defence on
			// the receiving side.** There is no field on the wire that says
			// whether a packet is sealed, so a peer cannot ask for the plaintext
			// path; it can only send bytes that do not open. A session with no
			// keys cannot check anything, and accepting bytes *because* it
			// cannot check them is the same as having no encryption.
			Stats_.Unopened++;
			return false;
		}

		// **Opened before the link is told anything at all.** A packet that does
		// not authenticate must not move the sequence window, retire a reliable
		// payload or reset the idle timeout — all three are things somebody who
		// can write to this address could otherwise do to a connection they hold
		// no key for. Nothing below this line runs for a packet that failed.
		const std::optional<std::span<const std::byte>> plain =
			Opener_->Open(packet->Header.Counter, packet->Payload, packet->HeaderBytes);
		if (!plain.has_value()) {
			// A forged tag, a rewritten header, a wrong counter, a packet from
			// somebody else's session, or a peer sending in the clear. One
			// answer for all of them, deliberately: which check failed is
			// information about the key.
			Stats_.Unopened++;
			return false;
		}

		if (!Link_.OnPacket(packet->Header, plain->size(), nowSeconds)) {
			// Stale, or a link that is not accepting. Counted rather than
			// treated as malformed: an unreliable transport reorders, and a
			// packet arriving after a newer one is the normal case.
			return false;
		}

		// Whatever this packet acknowledged, in either direction.
		Sender.OnAcknowledge(packet->Header, nowSeconds);

		// **The one place both halves of a round trip are in scope.** The
		// sender holds what went out and when; the link holds the statistics a
		// game reads. Neither can do this alone, so the session — which owns
		// both — carries the number across.
		Link_.RecordRoundTrip(Sender.SmoothedRoundTripSeconds());

		if (packet->Header.Channel == net::ChannelKind::Unreliable) {
			// `Link::OnPacket` already discarded anything older than what has
			// been seen, which is the whole reason a sequence rides every
			// packet.
			Inbound_.emplace_back(plain->begin(), plain->end());
			return true;
		}

		if (!Receiver.Accept(packet->Header.Sequence, *plain)) {
			// A duplicate, or the out-of-order queue is full. Neither is
			// malformed and neither is delivered twice.
			return false;
		}

		// In order, with a gap held until it is filled.
		for (const net::ReliableReceiver::Delivery &delivery : Receiver.Drain()) {
			Inbound_.push_back(delivery.Payload);
		}
		return true;
	}
}
