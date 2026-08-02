#include <engine/core/Log.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Session.hpp>

#include <algorithm>

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
		case MessageKind::Forget:
		case MessageKind::Input:
		case MessageKind::Applied:
			// A lost chunk is a client that never joins. A lost forget is an
			// entity a client draws forever. A lost input is a jump that never
			// happened. A lost acknowledgement stalls the stream. None of them
			// is covered by the next message.
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

	bool Session::Emit(net::ChannelKind channel, std::span<const std::byte> payload, double nowSeconds) {
		// Asked and booked in one call. A separate "may I" and "I did" is two
		// calls a caller can get out of step, and the one that gets forgotten
		// is the second.
		if (!Link_.Reserve(payload.size())) {
			return false;
		}

		net::PacketHeader header = Link_.NextHeader(channel);

		// The reliable stream's own acknowledgement, written over the link's.
		// `Link` keeps one window across both channels, and the unreliable
		// counter runs so much faster that a reliable sequence falls outside it
		// within seconds — so the ack that can retire a reliable payload has to
		// come from the receiver that is tracking them.
		header = Receiver.Acknowledging(header);

		core::ByteWriter writer;
		if (!net::Packet::Write(writer, header, payload)) {
			return false;
		}

		const net::TransportStatus status = Transport_->Send(Peer_, writer.Bytes());
		if (status != net::TransportStatus::Ok) {
			Stats_.Undeliverable++;
			return false;
		}

		Stats_.Sent++;
		Link_.OnSent(payload.size(), nowSeconds);

		if (channel == net::ChannelKind::Reliable) {
			// Tracked after it has actually gone. Tracking first and failing to
			// send would hold a payload the peer will never acknowledge, and
			// the resend would be the only thing that ever delivered it.
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

			core::ByteWriter writer;
			if (!net::Packet::Write(writer, header, waiting.Payload)) {
				continue;
			}

			if (Transport_->Send(Peer_, writer.Bytes()) != net::TransportStatus::Ok) {
				Stats_.Undeliverable++;
				continue;
			}

			Sender.OnResent(waiting.Sequence, nowSeconds);
			Link_.OnSent(waiting.Payload.size(), nowSeconds);
			Stats_.Retransmissions++;
			Stats_.Sent++;
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

		if (!Link_.OnPacket(packet->Header, packet->Payload.size(), nowSeconds)) {
			// Stale, or a link that is not accepting. Counted rather than
			// treated as malformed: an unreliable transport reorders, and a
			// packet arriving after a newer one is the normal case.
			return false;
		}

		// Whatever this packet acknowledged, in either direction.
		Sender.OnAcknowledge(packet->Header);

		if (packet->Header.Channel == net::ChannelKind::Unreliable) {
			// `Link::OnPacket` already discarded anything older than what has
			// been seen, which is the whole reason a sequence rides every
			// packet.
			Inbound_.emplace_back(packet->Payload.begin(), packet->Payload.end());
			return true;
		}

		if (!Receiver.Accept(packet->Header.Sequence, packet->Payload)) {
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
