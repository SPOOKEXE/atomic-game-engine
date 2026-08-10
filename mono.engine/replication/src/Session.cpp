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
			return net::ChannelKind::Unreliable;

		case MessageKind::SnapshotChunk:
		case MessageKind::Structure:
		case MessageKind::Input:
		case MessageKind::Applied:
		case MessageKind::User:
		case MessageKind::Identify:
			return net::ChannelKind::Reliable;
		}

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
			return false;
		}

		Sealer_ = std::move(keys.Sending);
		Opener_ = std::move(keys.Receiving);
		return true;
	}

	bool Session::Transmit(net::PacketHeader header, std::span<const std::byte> payload) {
		if (!Sealer_.has_value()) {
			return false;
		}

		header.Counter = Sealer_->NextCounter();

		Framing.Clear();
		if (!net::Packet::WriteHeader(Framing, header, payload.size() + net::Cipher::OVERHEAD_BYTES)) {
			return false;
		}

		const std::optional<net::Cipher::Sealed> sealed =
			Sealer_->Seal(payload, Framing.Bytes().last(net::Packet::HEADER_BYTES));
		if (!sealed.has_value() || sealed->Counter != header.Counter) {
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
			return false;
		}

		if (!Link_.Reserve(payload.size())) {
			return false;
		}

		net::PacketHeader header = Link_.NextHeader(channel);

		header = Receiver.Acknowledging(header);

		if (!Transmit(header, payload)) {
			return false;
		}

		Link_.OnSent(payload.size(), nowSeconds);

		if (channel == net::ChannelKind::Reliable) {
			Sender.Track(header.Sequence, payload, nowSeconds);
		}
		return true;
	}

	bool Session::Send(std::span<const std::byte> message, double nowSeconds) {
		core::ByteReader reader(message);
		Message read;
		if (!ReadMessage(reader, read)) {
			return false;
		}

		const net::ChannelKind channel = ChannelFor(read.Kind);
		if (channel == net::ChannelKind::Reliable && !Sender.HasRoom()) {
			return false;
		}

		return Emit(channel, message, nowSeconds);
	}

	size_t Session::Flush(double nowSeconds) {
		ENGINE_PROFILE_CAT("replica.flush", core::ProfileCategory::Network);

		size_t sent = 0;

		for (const net::ReliableSender::Unacknowledged &waiting : Sender.Due(nowSeconds)) {
			if (!Link_.Reserve(waiting.Payload.size())) {
				continue;
			}

			net::PacketHeader header;
			header.Channel = net::ChannelKind::Reliable;
			header.Sequence = waiting.Sequence;
			header = Receiver.Acknowledging(header);

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
			Stats_.Refused++;
			return false;
		}

		if (!Opener_.has_value()) {
			Stats_.Unopened++;
			return false;
		}

		const std::optional<std::span<const std::byte>> plain =
			Opener_->Open(packet->Header.Counter, packet->Payload, packet->HeaderBytes);
		if (!plain.has_value()) {
			Stats_.Unopened++;
			return false;
		}

		if (!Link_.OnPacket(packet->Header, plain->size(), nowSeconds)) {
			return false;
		}

		Sender.OnAcknowledge(packet->Header, nowSeconds);

		Link_.RecordRoundTrip(Sender.SmoothedRoundTripSeconds());

		if (packet->Header.Channel == net::ChannelKind::Unreliable) {
			Inbound_.emplace_back(plain->begin(), plain->end());
			return true;
		}

		if (!Receiver.Accept(packet->Header.Sequence, *plain)) {
			return false;
		}

		for (const net::ReliableReceiver::Delivery &delivery : Receiver.Drain()) {
			Inbound_.push_back(delivery.Payload);
		}
		return true;
	}
}
