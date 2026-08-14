#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Connector.hpp>

#include <algorithm>
#include <utility>

namespace engine::replication {

	Connector::Connector(
		net::Transport &transport,
		const net::Endpoint &server,
		double nowSeconds,
		const ConnectorSettings &settings
	)
		: Transport_(&transport),
		  Wire(transport, server, net::ConnectionId{1, 1}, nowSeconds, settings.Session),
		  Prediction_(settings.Prediction), Settings(settings) {
		if (!Settings.ServerIdentity.has_value()) {
			ENGINE_WARN(
				"replication: no server identity pinned - the exchange is encrypted but "
				"authenticates nobody, so a relay in the path can read everything."
			);
		}

		Exchange = net::Handshake::Begin(net::HandshakeRole::Initiator);
		if (!Exchange.has_value()) {
			ENGINE_ERROR("replication: no operating system entropy for a key exchange, so nothing connects.");
			Refuse();
			return;
		}

		const std::span<const std::byte> mine = Exchange->Message();
		std::copy(mine.begin(), mine.end(), Mine.begin());
	}

	void
	Connector::SetForeign(std::function<bool(std::span<const std::byte>, const net::Endpoint &)> handler) {
		Foreign = std::move(handler);
	}

	void Connector::OnUserMessage(std::function<void(std::span<const std::byte>)> handler) {
		UserMessages = std::move(handler);
	}

	bool Connector::SendUser(std::span<const std::byte> message, double nowSeconds) {
		if (Phase != Stage::Admitted) {
			// No session to carry it on. Refused rather than queued: an outbox
			// here would hold payloads whose meaning this module is not allowed
			// to understand, which is the same reason `net` keeps none.
			return false;
		}

		core::ByteWriter writer;
		User payload;
		payload.Bytes.assign(message.begin(), message.end());
		WriteMessage(writer, payload);
		return Wire.Send(writer.Bytes(), nowSeconds);
	}

	void Connector::Refuse() {
		Phase = Stage::Refused;
		Exchange.reset();
		Wire.Link().Close(net::DisconnectReason::HandshakeFailed);
	}

	void Connector::Repeat(double nowSeconds) {
		if (Phase == Stage::Refused || Phase == Stage::Admitted) {
			return;
		}
		if (Wire.Link().State() != net::ConnectionState::Connecting) {
			Phase = Stage::Refused;
			Exchange.reset();
			return;
		}
		if (Spoken && nowSeconds - SpokeAt < Settings.RepeatEverySeconds) {
			return;
		}

		Spoken = true;
		SpokeAt = nowSeconds;

		core::ByteWriter body;
		if (Phase == Stage::Greeting) {
			Hello hello;
			hello.PublicKey = Mine;
			WriteAdmission(body, hello);
		} else {
			Answer answer;
			answer.PublicKey = Mine;
			answer.Cookie = Cookie_;
			WriteAdmission(body, answer);
		}

		core::ByteWriter datagram;
		if (!FrameAdmission(datagram, body.Bytes())) {
			return;
		}

		Transport_->Send(Wire.Peer(), datagram.Bytes());
	}

	void Connector::Consume(std::span<const std::byte> datagram, double nowSeconds) {
		if (Phase == Stage::Admitted || Phase == Stage::Refused) {
			Stats_.Refused++;
			return;
		}

		core::ByteReader reader(datagram);
		const std::optional<net::Packet::Inbound> packet = net::Packet::Read(reader);
		if (!packet.has_value()) {
			Stats_.Refused++;
			return;
		}

		core::ByteReader body(packet->Payload);
		Admission message;
		if (!ReadAdmission(body, message)) {
			Stats_.Refused++;
			return;
		}

		switch (message.Kind) {
		case AdmissionKind::Challenge:
			if (Phase != Stage::Greeting) {
				Stats_.Refused++;
				return;
			}

			Cookie_ = message.Challenge.Cookie;
			Phase = Stage::Answering;

			Spoken = false;
			Repeat(nowSeconds);
			return;

		case AdmissionKind::Welcome: {
			if (Phase != Stage::Answering || !Exchange.has_value()) {
				Stats_.Refused++;
				return;
			}

			if (!Exchange->Consume(message.Welcome.PublicKey)) {
				Stats_.Refused++;
				Refuse();
				return;
			}

			std::optional<net::Handshake::Session> keys = Exchange->TakeKeys();
			if (!keys.has_value()) {
				Stats_.Refused++;
				Refuse();
				return;
			}

			const auto transcript = AdmissionTranscript(Mine, message.Welcome.PublicKey, Cookie_);
			if (!keys->Receiving.Open(message.Welcome.Counter, message.Welcome.Confirmation, transcript)
					 .has_value()) {
				ENGINE_ERROR("replication: the server's key exchange did not verify, so nothing connects.");
				Stats_.Refused++;
				Refuse();
				return;
			}

			if (Settings.ServerIdentity.has_value()) {
				assets::SignatureBytes signature;
				for (size_t index = 0; index < signature.Value.size(); index++) {
					signature.Value[index] = static_cast<uint8_t>(message.Welcome.Identity[index]);
				}

				if (!assets::VerifySessionTranscript(transcript, signature, *Settings.ServerIdentity)) {
					ENGINE_ERROR(
						"replication: this is not the server that was pinned - refusing to connect. "
						"An unsigned or wrongly signed welcome is what a relay in the path looks like."
					);
					Stats_.Refused++;
					Refuse();
					return;
				}
			}

			if (!Wire.AdoptKeys(std::move(*keys))) {
				ENGINE_ERROR("replication: the session already held keys, so nothing connects.");
				Stats_.Refused++;
				Refuse();
				return;
			}

			Phase = Stage::Admitted;
			Exchange.reset();
			Wire.Link().CompleteHandshake(nowSeconds);

			if (Settings.ClientIdentity != nullptr) {
				Identify claim;
				claim.Key = Settings.ClientIdentity->Public();
				claim.Signature = Settings.ClientIdentity->SignSessionTranscript(transcript);

				core::ByteWriter writer;
				WriteMessage(writer, claim);
				if (!Wire.Send(writer.Bytes(), nowSeconds)) {
					ENGINE_WARN("replication: the identity claim did not fit - the server may refuse us.");
				}
			}
			return;
		}

		case AdmissionKind::Hello:
		case AdmissionKind::Answer:
			Stats_.Refused++;
			return;
		}

		Stats_.Refused++;
	}

