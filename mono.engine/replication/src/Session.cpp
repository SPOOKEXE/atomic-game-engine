#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Session.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::replication {

	net::ChannelKind ChannelFor(MessageKind kind) {
		switch (kind) {
		case MessageKind::Delta:

		// **Unreliable, and that is the cadence argument on the wire.** An
		// audit that is lost costs one rotation of a sweep whose whole premise
		// is that what it finds is not urgent, and an answer that is lost costs
		// the same. Putting either on the reliable channel would spend the
		// window that structural changes depend on, for a message whose
		// resend is never worth more than the next one.
		case MessageKind::GroupSignatures:
		case MessageKind::Disputed:
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

	void Session::SetSimulatedLatency(double milliseconds) {
		constexpr double MAXIMUM_MILLISECONDS = 60'000.0;
		const double finite = std::isfinite(milliseconds) ? milliseconds : 0.0;
		SimulatedLatencySeconds = std::clamp(finite, 0.0, MAXIMUM_MILLISECONDS) / 1000.0;
	}

	bool Session::Transmit(net::PacketHeader header, std::span<const std::byte> payload, double nowSeconds) {
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

		if (SimulatedLatencySeconds > 0.0) {
			Delayed.push_back(
				DelayedDatagram{
					nowSeconds + SimulatedLatencySeconds,
					std::vector<std::byte>(Framing.Bytes().begin(), Framing.Bytes().end()),
				}
			);
			Stats_.Sent++;
			return true;
		}

		if (Transport_->Send(Peer_, Framing.Bytes()) != net::TransportStatus::Ok) {
			Stats_.Undeliverable++;
			return false;
		}

		Stats_.Sent++;
		return true;
	}

	size_t Session::FlushDelayed(double nowSeconds) {
		size_t sent = 0;
		while (!Delayed.empty() && Delayed.front().ReadyAtSeconds <= nowSeconds) {
			DelayedDatagram datagram = std::move(Delayed.front());
			Delayed.pop_front();
			if (Transport_->Send(Peer_, datagram.Bytes) != net::TransportStatus::Ok) {
				Stats_.Undeliverable++;
				continue;
			}
			sent++;
		}
		return sent;
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

		if (!Transmit(header, payload, nowSeconds)) {
			return false;
		}

		Link_.OnSent(payload.size(), nowSeconds);

		// Whatever went out carried `Receiver.Acknowledging`, so nothing is
		// owed any more.
		Owed = false;

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

		size_t sent = FlushDelayed(nowSeconds);

		for (const net::ReliableSender::Unacknowledged &waiting : Sender.Due(nowSeconds)) {
			if (!Link_.Reserve(waiting.Payload.size())) {
				continue;
			}

			net::PacketHeader header;
			header.Channel = net::ChannelKind::Reliable;
			header.Sequence = waiting.Sequence;
			header = Receiver.Acknowledging(header);

			if (!Transmit(header, waiting.Payload, nowSeconds)) {
				continue;
			}

			Sender.OnResent(waiting.Sequence, nowSeconds);
			Link_.OnSent(waiting.Payload.size(), nowSeconds);
			Stats_.Retransmissions++;
			sent++;
		}

		// **A packet carrying only an acknowledgement, when nothing else is
		// going.** `net::Link` has described this since v0.3 - "a connection
		// with no traffic is indistinguishable from a dead one" - and until
		// v0.13 nothing in this repository called `NeedsKeepAlive`, because
		// every caller published a world every tick and so never went quiet.
		//
		// The first caller that does go quiet is the studio's edit stream, and
		// what it exposed is worse than a missed timeout: an acknowledgement
		// rides on an outgoing packet, so a link with nothing to say never
		// acknowledges, the far side's reliable window fills, its payloads are
		// resent to the resend limit, and a link that is working perfectly
		// gives up.
		//
		// Unreliable, because it carries nothing that has to arrive. It is the
		// header that matters.
		// **A packet carrying only an acknowledgement**, when either the far
		// side is waiting for one or the link has gone quiet enough to look
		// dead.
		//
		// `net::Link` has described the second half since v0.3 - "a connection
		// with no traffic is indistinguishable from a dead one" - and until
		// v0.13 nothing in this repository called `NeedsKeepAlive`, because
		// every caller published a world every tick and so never went quiet.
		//
		// **The first half is the one that bites, and the keep-alive timer
		// alone does not cover it.** An acknowledgement rides on an outgoing
		// packet, so a receiver with nothing to say never acknowledges. The
		// sender resends on its own timer and gives up at
		// `MaximumResends` - which it reaches *before* a one-second keep-alive
		// comes round. The far side then has a `ReliableSender` that refuses
		// everything, on a link that is up and healthy, and the only symptom is
		// that sends start returning false.
		//
		// So a reliable payload that has been accepted and not yet acknowledged
		// makes an acknowledgement due immediately. That costs one small
		// datagram per reliable message on an otherwise silent link, and buys
		// the property the whole reliable channel depends on.
		if (sent == 0 && (Owed || Link_.NeedsKeepAlive(nowSeconds)) && Sealer_.has_value()) {
			if (Link_.Reserve(0)) {

				net::PacketHeader header = Link_.NextHeader(net::ChannelKind::Unreliable);
				header = Receiver.Acknowledging(header);
				if (Transmit(header, {}, nowSeconds)) {
					Link_.OnSent(0, nowSeconds);
					Stats_.KeepAlives++;
					Owed = false;
				}
			}
		}

		return sent + FlushDelayed(nowSeconds);
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

		// **The variance goes with the estimate, and leaving it out is not a
		// smaller version of passing it.** The congestion controller reads this
		// as its delay signal and sizes its noise threshold from RTTVAR; with
		// nothing to size it from the threshold falls back to a one-millisecond
		// floor, and a jittery wireless path then reads as a standing queue.
		// The link narrows for a queue that is not there, on exactly the
		// connections least able to spare it.
		Link_.RecordRoundTrip(Sender.SmoothedRoundTripSeconds(), Sender.RoundTripVarianceSeconds());

		if (packet->Header.Channel == net::ChannelKind::Unreliable) {
			// **An empty payload is an acknowledgement and not a message.**
			// That is what `Flush` sends on a quiet link, and surfacing it as
			// inbound would hand every reader a zero-length message it cannot
			// parse - which each of them would then count as a refusal, on a
			// link that is working exactly as designed.
			//
			// No legitimate message is empty: `WriteMessage` always writes a
			// version and a kind.
			if (!plain->empty()) {
				Inbound_.emplace_back(plain->begin(), plain->end());
			}
			return true;
		}

		// The far side is now waiting to hear that this arrived, and nothing
		// else may be going out for a while.
		Owed = true;

		if (!Receiver.Accept(packet->Header.Sequence, *plain)) {
			return false;
		}

		for (const net::ReliableReceiver::Delivery &delivery : Receiver.Drain()) {
			Inbound_.push_back(delivery.Payload);
		}
		return true;
	}
}