	void Connector::Poll(ecs::Store &store, double nowSeconds) {
		ENGINE_PROFILE_CAT("replica.poll", core::ProfileCategory::Network);

		if (!store.AdoptOnly()) {
			store.SetAdoptOnly(true);
		}

		for (;;) {
			const net::Transport::Inbound inbound = Transport_->Receive(Datagram);
			if (inbound.Status != net::TransportStatus::Ok) {
				break;
			}

			// Before the source check: a rendezvous message arrives from a
			// coordination point rather than from the server, and would
			// otherwise be counted as a refusal.
			if (Foreign && Foreign(Datagram, inbound.From)) {
				continue;
			}

			if (!(inbound.From == Wire.Peer())) {
				Stats_.Refused++;
				continue;
			}

			if (net::Packet::PeekChannel(Datagram) == net::ChannelKind::Handshake) {
				Consume(Datagram, nowSeconds);
				continue;
			}

			if (Phase != Stage::Admitted) {
				Stats_.Refused++;
				continue;
			}

			Wire.Receive(Datagram, nowSeconds);
		}

		if (Phase != Stage::Admitted) {
			Repeat(nowSeconds);
			return;
		}

		for (const std::vector<std::byte> &message : Wire.Inbound()) {
			// Routed before the replica sees it, for `Listener::Poll`'s reason:
			// a user message is not this module's, and handing one to the
			// replica would count a failure for a message that arrived
			// perfectly well.
			if (PeekMessageKind(message) == MessageKind::User) {
				if (UserMessages) {
					core::ByteReader reader(message);
					Message read;
					if (ReadMessage(reader, read)) {
						UserMessages(read.User.Bytes);
					}
				}
				continue;
			}

			const ApplyStatus status = Replica_.Receive(store, message);
			if (status == ApplyStatus::Ok) {
				Stats_.Applied++;
			} else {
				Stats_.Refused++;
			}
		}
		Wire.ClearInbound();

		std::vector<std::byte> acknowledgement = Replica_.Acknowledge();

		if (acknowledgement.empty()) {
			core::ByteWriter writer;
			WriteMessage(writer, replication::Applied{0});
			acknowledgement.assign(writer.Bytes().begin(), writer.Bytes().end());
		}

		Wire.Send(acknowledgement, nowSeconds);

		// After the acknowledgement, because it is the message the server needs
		// and this is the message it can wait a tick for.
		if (const std::vector<std::byte> dispute = Replica_.Dispute(); !dispute.empty()) {
			Wire.Send(dispute, nowSeconds);
		}

		Prediction_.Reconcile(Replica_.Applied());

		Wire.Flush(nowSeconds);
	}

	void Connector::Advance(double nowSeconds) {
		Wire.Link().Advance(nowSeconds);
		Wire.Link().ResetBudget();
	}

	bool Connector::Submit(uint64_t tick, std::span<const std::byte> bytes, double nowSeconds) {
		Prediction_.Record(tick, bytes);

		Input input;
		input.Tick = tick;
		input.Bytes.assign(bytes.begin(), bytes.end());

		core::ByteWriter writer;
		WriteMessage(writer, input);
		return Wire.Send(writer.Bytes(), nowSeconds);
	}

	bool Connector::SubmitState(const Delta &delta, double nowSeconds) {
		// **The same message the server sends, going the other way**, which is
		// the whole reason this is four lines: a delta is a delta, and the
		// direction is carried by which end is reading it rather than by the
		// format. What differs is that the server checks the sender's right to
		// say it - see `Authority::SetOwnership` - and the client does not,
		// because the server has no right to check.
		core::ByteWriter writer;
		WriteMessage(writer, delta);
		return Wire.Send(writer.Bytes(), nowSeconds);
	}
}
